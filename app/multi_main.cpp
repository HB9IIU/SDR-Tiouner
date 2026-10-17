#include "version.h"
#include "multi_web.h"
#include "auto_monitor.h"
#include "multi_udp.h"
#include "multi_record.h"
#include "update_check.h"
#include "multi_load.h"
#include "browser_stream.h"
#include "dect2/open_url.h"
#include "dect2/platform.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;
namespace {
std::atomic<bool> quit{false};
void interrupted(int) { quit = true; }
struct Settings {
    MultiConfig capture;
    int port = 47321, videoBase = 18900, rtspBase = 18560, iceBase = 18190;
    // the untouched TS of every running channel over UDP (host:base+id-1), for VLC / ffplay / OBS / recording
    bool udpOutput = true;
    bool checkUpdates = true;   // ask GitHub, a few seconds after the start, whether a newer release is out
    std::string udpHost = "127.0.0.1";
    int udpBase = 19000;
    // The frequency correction measured on the beacon, per radio (its serial): what that radio plus the LNB are off by
    struct Calibration { std::string key; double correctionHz = 0; long long when = 0; };
    std::vector<Calibration> calibrations;
    double legacyCorrection = 0;   // correction_hz of the file: for a radio that was never calibrated
    std::string lastRadio;         // the radio (as listed) of the last capture: selected again at the next start
    double gain = 30;
    bool automaticGain = true;
    std::vector<ChannelConfig> channels{{10491.5,1500000,0,false},{10492.75,500000,0,false},{10493.5,333000,0,false},{10494.0,333000,0,false}};
};
std::string trim(std::string value) {
    auto first = value.find_first_not_of(" \t\r\n"); if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
}
double number(const std::string& value) {
    size_t end = 0; double result = std::stod(value, &end);
    if (end != value.size() || !std::isfinite(result)) throw std::runtime_error("Invalid numeric configuration value: " + value);
    return result;
}
Settings load(const fs::path& path) {
    Settings settings; std::ifstream input(path); std::string line; bool channelsSeen = false;
    while (std::getline(input, line)) {
        line = trim(line); if (line.empty() || line[0] == '#') continue;
        auto equal = line.find('='); if (equal == std::string::npos) throw std::runtime_error("Expected key=value in multi-receiver.ini");
        auto key = trim(line.substr(0, equal)), value = trim(line.substr(equal + 1));
        if (key == "udp_host") {
            sockaddr_in probe{}; if (inet_pton(AF_INET, value.c_str(), &probe.sin_addr) != 1) throw std::runtime_error("udp_host must be an IPv4 address");
            settings.udpHost = value;
        } else if (key == "calibration") {   // calibration=<radio key>,<correction Hz>,<unix time>
            const auto a = value.find(","), b = value.rfind(",");
            if (a == std::string::npos || b == a) throw std::runtime_error("calibration=radio,correction_Hz,unix_time");
            settings.calibrations.push_back({trim(value.substr(0, a)), number(trim(value.substr(a + 1, b - a - 1))), (long long)number(trim(value.substr(b + 1)))});
        } else if (key == "radio") {
            settings.lastRadio = value;
        } else if (key == "channel") {
            if (!channelsSeen) { settings.channels.clear(); channelsSeen = true; }
            std::istringstream fields(value); std::vector<double> parameters; std::string field;
            while (std::getline(fields, field, ',')) parameters.push_back(number(trim(field)));
            if (parameters.size() != 3 || parameters[2] != std::floor(parameters[2])) throw std::runtime_error("channel=frequency_MHz,symbol_rate_kSps,standard_0_auto_1_S_2_S2");
            settings.channels.push_back({parameters[0],parameters[1]*1000,(int)parameters[2],false});
        } else {
            double v = number(value);
            auto integer = [&](int low, int high) { if (v != std::floor(v) || v < low || v > high) throw std::runtime_error("Invalid " + key); return (int)v; };
            if (key == "max_channels") settings.capture.maxChannels = integer(1,16);
            else if (key == "sample_rate") settings.capture.sampleRate = v;
            else if (key == "center_if_mhz") settings.capture.centerIfHz = v * 1e6;
            else if (key == "lnb_mhz") settings.capture.lnbHz = v * 1e6;
            else if (key == "correction_hz") settings.capture.correctionHz = v;
            else if (key == "filter_mhz") { if (v < 0 || v > 56) throw std::runtime_error("Invalid filter_mhz"); settings.capture.filterHz = v * 1e6; }
            else if (key == "auto_gain") settings.automaticGain = integer(0,1) != 0;
            else if (key == "gain_db") { if (v < 0 || v > 100) throw std::runtime_error("Invalid gain_db"); settings.gain = v; }
            else if (key == "http_port") settings.port = integer(1024,65000);
            else if (key == "video_port_base") settings.videoBase = integer(1024,65000);
            else if (key == "rtsp_port_base") settings.rtspBase = integer(1024,65000);
            else if (key == "ice_port_base") settings.iceBase = integer(1024,65000);
            else if (key == "udp_output") settings.udpOutput = integer(0,1) != 0;
            else if (key == "check_updates") settings.checkUpdates = integer(0,1) != 0;
            else if (key == "udp_port_base") settings.udpBase = integer(1024,65000);
            else throw std::runtime_error("Unknown configuration key: " + key);
        }
    }
    settings.legacyCorrection = settings.capture.correctionHz;
    // Every start has the capture centred on the middle of the band (10495.0 MHz); LEFT / RIGHT on the page move it for the session.
    settings.capture.centerIfHz = 10495.0e6 - settings.capture.lnbHz;
    // a settings file of the two-channel version: the slots up to the four of this version are added (stopped, at free places of the band)
    if (settings.capture.maxChannels == 2) settings.capture.maxChannels = 4;
    for (const double spare : {10493.0, 10498.0, 10494.5, 10497.0})
        if (settings.channels.size() < std::min<size_t>(settings.capture.maxChannels, 4)) settings.channels.push_back({spare, 333000, 0, false});
    if (settings.channels.size() > settings.capture.maxChannels) throw std::runtime_error("Configured channel count exceeds max_channels");
    return settings;
}
void save(const fs::path& path, const Settings& settings) {
    std::ofstream out(path); out.precision(12);
    out << "# SDR-Tiouner settings. Frequencies are satellite MHz.\n"
        << "max_channels=" << settings.capture.maxChannels << "\nsample_rate=" << settings.capture.sampleRate
        << "\ncenter_if_mhz=" << settings.capture.centerIfHz / 1e6 << "\nlnb_mhz=" << settings.capture.lnbHz / 1e6
        << "\ncorrection_hz=" << settings.legacyCorrection << "\nfilter_mhz=" << settings.capture.filterHz / 1e6 << "\ngain_db=" << settings.gain
        << "\nauto_gain=" << (settings.automaticGain ? 1 : 0) << "\nhttp_port=" << settings.port << "\nvideo_port_base=" << settings.videoBase
        << "\nrtsp_port_base=" << settings.rtspBase << "\nice_port_base=" << settings.iceBase
        << "\ncheck_updates=" << (settings.checkUpdates ? 1 : 0) << "\nudp_output=" << (settings.udpOutput ? 1 : 0) << "\nudp_host=" << settings.udpHost << "\nudp_port_base=" << settings.udpBase << '\n';
    if (!settings.lastRadio.empty()) out << "radio=" << settings.lastRadio << '\n';
    for (const auto& c : settings.calibrations) out << "calibration=" << c.key << ',' << c.correctionHz << ',' << c.when << '\n';
    // the start list of the file stays as it is: at every start the channels come back on it, whatever was tuned while the program ran
    for (const auto& channel : settings.channels) out << "channel=" << channel.satelliteMhz << ',' << channel.symbolRate / 1000 << ',' << channel.standard << '\n';
    if (!out) throw std::runtime_error("Cannot save " + path.string());
}
// How a radio is told apart in the settings: its serial, else its name (no commas or line ends: it is a field of the .ini)
std::string radioKey(const DeviceInfo& d) {
    std::string key = !d.serial.empty() ? d.board + ":" + d.serial : d.name;
    for (auto& c : key) if (c == ',' || c == '\n' || c == '\r') c = ' ';
    return key;
}
// The radio as the page lists it (the entry, e.g. the antenna port included): what the settings remember
std::string radioLabel(const DeviceInfo& d) {
    std::string label = d.name;
    for (auto& c : label) if (c == ',' || c == '\n' || c == '\r') c = ' ';
    return label;
}
const Settings::Calibration* findCalibration(const Settings& settings, const std::string& key) {
    for (const auto& c : settings.calibrations) if (c.key == key) return &c;
    return nullptr;
}
// One run of the calibration on the beacon: the first channel is tuned to 10491.500 MHz, its carrier offset is averaged for a few seconds,
// the correction is moved by that much, and the channel gets its own settings back.
struct CalibrationRun {
    int phase = 0;   // 0 idle, 1 waiting for the beacon to lock, 2 measuring
    unsigned id = 0;
    ChannelConfig saved;
    std::chrono::steady_clock::time_point since;
    double sum = 0;
    int n = 0;
    uint64_t seq = 0;
    std::string failure;   // the last failure (shown until the next start)
};
struct Publisher {
    BrowserStream stream;
    uint64_t revision = 0;
    std::chrono::steady_clock::time_point retryAt{};
};
}
// A very narrow radio (an RTL-SDR at 2.4 Msps) runs one channel. The radio's own DC spike sits at the exact centre of its window, so the window
// is put beside the channel (offset tuning, as OnAir does), not on it: the channel is `offset` above the window's centre. The offset is
// the channel's half width plus a margin, at least 250 kHz, and at most what still keeps the whole channel inside the window.
double narrowWindowIfHz(double satMhz, double symbolRateHz, double rateHz, double lnbHz, double correctionHz, double* offsetHz = nullptr) {
    const double half = symbolRateHz * 1.35 / 2, room = rateHz * 0.49 - half;
    double off = std::max(250e3, half + 40e3);
    if (off > room) off = std::max(0.0, room);
    if (offsetHz) *offsetHz = off;
    return satMhz * 1e6 - off - lnbHz + correctionHz;
}

