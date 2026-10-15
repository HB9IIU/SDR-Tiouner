// Recording the radio's IQ samples (iq_record.h, Engine::startRecording): 3 s of the built-in DVB-S2 test signal in both formats. The files have the
// size the length and the rate say, the name reads back with guessSampleRate() / guessFormat(), nothing was dropped, and the float file played
// back as a file gives the same receiver state as the live signal. A file source and a stopped engine refuse to record.
#include "dect2/engine.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <thread>

using namespace dect2;
namespace fs = std::filesystem;

static int fails = 0;
#define CHECK(c, m) do { if (!(c)) { printf("FAIL: %s\n", m); fails++; } } while (0)

static const double kRate = 2e6;

static TuneSettings dvbsTune() {   // a QO-100 DATV channel: DVB-S2 QPSK, 1 Msym/s
    TuneSettings t;
    t.sampleRate = kRate;
    t.centerHz = 741.5e6;
    t.bandwidthMhz = 1.5;
    t.synth.mode = 8;
    t.synth.snrDb = 20;
    t.synth.modeVal[0] = 1e6;
    return t;
}

// highest DVB-S state (0 searching, 1 tracking, 2 locked) the engine reported while we waited secs
static int watchDvbs(Engine& e, double secs) {
    int best = 0;
    uint64_t seq = 0;
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < secs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        RxTelemetry t;
        if (e.latestRx(t, seq)) { seq = t.seq; if (t.standard == 7) best = std::max(best, t.dvbs.state); }
    }
    return best;
}

static uint64_t record(const fs::path& dir, FileFormat fmt, std::string& path, double& wall, RecordingStats& st, int& liveState) {
    Engine e;
    e.setStandard(8);
    e.player().setMuted(true);
    DeviceInfo dev; dev.kind = DeviceInfo::Synthetic; dev.name = "test signal";
    std::string err;
    CHECK(!e.startRecording((dir / "x.cs8").string(), fmt, err) && !err.empty(), "a stopped engine refuses to record");
    CHECK(e.start(dev, dvbsTune(), FileOptions()), "engine started");
    path = (dir / e.recordingName(fmt)).string();
    CHECK(e.startRecording(path, fmt, err), "recording started");
    const auto t0 = std::chrono::steady_clock::now();
    liveState = watchDvbs(e, 3.0);
    st = e.recordingStats();
    e.stopRecording();
    wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    st = e.recordingStats();
    CHECK(!st.active, "the recording is stopped");
    e.stop();
    // The operating system may still be finishing the last write (and a virus scanner may hold the file for a moment):
    // wait up to a second for the size on disk to reach what the recorder reports.
    uint64_t onDisk = 0;
    for (int i = 0; i < 20; ++i) {
        std::error_code ec;
        onDisk = fs::exists(path, ec) ? (uint64_t)fs::file_size(path, ec) : 0;
        if (onDisk == st.bytes) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return onDisk;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "onair_iq_record_test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::string pCs8, pCf32;
    double wCs8 = 0, wCf32 = 0;
    RecordingStats sCs8, sCf32;
    int liveCs8 = 0, liveCf32 = 0;
    const uint64_t bCs8 = record(dir, FileFormat::CS8, pCs8, wCs8, sCs8, liveCs8);
    const uint64_t bCf32 = record(dir, FileFormat::CF32, pCf32, wCf32, sCf32, liveCf32);
    printf("  cs8:  %s  %llu bytes in %.2f s, recorded %.2f s, dropped %llu, DVB-S state %d\n", fs::path(pCs8).filename().string().c_str(), (unsigned long long)bCs8, wCs8, sCs8.seconds, (unsigned long long)sCs8.droppedSamples, liveCs8);
    printf("  cf32: %s  %llu bytes in %.2f s, recorded %.2f s, dropped %llu, DVB-S state %d\n", fs::path(pCf32).filename().string().c_str(), (unsigned long long)bCf32, wCf32, sCf32.seconds, (unsigned long long)sCf32.droppedSamples, liveCf32);

    CHECK(std::abs(bCs8 - wCs8 * kRate * 2) < 0.05 * wCs8 * kRate * 2, "CS8 size = seconds x rate x 2 bytes (5 %)");
    CHECK(std::abs(bCf32 - wCf32 * kRate * 8) < 0.05 * wCf32 * kRate * 8, "CF32 size = seconds x rate x 8 bytes (5 %)");
    CHECK(bCs8 == sCs8.bytes && bCf32 == sCf32.bytes, "the reported size is the file size");
    CHECK(sCs8.droppedSamples == 0 && sCf32.droppedSamples == 0, "nothing dropped");
    CHECK(std::abs(guessSampleRate(pCs8) - kRate) < 1 && std::abs(guessSampleRate(pCf32) - kRate) < 1, "the name gives the sample rate back");
    CHECK(guessFormat(pCs8) == FileFormat::CS8 && guessFormat(pCf32) == FileFormat::CF32, "the name gives the format back");
    CHECK(fs::path(pCf32).filename().string().rfind("onair_dvbs_741.500MHz_2Msps_", 0) == 0, "the name has the mode, the frequency and the rate");
    CHECK(std::abs(guessSampleRate("onair_dvb_554.000MHz_10Msps_20261009-071530.cs8") - 10e6) < 1 && std::abs(guessSampleRate("onair_dvbs_10491.500MHz_2.048Msps_x.cf32") - 2.048e6) < 1, "the documented file names parse");
    CHECK(liveCs8 >= 1 && liveCf32 >= 1, "the live signal was received");

    // the float file played back as a file: the same receiver state as the live signal
    int replay = 0;
    {
        Engine e;
        e.setStandard(8);
        e.player().setMuted(true);
        DeviceInfo dev; dev.kind = DeviceInfo::File; dev.name = pCf32;
        FileOptions fo;
        fo.path = pCf32; fo.format = guessFormat(pCf32); fo.sampleRate = guessSampleRate(pCf32); fo.loop = false;
        CHECK(e.start(dev, dvbsTune(), fo), "the recording opens as a file");
        std::string err;
        CHECK(!e.startRecording((dir / "y.cs8").string(), FileFormat::CS8, err) && !err.empty(), "a file source refuses to record");
        printf("  refusal: %s\n", err.c_str());
        replay = watchDvbs(e, 4.0);
        e.stop();
    }
    printf("  replay of the float file: DVB-S state %d (live %d)\n", replay, liveCf32);
    CHECK(replay == liveCf32, "the replay gives the same receiver state as the live signal");

    fs::remove_all(dir);
    printf(fails ? "iq record: FAILED\n" : "iq record: ok\n");
    return fails ? 1 : 0;
}
