#pragma once
#include <array>
#include "source.h"
#include "dvbs_tel.h"
#include "spectrum.h"
#include "ts.h"
#include <condition_variable>
#include <deque>
#include <mutex>

namespace dect2 {
struct MultiConfig {
    size_t maxChannels = 4;
    double sampleRate = 10e6, centerIfHz = 745e6, lnbHz = 9750e6, correctionHz = 0;
    double filterHz = 0;   // the radio's analog filter width (0 = 8 MHz); only radios that have one act on it (Pluto, Lime, HackRF)
};
struct ChannelConfig {
    double satelliteMhz = 10491.5, symbolRate = 1500000;
    int standard = 0;
    bool enabled = false;
};
struct ChannelStatus {
    unsigned id = 0;
    uint64_t revision = 0, droppedSamples = 0;
    ChannelConfig config;
    DvbsTelemetry telemetry;
    TsSnapshot transport;
    double levelDbfs = -120, outputRate = 0, packetAgeSeconds = -1;
    std::string error;
};
struct SharedGainStatus {
    bool automatic = true, available = false;
    double gain = 30, minimum = 0, maximum = 100;
    uint64_t overloads = 0;
    std::string status = "Auto ready";
    std::string detail;   // the radio's own stages the gain number set, in words ("LNA state 2 · IF gain reduction 38 dB")
};
// A bounded subscriber never blocks IQ reception or another channel.
struct ChannelStream {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> queue;
    size_t bytes = 0;
    bool closed = false;
};
class MultiReceiver {
public:
    explicit MultiReceiver(MultiConfig config);
    ~MultiReceiver();
    unsigned create(ChannelConfig config);
    bool configure(unsigned id, ChannelConfig config, std::string& error);
    std::vector<ChannelStatus> channels() const;
    std::shared_ptr<ChannelStream> subscribe(unsigned id);
    bool start(const DeviceInfo& device, TuneSettings tune, std::string& error);
    bool startDemo(std::string& error, int secondStandard = 1);
    void stop();
    void setGain(bool automatic, double gain);
    SharedGainStatus gainStatus() const;
    bool running() const;
    double sampleRate() const;
    uint64_t sourceDrops() const;
    std::string sourceError() const;
    bool spectrum(SpectrumFrame& frame, uint64_t lastSequence = 0) const;
    const MultiConfig& config() const;
    // The frequency correction (Hz, IF side): what the LNB and the radio are off by. Applies to the running channels at once.
    void setCorrection(double hz);
    // Moves the shared capture (the radio's centre, as IF Hz) while it runs: the channels keep their satellite frequencies
    bool setCenterIf(double hz);
    // The three capture positions as the real satellite frequency (MHz) of the window's centre: left, centre, right. For a window as wide as the
    // band (10 Msps) the window's edge is the band's edge; for a narrower one (a radio whose link is slower) the position leaves room for a
    // 1.5 MS/s channel at 10491.5 on the left and for a 333 kS/s channel at 10499.4 on the right, so that those can be tuned.
    // (the IF to set is  centre*1e6 - lnb + correction: the correction moves the real window)
    std::array<double,3> alignTargetsMhz() const;
    // Test/file adapters can supply exactly the same shared IQ blocks as hardware.
    void feed(const cf32* samples, size_t count);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
} // namespace dect2