// Why a radio runs below the full band, in words the user can act on (the link was measured, or the radio is simply narrow)
// A notice is "title\ntext": the page shows the first line as the title.
static const char* kPositionHint = "Choose which part you see with the three buttons on the Local Spectrum panel.";
std::string slowBandNotice(const DeviceInfo& d, double rateHz) {
    char b[400];
    const double mhz = rateHz / 1e6 * 0.98;
    if (d.board == "pluto")   // (USB and network cannot be told apart reliably: the Pluto also shows up as a network device over USB)
        std::snprintf(b, sizeof b, "Pluto: part of the band only\nThis connection delivers %.1f MHz of the 9 MHz band.\n%s\nA faster link (gigabit Ethernet) gives the full band.", mhz, kPositionHint);
    else if (d.board == "lime")
        std::snprintf(b, sizeof b, "Lime: part of the band only\nThis USB port delivers %.1f MHz of the 9 MHz band.\n%s\nA USB 3 port and cable give the full band.", mhz, kPositionHint);
    else
        std::snprintf(b, sizeof b, "Part of the band only\nThis radio's connection delivers %.1f MHz of the 9 MHz band.\n%s", mhz, kPositionHint);
    return b;
}

// Demo mode has no internet band feed: the two synthetic 1.5 MS/s carriers (10491.5 and 10496.5 MHz) as a BATC-style spectrum frame.
static std::vector<uint16_t> demoBatcFrame() {
    std::vector<uint16_t> bins(9001, 10000);   // 10490.5-10499.5 MHz, 1 kHz per bin
    for (const int centre : {1000, 6000}) for (int i = centre - 750; i <= centre + 750; ++i) bins[i] = 23000;
    return bins;
}

