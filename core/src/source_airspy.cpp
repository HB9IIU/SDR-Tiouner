// Native driver for the Airspy R2 / Mini: the radio's own library (airspy.dll / libairspy.so) is loaded at run time when it is installed.
// See native_common.h. The stream is 32-bit float IQ, so nothing is converted: the samples go to the ring as they come.
#include "native_common.h"
#include <map>

namespace dect2 {
namespace native {

struct AirspyApi {
    struct dev_t;
    struct transfer_t {   // airspy_transfer in airspy.h
        dev_t* device;
        void* ctx;
        void* samples;
        int sample_count;
        uint64_t dropped_samples;
        int sample_type;
    };
    typedef int (DECT2_CALL* cb_t)(transfer_t*);
    int (DECT2_CALL* init)(void) = nullptr;
    int (DECT2_CALL* list_devices)(uint64_t*, int) = nullptr;
    int (DECT2_CALL* open_sn)(dev_t**, uint64_t) = nullptr;
    int (DECT2_CALL* close)(dev_t*) = nullptr;
    int (DECT2_CALL* get_samplerates)(dev_t*, uint32_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_samplerate)(dev_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_sample_type)(dev_t*, int) = nullptr;
    int (DECT2_CALL* set_freq)(dev_t*, uint32_t) = nullptr;
    int (DECT2_CALL* set_linearity_gain)(dev_t*, uint8_t) = nullptr;
    int (DECT2_CALL* set_sensitivity_gain)(dev_t*, uint8_t) = nullptr;
    int (DECT2_CALL* start_rx)(dev_t*, cb_t, void*) = nullptr;
    int (DECT2_CALL* stop_rx)(dev_t*) = nullptr;
    int (DECT2_CALL* is_streaming)(dev_t*) = nullptr;
    int (DECT2_CALL* set_rf_bias)(dev_t*, uint8_t) = nullptr;        // optional: antenna power (libairspy 1.0.7 and later)
    int (DECT2_CALL* set_packing)(dev_t*, uint8_t) = nullptr;        // optional: 12-bit packed USB transfers (libairspy 1.0.6 and later)
    const char* (DECT2_CALL* error_name)(int) = nullptr;             // optional
    bool ok = false;
    DynLib lib;
    AirspyApi() {
        if (nativeDisabled() || !lib.open(libNames("airspy", {".0"}, {"airspy.dll", "libairspy.dll"}))) return;
        ok = lib.get(init, "airspy_init") && lib.get(list_devices, "airspy_list_devices") && lib.get(open_sn, "airspy_open_sn") &&
             lib.get(close, "airspy_close") && lib.get(get_samplerates, "airspy_get_samplerates") &&
             lib.get(set_samplerate, "airspy_set_samplerate") && lib.get(set_sample_type, "airspy_set_sample_type") &&
             lib.get(set_freq, "airspy_set_freq") && lib.get(set_linearity_gain, "airspy_set_linearity_gain") &&
             lib.get(set_sensitivity_gain, "airspy_set_sensitivity_gain") && lib.get(start_rx, "airspy_start_rx") &&
             lib.get(stop_rx, "airspy_stop_rx") && lib.get(is_streaming, "airspy_is_streaming");
        if (ok) {
            lib.opt(set_rf_bias, "airspy_set_rf_bias");
            lib.opt(set_packing, "airspy_set_packing");
            lib.opt(error_name, "airspy_error_name");
            if (init() != 0) ok = false;   // (never ended: the library lives as long as the program)
        }
    }
};
AirspyApi& airspy() { static AirspyApi a; return a; }

namespace {

constexpr double kMinHz = 24e6, kMaxHz = 1800e6;   // R820T2
constexpr int kGainSteps = 22;                      // linearity / sensitivity gain: 0..21
constexpr int kSampleFloat32Iq = 0;                 // AIRSPY_SAMPLE_FLOAT32_IQ
constexpr int kErrNotFound = -5, kErrBusy = -6, kErrLibusb = -1000;

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

std::string serialText(uint64_t sn) {
    char b[24];
    snprintf(b, sizeof b, "%016llX", (unsigned long long)sn);
    return b;
}

std::string errorText(int code) {
    std::string s = "error " + std::to_string(code);
    if (airspy().error_name) if (const char* n = airspy().error_name(code)) s = n;
    return s;
}

// The rates a radio offers, read once per radio: an open one cannot be opened again to ask.
std::mutex gRatesMu;
std::map<std::string, std::vector<uint32_t>> gRates;

// libairspy 1.0.10 can crash inside airspy_close() on Windows (a null write in ntdll from one of the library's own USB threads; the
// close-order patch is not enough with every libusb). So on Windows a radio is never closed: its handle is kept and the next start of the
// same radio reuses it (stopped, never streaming). DECT2_AIRSPY_KEEP=0 closes handles, =1 keeps them elsewhere too.
std::mutex gKeepMu;
std::map<uint64_t, AirspyApi::dev_t*> gKept;
bool keepHandles() {
    const char* k = getenv("DECT2_AIRSPY_KEEP");
    return k && *k ? *k != '0' : kOnWindows;
}

std::vector<uint32_t> readRates(AirspyApi::dev_t* dev) {
    std::vector<uint32_t> r;
    uint32_t n = 0;
    if (airspy().get_samplerates(dev, &n, 0) != 0 || n == 0 || n > 16) return r;
    r.resize(n);
    if (airspy().get_samplerates(dev, r.data(), n) != 0) r.clear();
    return r;
}

const char* modelOf(const std::vector<uint32_t>& rates) {
    uint32_t top = 0;
    for (uint32_t v : rates) top = std::max(top, v);
    return top >= 10000000 ? "Airspy R2" : top >= 6000000 ? "Airspy Mini" : "Airspy";
}

std::vector<RadioSetting> airspySettings() {
    std::vector<RadioSetting> v = {ppmSetting(0.1)};
    v.push_back(choiceSetting("table", "Gain table",
                              "Linearity: the gain steps keep the strongest signals clean (the default; best next to a strong carrier or an LNB).\n"
                              "Sensitivity: the steps favour weak signals, at the price of overloading earlier.",
                              {"linearity", "sensitivity"}, {"Linearity", "Sensitivity"}, "linearity"));
    return v;
}

} // namespace

class AirspySource : public NativeSource {
public:
    explicit AirspySource(uint64_t serial) : serial_(serial) {}
    ~AirspySource() override { stop(); }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        dev_ = nullptr;
        broken_ = false;
        {   // a handle kept by an earlier run (or by the listing)
            std::lock_guard<std::mutex> lk(gKeepMu);
            const auto it = gKept.find(serial_);
            if (it != gKept.end()) { dev_ = it->second; gKept.erase(it); }
        }
        const bool reused = dev_ != nullptr;
        const int r = dev_ ? 0 : airspy().open_sn(&dev_, serial_);
        if (r != 0 || !dev_) {
            dev_ = nullptr;
            err = "Airspy: cannot open the radio: " + errorText(r);
            if (r == kErrNotFound) {
                err = "Airspy " + serialText(serial_) + " not found (unplugged?)";
                if (kOnWindows) err += std::string(". ") + windowsUsbDriverHint();
                openFailure_ = OpenFailure::Final;
            } else if (r == kErrLibusb && kOnWindows) {
                err += std::string(". ") + windowsUsbDriverHint() + ", or another program has the radio open";
            } else if (r == kErrBusy) openFailure_ = OpenFailure::Retry;
            return false;
        }
        {   // the rates this radio offers (kept for the listing, which cannot ask an open radio)
            std::vector<uint32_t> rates = readRates(dev_);
            if (!rates.empty()) {
                rates_ = rates;
                std::lock_guard<std::mutex> lk(gRatesMu);
                gRates[serialText(serial_)] = rates;
            }
        }
        bias_ = false;
        airspy().set_sample_type(dev_, kSampleFloat32Iq);   // before the rate: the library lists its rates for the sample type
        if (configure(s, err, false)) return true;
        // A kept handle that cannot be configured belongs to a radio that was unplugged since: it is dropped without closing it (that is the
        // call that crashes), and the next try opens the radio afresh. A refusal for the frequency range says nothing about the handle.
        if (reused && openFailure_ != OpenFailure::Final) { dev_ = nullptr; openFailure_ = OpenFailure::Retry; }
        return false;
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!dev_) return false;
        if (!live) {
            // the radio offers a few fixed rates (R2: 10 and 2.5 Msps, Mini: 6 and 3): the smallest that is enough, else the largest
            const double want = s.sampleRate > 0 ? s.sampleRate : 10e6;
            double best = 0, largest = 0;
            for (uint32_t v : rates_) {
                largest = std::max(largest, (double)v);
                if (v >= want * 0.999 && (best == 0 || v < best)) best = v;
            }
            if (best == 0) best = largest;
            if (best == 0) best = 10e6;   // a library that did not list its rates
            if (const int r = airspy().set_samplerate(dev_, (uint32_t)std::lround(best)); r != 0) {
                err = "Airspy: the sample rate of " + mhz(best) + " Msps was refused (" + errorText(r) + ")";
                return false;
            }
            rate_ = best;
            // Packed 12-bit transfers: the unpacked 16-bit stream of 10 Msps (20 Msps of the ADC) is about 40 MB/s, more than USB 2 carries
            // reliably, and the library then loses whole transfers (about 4 % of the samples: nothing locks to a stream with gaps).
            // Packed it is 30 MB/s. Not needed at the low rate. Only while the radio is not streaming, which is the case here.
            if (airspy().set_packing) {
                const int p = airspy().set_packing(dev_, best > 5e6 ? 1 : 0);
                if (p != 0) { fprintf(stderr, "Airspy: packed transfers refused (%s)\n", errorText(p).c_str()); fflush(stderr); }
            }
            requested_ = s.sampleRate;
            checkRate(s.sampleRate, err);
        }
        const double f = std::max(s.centerHz, 0.0);
        if (f < kMinHz || f > kMaxHz) {
            err = "Airspy could not tune to " + mhz(f) + " MHz (range " + mhz(kMinHz) + "-" + mhz(kMaxHz) + " MHz)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        // the library has no frequency correction: ask for the frequency that really lands on the one wanted
        const double tuneHz = ppmCorrectedHz(f, radioPpm(s));
        if (const int r = airspy().set_freq(dev_, (uint32_t)std::llround(std::clamp(tuneHz, kMinHz, kMaxHz))); r != 0) {
            err = "Airspy could not tune to " + mhz(f) + " MHz (" + errorText(r) + ")";
            return false;
        }
        if (airspy().set_rf_bias && s.biasTee != bias_) {
            if (airspy().set_rf_bias(dev_, s.biasTee ? 1 : 0) == 0) bias_ = s.biasTee;
            else if (s.biasTee) { fprintf(stderr, "Airspy: the radio refused the bias-tee\n"); fflush(stderr); }
        }
        // one overall gain (0..21): the library's linearity or sensitivity table sets LNA, mixer and VGA together
        const uint8_t g = (uint8_t)std::clamp((int)std::lround(s.gainDb), 0, kGainSteps - 1);
        gainShown_ = g; tableShown_ = radioOption(s, "table", "linearity") == "sensitivity";
        const int r = radioOption(s, "table", "linearity") == "sensitivity" ? airspy().set_sensitivity_gain(dev_, g)
                                                                              : airspy().set_linearity_gain(dev_, g);
        if (r != 0) { fprintf(stderr, "Airspy: the gain of %d was refused (%s)\n", (int)g, errorText(r).c_str()); fflush(stderr); }
        return true;
    }
    void closeDevice() override {
        if (dev_ && bias_ && airspy().set_rf_bias) airspy().set_rf_bias(dev_, 0);   // antenna power never stays on after OnAir lets go
        bias_ = false;
        if (dev_) {
            // A radio whose stream died (unplugged) is not kept: its handle is no good any more.
            if (keepHandles() && !broken_) {
                if (airspy().is_streaming(dev_) == 1) airspy().stop_rx(dev_);
                std::lock_guard<std::mutex> lk(gKeepMu);
                auto& slot = gKept[serial_];
                if (slot && slot != dev_) airspy().close(slot);   // (cannot happen: one source per radio)
                slot = dev_;
            } else {
                airspy().close(dev_);
            }
        }
        dev_ = nullptr;
    }
    std::string gainDescription() const override {
        const int g = gainShown_.load();
        if (g < 0) return {};
        return std::string(tableShown_.load() ? "Sensitivity" : "Linearity") + " gain table, step " + std::to_string(g) + " of " + std::to_string(kGainSteps - 1);
    }
    void streamLoop() override {
        if (airspy().start_rx(dev_, &AirspySource::callback, this) != 0) { broken_ = true; return; }   // not started: opened again by the caller
        uint64_t reported = 0;
        auto lastReport = std::chrono::steady_clock::now();
        while (run_ && airspy().is_streaming(dev_) == 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            // samples the library lost on the USB link (a slow port or hub, other traffic): the stream then has gaps and nothing can lock to it
            const auto now = std::chrono::steady_clock::now();
            if (now - lastReport < std::chrono::seconds(2)) continue;
            lastReport = now;
            const uint64_t lost = lost_.load();
            if (lost != reported) {
                fprintf(stderr, "Airspy: %llu samples lost on the USB link in the last 2 s (%llu in total): use a USB 3 port without a hub\n",
                        (unsigned long long)(lost - reported), (unsigned long long)lost);
                fflush(stderr);
                reported = lost;
            }
        }
        if (run_) broken_ = true;   // the stream ended by itself: the radio is gone
        airspy().stop_rx(dev_);
    }

private:
    static int DECT2_CALL callback(AirspyApi::transfer_t* t) {
        auto* self = static_cast<AirspySource*>(t->ctx);
        if (!self->run_ || t->sample_count <= 0) return 0;
        self->lost_ += t->dropped_samples;   // the samples lost before this block (the library reports them per block)
        self->push(static_cast<const cf32*>(t->samples), (size_t)t->sample_count);   // float32 I, Q pairs: the ring's own sample type
        return 0;
    }
    uint64_t serial_;
    AirspyApi::dev_t* dev_ = nullptr;
    std::vector<uint32_t> rates_;
    bool bias_ = false;
    std::atomic<bool> broken_{false};
    std::atomic<int> gainShown_{-1};
    std::atomic<bool> tableShown_{false};
    std::atomic<uint64_t> lost_{0};
};

void listAirspy(std::vector<DeviceInfo>& out) {
    if (!airspy().ok) return;
    uint64_t serials[16] = {0};
    const int n = airspy().list_devices(serials, 16);
    for (int i = 0; i < n && i < 16; i++) {
        DeviceInfo d;
        d.kind = DeviceInfo::Native;
        d.board = "airspy";
        d.serial = serialText(serials[i]);
        d.nativeArgs = d.serial;
        std::vector<uint32_t> rates;
        {
            std::lock_guard<std::mutex> lk(gRatesMu);
            auto it = gRates.find(d.serial);
            if (it != gRates.end()) rates = it->second;
        }
        if (rates.empty()) {
            AirspyApi::dev_t* dev = nullptr;
            if (airspy().open_sn(&dev, serials[i]) == 0 && dev) {
                rates = readRates(dev);
                bool kept = false;
                if (keepHandles()) {   // the radio is opened again by the start: it gets this handle
                    std::lock_guard<std::mutex> lk(gKeepMu);
                    if (!gKept.count(serials[i])) { gKept[serials[i]] = dev; kept = true; }
                }
                if (!kept) airspy().close(dev);
                if (!rates.empty()) { std::lock_guard<std::mutex> lk(gRatesMu); gRates[d.serial] = rates; }
            }
        }
        d.name = std::string(modelOf(rates)) + " " + d.serial + " (native)";
        d.maxRateHz = 10e6; d.minRateHz = 2.5e6;
        if (!rates.empty()) {
            d.maxRateHz = *std::max_element(rates.begin(), rates.end());
            d.minRateHz = *std::min_element(rates.begin(), rates.end());
        }
        d.gainMinDb = 0; d.gainMaxDb = kGainSteps - 1;
        d.minFreqHz = kMinHz; d.maxFreqHz = kMaxHz;
        d.hasBiasTee = airspy().set_rf_bias != nullptr;
        d.settings = airspySettings();
        out.push_back(d);
    }
}

std::unique_ptr<IqSource> makeAirspy(const DeviceInfo& d) {
    return std::make_unique<AirspySource>((uint64_t)strtoull(d.nativeArgs.c_str(), nullptr, 16));
}

} // namespace native
} // namespace dect2