int main(int argc, char** argv) {
    try {
        if (!cpuSupportsBuild()) { std::cerr << "This build requires an AVX2 CPU.\n"; return 1; }
        {   // the greeting
            std::cout << "\n"
                "  ===============================================================\n"
                "   SDR-Tiouner  v" SDR_TIOUNER_VERSION "   -   MULTI CHANNEL  -  QO-100 DATV\n"
                "   by HB9IIU        (built " __DATE__ ")\n"
                "  ===============================================================\n"
                "   Hello, and welcome! Several DATV channels from one radio.\n"
                "   Free software (GPL-3.0-or-later), based on OnAir.\n"
                "   Contact: qrz.com/db/HB9IIU\n"
                "   73 and happy DATV watching!\n"
                "  ---------------------------------------------------------------\n\n" << std::flush;
        }
        bool demo = false, open = true, bothS2 = false;
        std::string iqFile; double iqRate = 0;   // --iq-file <cf32 file> <Msps>: a recording offered as a radio (tests)
        fs::path executable = fs::absolute(argv[0]).parent_path(), configPath = executable / "multi-receiver.ini";
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--demo") demo = true;
            else if (arg == "--demo-s2") { demo = true; bothS2 = true; }
            else if (arg == "--no-open") open = false;
            else if (arg == "--iq-file" && i + 2 < argc) { iqFile = argv[++i]; iqRate = std::atof(argv[++i]) * 1e6; }   // a cf32 recording (tests): listed as a radio
            else if (arg == "--config" && i + 1 < argc) configPath = fs::path(std::u8string(reinterpret_cast<const char8_t*>(argv[++i])));
            else { std::cerr << "Usage: SDR-Tiouner.exe [--demo | --demo-s2] [--no-open] [--config file]\n"; return 1; }
        }
        auto settings = load(configPath);
        if (demo) {
            if (settings.capture.maxChannels < 2) throw std::runtime_error("The two-carrier demo needs max_channels >= 2");
            settings.channels = {{10491.5,1500000,0,false},{10496.5,1500000,0,false}};
        }
        MultiReceiver receiver(settings.capture);
        AutoMonitor autoMonitor;
        auto persist = [&] { save(configPath,settings); };
        receiver.setGain(settings.automaticGain,settings.gain);
        for (const auto& channel : settings.channels)   // a saved channel outside this capture is left out, not a reason to stop
            if (!receiver.create(channel)) std::cerr << "Channel at " << channel.satelliteMhz << " MHz does not fit the IQ capture: left out" << std::endl;
        MultiWeb web(receiver); std::string error;
        if (!web.start(settings.port, settings.videoBase, error)) {
            if (open && error.rfind("Already running", 0) == 0) openUrl("http://127.0.0.1:" + std::to_string(settings.port) + "/");   // a second double-click: show the one that runs
            throw std::runtime_error(error);
        }
        web.setUdpSettings(settings.udpOutput, settings.udpHost, settings.udpBase);
        std::vector<DeviceInfo> radios;
        int selected = -1;
        auto refresh = [&] {
            std::string scanError; radios = listRadios(scanError); selected = -1;
            if (!iqFile.empty() && iqRate > 0) {
                DeviceInfo f; f.kind = DeviceInfo::File; f.name = "IQ file " + iqFile; f.serial = iqFile; f.maxRateHz = iqRate; f.minRateHz = iqRate;
                radios.push_back(f);
            }
            // The radio picked when the page opens: the first that can do the full band (9+ Msps): SDRplay, Airspy, PlutoSDR, HackRF, then the rest.
            auto rank = [](const DeviceInfo& d) {
                auto has = [&](const char* name) { return d.board.find(name) != std::string::npos || d.soapyArgs.find(name) != std::string::npos; };
                if (d.maxRateHz > 0 && d.maxRateHz < 9e6) return 99;   // too slow for the whole transponder
                if (has("sdrplay")) return 0;
                if (has("airspy")) return 1;
                if (has("pluto")) return 2;
                if (has("lime")) return 3;
                if (d.kind == DeviceInfo::HackRF) return 3;
                return 4;
            };
            int best = 99;
            for (size_t i = 0; i < radios.size(); ++i) { const int r = rank(radios[i]); if (r < best) { best = r; selected = (int)i; } }
            // the radio used last time, when it is still there, wins over the ranking (even a slow one: it was the user's own choice)
            if (!settings.lastRadio.empty())
                for (size_t i = 0; i < radios.size(); ++i) if (radioLabel(radios[i]) == settings.lastRadio) selected = (int)i;
            // nothing chosen yet (only a radio too slow for the whole band, such as an RTL-SDR, is connected): it is still the one to offer
            if (selected < 0 && !radios.empty()) selected = 0;
            web.setRadios(radios, selected, settings.gain);
            if (!scanError.empty()) std::cerr << scanError << '\n';
        };
        // Demo/testing never enumerates or opens the user's physical radio.
        if (!demo) refresh(); else web.setRadios({}, -1);
        if (demo) {
            if (!receiver.startDemo(error,bothS2 ? 2 : 1)) throw std::runtime_error(error);
            for (auto channel : receiver.channels()) { channel.config.enabled = true; receiver.configure(channel.id,channel.config,error); }
            web.setMessage(std::string("Synthetic IQ test: DVB-S2 at 10491.500 and ") + (bothS2 ? "DVB-S2" : "DVB-S") + " at 10496.500 MHz, both 1500 kS/s.");
        }
        std::signal(SIGINT, interrupted); std::signal(SIGTERM, interrupted);
        const std::string url = "http://127.0.0.1:" + std::to_string(settings.port) + "/";
        std::cout << "SDR-Tiouner: " << url << "\nOne wideband radio; cap " << settings.capture.maxChannels << " channels.\nOpen the page in your browser; use Close on the page or press Ctrl+C here to exit.\n" << std::flush;
        if (open) openUrl(url);
        std::map<unsigned, std::unique_ptr<Publisher>> publishers;
        UdpTsOutputs udpOutputs;   // declared after the receiver: its senders end first
        TsRecorder recorder;       // the same for its writers
        UpdateCheck updates;       // is a newer release out?
        if (settings.checkUpdates) updates.start(SDR_TIOUNER_VERSION);
        bool recording = false;    // the Record button: not remembered, a new start never begins by recording
        const fs::path recordDir = executable / "recordings";
        CalibrationRun calibration;
        LoadMeter loadMeter;
        auto nextLoad = std::chrono::steady_clock::now();
        unsigned loadSeq = 0;
        bool smartStartDone = false;   // the first Start receiving after launch places the channels on the stations found (see below)
        while (!quit) {
            MultiCommand command;
            if (web.takeCommand(command)) {
                try {
                error.clear();
                // Auto Monitor does not need the Receiver page: with no capture running it starts the selected radio itself.
                bool autoAfterStart = false;
                if (command.action == MultiCommand::AutoStart && !receiver.running() && selected >= 0 && !calibration.phase) {
                    command.action = MultiCommand::RadioStart; command.device = selected; command.gain = settings.gain; autoAfterStart = true;
                }
                if (autoMonitor.enabled && command.action != MultiCommand::AutoStart && command.action != MultiCommand::AutoStop &&
                    command.action != MultiCommand::GainSet && command.action != MultiCommand::UdpSet && command.action != MultiCommand::RecordSet && command.action != MultiCommand::RecordOpen) autoMonitor.stop(receiver);
                if (command.action == MultiCommand::AutoStart) {
                    if (calibration.phase) error = "Wait for calibration to finish";
                    else autoMonitor.start(receiver);
                }
                else if (command.action == MultiCommand::AutoStop) autoMonitor.stop(receiver);
                else if (command.action == MultiCommand::Shutdown) quit = true;
                else if (command.action == MultiCommand::RadioStop) { receiver.stop(); web.setMessage("Shared capture stopped"); }
                else if (command.action == MultiCommand::RadioRefresh) { if (!receiver.running()) refresh(); }
                else if (command.action == MultiCommand::RadioStart) {
                    if (receiver.running()) error = "Stop capture before changing radios";
                    else {
                        selected = command.device; settings.gain = command.gain;
                        bool started = false;
                        unsigned narrowOnly = 0;   // a very narrow radio: the one channel that runs
                        if (selected >= 0 && selected < (int)radios.size()) {
                            TuneSettings tune; tune.gainDb = command.gain; tune.biasTee = false;
                            // Automatic gain starts in the middle of this radio's own range (Airspy 0-21, PlutoSDR 0-73, SDRplay/HackRF 0-100) and
                            // finds its level from there: the page no longer asks for a number.
                            if (settings.automaticGain) {
                                const auto& d = radios[selected];
                                const double lo = d.isGeneric() ? d.gainMinDb : 0, hi = d.isGeneric() && d.gainMaxDb > 0 ? d.gainMaxDb : 100;
                                tune.gainDb = std::round(lo + 0.5 * (hi - lo));
                                settings.gain = tune.gainDb;
                            }
                            // the correction measured on this radio, or the one of the file for a radio never calibrated
                            const auto* known = findCalibration(settings, radioKey(radios[selected]));
                            receiver.setCorrection(known ? known->correctionHz : settings.legacyCorrection);
                            calibration.failure.clear();
                            // The LimeSDR takes its time (it is calibrated and its USB link measured): say so before the page waits
                            if (radios[selected].board == "lime")
                                web.setNotice("The LimeSDR is slow to start: it calibrates itself and checks its USB connection. Please be patient, this takes a few seconds...", true);
                            started = receiver.start(radios[selected], tune, error);
                        } else error = "Radio is no longer listed; refresh radios";
                        web.setRadios(radios, selected, settings.gain);
                        if (started) {
                            settings.lastRadio = radioLabel(radios[selected]);
                            // A radio that runs slower than the full band (a PlutoSDR over USB or Ethernet, at what its link carries): the capture is
                            // narrower, so the position is chosen for the most channels that fit (left, centre or right of the band).
                            const double actualRate = receiver.sampleRate();
                            const auto slots = receiver.channels();
                            if (actualRate > 0 && actualRate < 3e6 && !slots.empty()) {
                                // A very narrow radio (an RTL-SDR, 2.4 Msps): one channel. The window is put on channel 1, the others stay stopped
                                // (a card's Start brings one back if it falls inside the window).
                                double off = 0;
                                const double ifHz = narrowWindowIfHz(slots.front().config.satelliteMhz, slots.front().config.symbolRate, actualRate,
                                                                     settings.capture.lnbHz, receiver.config().correctionHz, &off);
                                if (receiver.setCenterIf(ifHz)) settings.capture.centerIfHz = ifHz;
                                narrowOnly = slots.front().id;
                                const bool rtl = radios[selected].board == "rtlsdr";
                                char text[800];
                                std::snprintf(text, sizeof text,
                                    "%s: one channel\n%s %.1f MHz of the band at once, so SDR-Tiouner runs one channel: CH %u at %.3f MHz.\n"
                                    "Another channel can be started from its card if it falls inside the window now shown.\n"
                                    "For more channels, use a wider radio (HackRF, Airspy, SDRplay, Lime on USB 3).%s",
                                    rtl ? "RTL-SDR" : "Narrow connection",
                                    rtl ? "An RTL-SDR sees only" : "This connection delivers only", actualRate / 1e6 * 0.98,
                                    slots.front().id, slots.front().config.satelliteMhz,
                                    slots.front().config.symbolRate > 1.0e6 ? "\nThis channel is wider than about 1 MS/s, which is too much for this radio. Please choose a narrower one." : "");
                                web.setNotice(text);
                            } else if (actualRate > 0 && actualRate < 9.5e6) {
                                web.setNotice(slowBandNotice(radios[selected], actualRate));
                                const auto targets = receiver.alignTargetsMhz();
                                const double correction = receiver.config().correctionHz, lnb = settings.capture.lnbHz;
                                int bestCount = -1; double bestSat = targets[1];
                                for (const double sat : {targets[1], targets[0], targets[2]}) {   // (the centre first: it wins a tie)
                                    int count = 0;
                                    for (const auto& slot : receiver.channels())
                                        if (std::abs(slot.config.satelliteMhz - sat) * 1e6 + slot.config.symbolRate * 1.35 / 2 <= actualRate * .49) ++count;
                                    if (count > bestCount) { bestCount = count; bestSat = sat; }
                                }
                                const double ifHz = bestSat * 1e6 - lnb + correction;
                                if (receiver.setCenterIf(ifHz)) settings.capture.centerIfHz = ifHz;
                            }
                            // First start after launch: channel 1 on the beacon, the strongest stations above it (up to three) on channels 2-4 in
                            // order of frequency; a channel without a station keeps its start-list frequency. Not for a one-channel radio, not for
                            // the demo, and not when the Auto Monitor starts the radio (it takes over the channels itself).
                            if (!smartStartDone && !autoAfterStart && !demo && !narrowOnly) {
                                smartStartDone = true;
                                const auto slots = receiver.channels();
                                std::string ignored;
                                if (!slots.empty()) {
                                    ChannelConfig beacon = slots[0].config; beacon.satelliteMhz = kBeaconMhz; beacon.symbolRate = kBeaconRate; beacon.standard = 0;
                                    receiver.configure(slots[0].id, beacon, ignored);
                                }
                                const auto& rc = receiver.config();
                                const double centreMhz = (rc.centerIfHz + rc.lnbHz - rc.correctionHz) / 1e6;
                                const auto stations = pickStartStations(web.batcFrame(), slots.size() > 1 ? std::min<size_t>(3, slots.size() - 1) : 0, centreMhz, receiver.sampleRate());
                                for (size_t i = 0; i < stations.size(); ++i) {
                                    ChannelConfig cfg = slots[1 + i].config; cfg.satelliteMhz = stations[i].mhz; cfg.symbolRate = stations[i].rate; cfg.standard = 0;
                                    receiver.configure(slots[1 + i].id, cfg, ignored);
                                }
                            }
                            // every channel slot starts with the capture, on the frequency and symbol rate it has
                            for (const auto& slot : receiver.channels()) {
                                ChannelConfig cfg = slot.config; cfg.enabled = !narrowOnly || slot.id == narrowOnly; std::string ignored;
                                receiver.configure(slot.id, cfg, ignored);
                            }
                            persist();
                            {
                                char text[220];
                                if (receiver.sampleRate() < 3e6) std::snprintf(text, sizeof text, "Capture running at %.1f Msps: %.1f to %.1f MHz. The window follows the channel you start.",
                                    receiver.sampleRate() / 1e6, receiver.config().centerIfHz / 1e6 + settings.capture.lnbHz / 1e6 - receiver.sampleRate() / 2e6, receiver.config().centerIfHz / 1e6 + settings.capture.lnbHz / 1e6 + receiver.sampleRate() / 2e6);
                                else if (receiver.sampleRate() < 9.5e6) std::snprintf(text, sizeof text, "Capture running at %.1f Msps (the most this radio can do here): %.1f to %.1f MHz. Channels outside it cannot lock; use the position buttons.",
                                    receiver.sampleRate() / 1e6, receiver.config().centerIfHz / 1e6 + settings.capture.lnbHz / 1e6 - receiver.sampleRate() / 2e6, receiver.config().centerIfHz / 1e6 + settings.capture.lnbHz / 1e6 + receiver.sampleRate() / 2e6);
                                else std::snprintf(text, sizeof text, "Capture running: all channels started.");
                                web.setMessage(text);
                            }
                        }
                    }
                } else if (command.action == MultiCommand::GainSet) {
                    settings.automaticGain = command.automaticGain; settings.gain = command.gain;
                    receiver.setGain(settings.automaticGain,settings.gain); persist();
                    web.setMessage(settings.automaticGain ? "Automatic gain enabled for the shared radio" : "Manual gain selected");
                } else if (command.action == MultiCommand::AlignCapture) {
                    // left / centre / right of the band for the window the radio really runs (receiver.alignTargetsMhz); the correction moves the
                    // real window, so it is added to the IF that is set
                    const auto targets = receiver.alignTargetsMhz();
                    const double satMhz = targets[command.align < 0 ? 1 : command.align > 2 ? 2 : command.align];
                    const double halfMhz = (receiver.running() && receiver.sampleRate() > 0 ? receiver.sampleRate() : settings.capture.sampleRate) / 2e6;
                    const double ifHz = satMhz * 1e6 - settings.capture.lnbHz + receiver.config().correctionHz;
                    if (!receiver.setCenterIf(ifHz)) error = "That capture centre is not possible";
                    else {
                        settings.capture.centerIfHz = ifHz; persist();
                        char range[96]; std::snprintf(range, sizeof range, "%.1f to %.1f MHz", satMhz - halfMhz, satMhz + halfMhz);
                        web.setMessage(std::string("Capture aligned ") + (command.align == 0 ? "left" : command.align == 2 ? "right" : "to the centre") + ": " + range);
                    }
                } else if (command.action == MultiCommand::Calibrate) {
                    const auto list = receiver.channels();
                    if (!receiver.running()) error = "Start receiving first";
                    else if (calibration.phase) error = "The calibration is already running";
                    else if (list.empty()) error = "There is no channel to calibrate with";
                    else {
                        calibration.id = list.front().id; calibration.saved = list.front().config; calibration.failure.clear();
                        if (receiver.configure(calibration.id, ChannelConfig{10491.5, 1500000, 0, true}, error)) {
                            calibration.phase = 1; calibration.since = std::chrono::steady_clock::now();
                            web.setMessage("Calibrating on the beacon at 10491.500 MHz...");
                        }
                    }
                } else if (command.action == MultiCommand::UdpSet) {
                    settings.udpOutput = command.udpEnabled; settings.udpHost = command.udpHost; settings.udpBase = command.udpPort;
                    udpOutputs.stopAll();   // the senders start again, to the new address, on the next pass
                    web.setUdpSettings(settings.udpOutput, settings.udpHost, settings.udpBase); persist();
                    web.setMessage(settings.udpOutput ? "Stream out to " + settings.udpHost + ":" + std::to_string(settings.udpBase) : std::string("Stream out switched off"));
                } else if (command.action == MultiCommand::RecordOpen) {
                    std::error_code ignored; fs::create_directories(recordDir, ignored);
                    fs::path shown = recordDir; shown.make_preferred();
                    openUrl(shown.string());
                } else if (command.action == MultiCommand::RecordSet) {
                    const bool was = recording;
                    recording = command.record && receiver.running();
                    fs::path shown = recordDir; shown.make_preferred();   // the folder as Windows writes it
                    if (recording && !was)
                        web.setNotice("Recording started\nThe files are saved in:\n" + shown.string() + "\nOne file per channel, named with the date, the frequency and the station. The beacon is not recorded.");
                    else if (!recording && was) {
                        const double mb = recorder.bytes() / 1e6;
                        char size[32]; std::snprintf(size, sizeof size, mb < 1000 ? "%.1f MB" : "%.2f GB", mb < 1000 ? mb : mb / 1000);
                        web.setNotice("Recording stopped\n" + std::to_string(recorder.files()) + (recorder.files() == 1 ? " file, " : " files, ") + size + ", saved in:\n" + shown.string());
                    }
                    web.setMessage(recording ? "Recording the transport streams to " + recordDir.generic_string() : std::string("Recording stopped"));
                } else if (command.action == MultiCommand::ChannelCreate) {
                    if (!receiver.create(command.channel)) error = "Channel pool is full or the carrier falls outside the capture";
                    else { persist(); web.setMessage("Channel created"); }
                } else if (command.action == MultiCommand::ChannelSet) {
                    if (command.channel.enabled && !receiver.running()) error = "Start shared receiving first";
                    else if ([&] {
                        // a narrow radio (an RTL-SDR): the window follows the channel that is started, so it can be tuned anywhere in the band;
                        // a running channel that no longer fits the moved window is stopped
                        const auto now = receiver.channels();
                        if (receiver.running() && receiver.sampleRate() > 0 && receiver.sampleRate() < 3e6 && command.channel.enabled) {
                            const double ifHz = narrowWindowIfHz(command.channel.satelliteMhz, command.channel.symbolRate, receiver.sampleRate(),
                                                                 settings.capture.lnbHz, receiver.config().correctionHz);
                            if (receiver.setCenterIf(ifHz)) {
                                settings.capture.centerIfHz = ifHz;
                                const double centreMhz = (ifHz - receiver.config().correctionHz + settings.capture.lnbHz) / 1e6;
                                for (const auto& other : now) {
                                    if (other.id == command.id || !other.config.enabled) continue;
                                    if (std::abs(other.config.satelliteMhz - centreMhz) * 1e6 + other.config.symbolRate * 1.35 / 2 > receiver.sampleRate() * .49) {
                                        ChannelConfig off = other.config; off.enabled = false; std::string ignored;
                                        receiver.configure(other.id, off, ignored);
                                    }
                                }
                            }
                        }
                        return receiver.configure(command.id,command.channel,error); }()) { persist(); web.setMessage("Channel " + std::to_string(command.id) + (command.channel.enabled ? " tuning" : " stopped")); }
                }
                if (autoAfterStart && error.empty() && receiver.running()) autoMonitor.start(receiver);
                if (!error.empty()) { web.setMessage(error); std::cerr << error << '\n'; }
                } catch (const std::exception& e) {
                    // A channel command or settings-write failure must not tear down
                    // the shared source or the unaffected channel.
                    web.setMessage(e.what()); std::cerr << e.what() << '\n';
                }
            }
            autoMonitor.tick(receiver,demo ? demoBatcFrame() : web.batcFrame(),std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
            web.setAutoMonitor(autoMonitor.json());
            for (const auto& channel : receiver.channels()) {
                auto& publisher = publishers[channel.id];
                if (!publisher) publisher = std::make_unique<Publisher>();
                if (!publisher->stream.active() && std::chrono::steady_clock::now() >= publisher->retryAt) {
                    auto path = configPath.parent_path() / "multi-cache" / ("channel-" + std::to_string(channel.id));
                    const auto pathUtf8 = path.u8string(); std::string workDir((const char*)pathUtf8.data(), pathUtf8.size());
                    // Each publisher has independent processes and ports; a failure stays local to its channel.
                    publisher->stream.start("",true,workDir,settings.videoBase + channel.id - 1,
                        settings.rtspBase + channel.id - 1,settings.iceBase + channel.id - 1,true);
                    publisher->revision = 0;
                    publisher->retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(3);
                }
                if (channel.revision != publisher->revision) {
                    std::string source;
                    if (channel.config.enabled && receiver.running()) source = "http://127.0.0.1:" + std::to_string(settings.port) + "/stream/" + std::to_string(channel.id);
                    publisher->stream.setSource(source,std::to_string(channel.revision)); publisher->revision = channel.revision;
                }
                web.setPublisher(channel.id,publisher->stream.status());
            }
            if (calibration.phase) {
                const auto now = std::chrono::steady_clock::now();
                DvbsTelemetry tel; double age = -1; bool found = false;
                for (const auto& ch : receiver.channels()) if (ch.id == calibration.id) { tel = ch.telemetry; age = ch.packetAgeSeconds; found = true; }
                const double elapsed = std::chrono::duration<double>(now - calibration.since).count();
                const bool locked = found && receiver.running() && tel.tsLock && age >= 0 && age < 2 && std::isfinite(tel.cfoHz);
                bool done = false, ok = false; std::string why; double mean = 0;
                if (!found || !receiver.running()) { done = true; why = "Capture stopped during the calibration."; }
                else if (calibration.phase == 1) {
                    if (locked) { calibration.phase = 2; calibration.since = now; calibration.sum = 0; calibration.n = 0; calibration.seq = tel.seq; }
                    else if (elapsed > 30) { done = true; why = "The beacon at 10491.500 MHz was not found. Check the antenna and the gain, then try again."; }
                } else {
                    if (locked && tel.seq != calibration.seq) { calibration.seq = tel.seq; calibration.sum += tel.cfoHz; ++calibration.n; }
                    if (elapsed >= 5) {
                        done = true;
                        if (calibration.n < 5) why = "The beacon was lost during the measurement. Try again.";
                        else {
                            mean = calibration.sum / calibration.n;
                            if (std::fabs(mean) > 200e3) why = "The measured offset is too large (over 200 kHz): is this really the beacon?";
                            else ok = true;
                        }
                    }
                }
                if (done) {
                    std::string ignored;
                    if (ok) {
                        // the carrier sat `mean` Hz away from where the correction put it: the correction moves by that much
                        const double corrected = receiver.config().correctionHz + mean;
                        receiver.setCorrection(corrected);
                        settings.capture.correctionHz = corrected;
                        const std::string key = selected >= 0 && selected < (int)radios.size() ? radioKey(radios[selected]) : std::string("unknown");
                        bool updated = false;
                        for (auto& c : settings.calibrations) if (c.key == key) { c.correctionHz = corrected; c.when = (long long)std::time(nullptr); updated = true; }
                        if (!updated) settings.calibrations.push_back({key, corrected, (long long)std::time(nullptr)});
                        try { persist(); } catch (const std::exception& e) { std::cerr << e.what() << '\n'; }
                        char text[160]; std::snprintf(text, sizeof text, "Calibrated on the beacon: correction %.1f kHz (moved by %+.1f kHz)", corrected / 1e3, mean / 1e3);
                        web.setMessage(text);
                    } else { calibration.failure = why; web.setMessage(why); }
                    receiver.configure(calibration.id, calibration.saved, ignored);   // the channel gets its own settings back
                    calibration.phase = 0;
                }
            }
            {
                std::ostringstream o; o.precision(12);
                const auto* known = selected >= 0 && selected < (int)radios.size() ? findCalibration(settings, radioKey(radios[selected])) : nullptr;
                if (calibration.phase) o << "{\"state\":\"running\",\"step\":" << calibration.phase << "}";
                else if (!calibration.failure.empty()) o << "{\"state\":\"failed\",\"message\":\"" << calibration.failure << "\"}";
                else if (known) o << "{\"state\":\"ok\",\"correctionHz\":" << known->correctionHz << ",\"when\":" << known->when << "}";
                else o << "{\"state\":\"none\"}";
                web.setCalibration(o.str());
            }
            if (std::chrono::steady_clock::now() >= nextLoad) {   // once a second
                web.setLoad(LoadMeter::json(loadMeter.sample(), ++loadSeq));
                nextLoad = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            udpOutputs.service(receiver, settings.udpHost, settings.udpBase, settings.udpOutput);
            for (const auto& channel : receiver.channels()) web.setUdpOutput(channel.id, udpOutputs.address(channel.id));
            if (recording && !receiver.running()) recording = false;   // the radio stopped: nothing left to record
            recorder.service(receiver, recording, recordDir);
            if (recording && !recorder.problem().empty()) { web.setNotice(recorder.problem()); recording = false; recorder.service(receiver, false, recordDir); }
            web.setRecording(recorder.json(recording, recordDir));
            web.setUpdate(updates.json());
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        updates.stop(); recorder.stopAll(); udpOutputs.stopAll(); publishers.clear(); web.stop(); receiver.stop(); return 0;
    } catch (const std::exception& e) { std::cerr << "SDR-Tiouner: " << e.what() << '\n'; return 1; }
}
