#include "dect2/engine.h"
#include "dect2/dvbt.h"
#include "dect2/isdbt.h"
#include "dect2/modes.h"
#include "dect2/t2rx.h"
#include <chrono>
#include "dect2/platform.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace dect2 {

// A radio that delivers floating-point samples (LimeSDR, SoapySDR, a cf32 recording) can hand over NaN or infinite values after a
// glitch or with mismatched firmware. They would poison every filter, FFT and gain loop after them, so they become silence here.
static void dropNonFinite(cf32* x, size_t n) {
    // looks at the exponent bits of every float: all ones means NaN or infinity. One branch per block of 256 values, the loop vectorises.
    static_assert(sizeof(cf32) == 2 * sizeof(float), "cf32 layout");
    uint32_t* w = reinterpret_cast<uint32_t*>(x);
    const size_t total = 2 * n;
    for (size_t b = 0; b < total; b += 256) {
        const size_t e = std::min<size_t>(b + 256, total);
        uint32_t bad = 0;
        for (size_t i = b; i < e; i++) bad |= (uint32_t)((w[i] & 0x7F800000u) == 0x7F800000u);
        if (bad)
            for (size_t i = b & ~(size_t)1; i < e; i += 2)
                if ((w[i] & 0x7F800000u) == 0x7F800000u || (w[i + 1] & 0x7F800000u) == 0x7F800000u) { w[i] = 0; w[i + 1] = 0; }
    }
}

// The receiver that runs for an engine standard code (setStandard): 0 and 1 are DVB-T2, 2 DVB-T, ... so it is the code minus one
static int activeStdFor(int stdMode) { return stdMode >= 2 ? stdMode - 1 : 0; }

// "Remove DC spike" and "IQ correction" of the samples (iq_correct.h), with the part skipped that would harm the active mode (tests/test_iq_modes.cpp):
// the DC removal where the wanted carrier can sit on the centre (FM, and the modes whose ModeTuning says carrierAtCentre), the IQ correction
// where the signal is not circular around the centre (ModeTuning::notCircular)
static void cleanSamples(IqCorrector& fix, OffsetMixer& mix, bool forceDc, cf32* x, size_t n, int activeStd) {
    const ModeTuning* mt = modeTuning(activeStd + 1);
    fix.process(x, n, activeStd == 6 || (mt && mt->carrierAtCentre), mt && mt->notCircular, forceDc);
    mix.mix(x, n);   // offset tuning: the channel back to the centre, after the IQ correction (the imbalance mirrors around the radio's centre)
}

// Offset tuning only where one channel sits on the centre: the TV standards, DAB, FM, DVB-S, DTMB, DMR, DRM, ADS-B, GNSS and radiosondes.
// Not analog TV (its own DC handling, carriers both sides), and not the modes that already tune off the channel or watch several channels.
bool Engine::offsetAllowed() const { return true; }

void Engine::ingest(cf32* x, size_t n) {
    rec_.push(x, n);   // the raw samples, before anything touches them
    dropNonFinite(x, n);
    cleanSamples(iqFix_, offMix_, offsetDc_.load(), x, n, activeStd_.load());
    const auto a0 = std::chrono::steady_clock::now();
    lastSamples_ = a0;   // the radio is alive, whether or not the resampler gives a sample out of this block
    if (offResample_) { offBuf_.clear(); offRs_.process(x, n, offBuf_); x = offBuf_.data(); n = offBuf_.size(); }
    if (!n) return;
    feedSpectrum(x, n);
    feedRx(x, n);
    tRx_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - a0).count();
    nSamp_ += n;
}

Engine::Engine() : t0_(std::chrono::steady_clock::now()) {}
Engine::~Engine() { stop(); }

void Engine::log(const std::string& line) {
    double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0_).count();
    char buf[32];
    snprintf(buf, sizeof buf, "%7.2fs ", t);
    std::lock_guard<std::mutex> lk(logMu_);
    log_.push_back(buf + line);
    if (log_.size() > 5000) log_.erase(log_.begin(), log_.begin() + 1000);
}

std::string Engine::loadProfile() const {
    char b[200];
    snprintf(b, sizeof b, "analysis thread: receiver %.2f s for %.2f s of signal processed (the spectrum thread used %.2f s)", tRx_, nSamp_ / std::max(1.0, rate_.load()), tSpec_);
    return b;
}

std::vector<std::string> Engine::logSnapshot(size_t& total) {
    std::lock_guard<std::mutex> lk(logMu_);
    total = log_.size();
    return log_;
}

void Engine::clearLog() {
    std::lock_guard<std::mutex> lk(logMu_);
    log_.clear();
}

bool Engine::start(const DeviceInfo& dev, const TuneSettings& tune, const FileOptions& file) {
    stop();
    ring_.clear();
    ring_.resetDropped();
    radioMissing_ = 0; radioMissEvents_ = 0; cpuDropEvents_ = 0; ringDroppedSeen_ = 0;
    lastRadioMissNs_ = 0; lastCpuDropNs_ = 0; loadPct_ = 0; loadT_ = 0;
    holdDelivery(1000);   // a radio often stutters while its stream starts
    std::string err;
    if (dev.kind == DeviceInfo::File)
        src_ = makeFileSource(file.path, file.format, file.sampleRate, file.loop);
    else
        src_ = makeSource(dev);
    if (!src_) { log("no source"); return false; }
    offHz_ = 0; offRate_ = 0; offsetDc_ = false;
    std::string offNote;
    if (autoOffset_ && dev.isRadio() && offsetAllowed()) {
        // a HackRF has no listed maximum (20 Msps); the HackRF Pro tunes exactly only at up to 10 Msps and at 20, so it gets 20 above 10
        const double maxRate = dev.maxRateHz > 0 ? dev.maxRateHz : dev.kind == DeviceInfo::HackRF ? 20e6 : 0;
        OffsetPlan p = planOffset(tune.bandwidthMhz * 1e6, tune.sampleRate, maxRate, dev.minRateHz);
        if (p.ok && dev.kind == DeviceInfo::HackRF && p.rateHz > 10e6) p.rateHz = 20e6;
        if (p.ok) { offHz_ = p.offsetHz; offRate_ = p.rateHz; }
        else { offsetDc_ = true; offNote = "offset tuning not possible (" + p.why + "): removing the DC spike instead"; }
    }
    if (!src_->start(radioTune(tune), ring_, err)) {
        log("source start failed: " + err);
        src_.reset();
        return false;
    }
    if (offHz_ > 0 && src_->sampleRate() / 2 < offHz_ + tune.bandwidthMhz * 0.5e6) {
        // the radio did not take the rate the offset needs: back on the channel, with the DC removal
        offNote = "offset tuning not possible (the radio runs at " + std::to_string(src_->sampleRate() / 1e6).substr(0, 5) + " Msps): removing the DC spike instead";
        offHz_ = 0; offRate_ = 0; offsetDc_ = true;
        std::string e2;
        if (!src_->retune(radioTune(tune), e2)) log("retune failed: " + e2);
    }
    { std::lock_guard<std::mutex> lk(noteMu_); note_.clear(); retuneNote_.clear(); }
    setNote(offNote.empty() ? err : err.empty() ? offNote : err + "; " + offNote, false);
    srcRate_ = src_->sampleRate();
    rate_ = srcRate_;
    live_ = src_->realtimeHardware();
    offMix_.set(offHz_, srcRate_);
    offResample_ = false;
    if (offHz_ > 0) {
        // back to the mode's rate after the shift (if the radio runs faster than the mode asked for)
        if (tune.sampleRate > 0 && srcRate_ > tune.sampleRate * 1.001 && offRs_.configure(srcRate_, tune.sampleRate)) { offRs_.reset(); offResample_ = true; rate_ = tune.sampleRate; }
        char b[200];
        snprintf(b, sizeof b, "offset tuning: the radio sits %.3f MHz below the channel at %.2f Msps, its DC spike outside the channel; the receiver gets %.3f Msps",
                 offHz_.load() / 1e6, srcRate_ / 1e6, rate_.load() / 1e6);
        log(b);
    }
    {
        std::lock_guard<std::mutex> lk(tuneMu_);
        lastDev_ = dev; lastTune_ = tune; lastFile_ = file;
    }
    radioLost_ = false; lastSamples_ = std::chrono::steady_clock::now(); reconnectErr_.clear();
    logP1Count_ = 0; logGi_ = -2; logState_ = -1; logFrameSyms_ = 0;
    bwMhz_ = tune.bandwidthMhz; bwActive_ = bwMhz_;
    bwDetected_ = 0; bwReset_ = true; bwReq_ = false;
    rxDvbs_.configure(rate_);
    rxDvbs_.setPacketCallback([this](const uint8_t* pk, size_t n, double secs) { onTsPackets(pk, n, secs); });
    rxDvbs_.setLogCallback([this](const std::string& message) { log(message); });
    modeSeq_[0] = 0;
    activeStd_ = 7;
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        demux_.reset();
        unpack_ = BbUnpacker();
        tsSnap_ = TsSnapshot();
    }
    char b[160];
    snprintf(b, sizeof b, "source started: %s  fs=%.4f Msps", dev.name.c_str(), rate_.load() / 1e6);
    log(b);
    if (dev.isGeneric()) {
        snprintf(b, sizeof b, "tuned %.3f MHz  gain %.0f dB", tune.centerHz / 1e6, tune.gainDb);
        log(b);
    }
    if (dev.kind == DeviceInfo::HackRF) {
        snprintf(b, sizeof b, "tuned %.3f MHz  LNA %d dB  VGA %d dB  amp %s", tune.centerHz / 1e6, tune.lnaDb, tune.vgaDb, tune.ampOn ? "on" : "off");
        log(b);
    }
    tSpec_ = tRx_ = 0; nSamp_ = 0;
    stopReq_ = false;
    running_ = true;
    specStop_ = false;
    specQ_.clear();
    specTh_ = std::thread([this] { spectrumLoop(); });
    th_ = std::thread([this] { analysisLoop(); });
    return true;
}

void Engine::stop() {
    player_.select(-1);
    if (!running_ && !src_) return;
    stopReq_ = true;
    if (th_.joinable()) th_.join();
    rec_.stop();   // a recording ends with the radio (the analysis thread is gone, nothing pushes any more)
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        specStop_ = true;
    }
    specQCv_.notify_all();
    if (specTh_.joinable()) specTh_.join();
    live_ = false;
    if (src_) {
        src_->stop(); src_.reset();
        char b[160];
        double dur = nSamp_ / std::max(1.0, rate_.load());
        snprintf(b, sizeof b, "source stopped (CPU load: spectrum %.0f%%, receiver %.0f%% of real time)", 100 * tSpec_ / std::max(1e-9, dur), 100 * tRx_ / std::max(1e-9, dur));
        log(b);
    }
    running_ = false;
}

bool Engine::startRecording(const std::string& path, FileFormat fmt, std::string& err) {
    DeviceInfo::Kind kind;
    { std::lock_guard<std::mutex> lk(tuneMu_); kind = lastDev_.kind; }
    if (!running_ || !src_) { err = "start the radio first"; return false; }
    if (kind == DeviceInfo::File) { err = "recording a file playback is not needed: the file is already there"; return false; }
    return rec_.start(path, fmt, srcRate_, err);
}

std::string Engine::recordingName(FileFormat fmt) const {
    TuneSettings t;
    { std::lock_guard<std::mutex> lk(tuneMu_); t = lastTune_; }
    return recordingFileName(recordingModeId(stdMode_.load()), radioTune(t).centerHz, srcRate_, fmt);
}

// The radio of a narrow-channel mode is tuned tuneOffsetHz above the user's frequency, so the channel stays off the DC spike.
TuneSettings Engine::radioTune(const TuneSettings& t) const {
    TuneSettings r = t;
    if (const ModeTuning* mt = modeTuning(stdMode_.load())) r.centerHz += mt->tuneOffsetHz;
    if (offHz_ > 0) { r.centerHz -= offHz_; r.sampleRate = offRate_; r.basebandFilterHz = 0; }   // offset tuning: the radio below the channel
    return r;
}

bool Engine::retune(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    holdDelivery(300);
    if (!src_->retune(radioTune(tune), err)) { log("retune failed: " + err); return false; }
    setNote(err, true);
    if (restartIfRateChanged(tune)) return running_;
    { std::lock_guard<std::mutex> lk(tuneMu_); lastTune_ = tune; }
    return true;
}

// Every receiver was set up for the rate the radio had at start(); a retune that changes it (another rate asked for, or the radio
// picking a different one) would leave them decoding at the wrong rate, so the engine starts again with the new settings
// A driver note is logged when it is new (a retune that brings the same one again, as the AGC does, does not fill the log)
void Engine::setNote(const std::string& n, bool retune) {
    {
        std::lock_guard<std::mutex> lk(noteMu_);
        std::string& slot = retune ? retuneNote_ : note_;
        if (n == slot) return;
        slot = n;
    }
    if (!n.empty()) log("note: " + n);
}

bool Engine::restartIfRateChanged(const TuneSettings& tune) {
    if (!src_ || std::fabs(src_->sampleRate() - srcRate_) < 0.5) return false;
    char b[160];
    snprintf(b, sizeof b, "the radio now runs at %.3f Msps (was %.3f): restarting the receivers", src_->sampleRate() / 1e6, srcRate_ / 1e6);
    log(b);
    const DeviceInfo dev = lastDev_;
    const FileOptions file = lastFile_;
    stop();
    start(dev, tune, file);
    return true;
}

bool Engine::retuneReset(const TuneSettings& tune) {
    if (!src_) return false;
    std::string err;
    holdDelivery(300);
    if (!src_->retune(radioTune(tune), err)) { log("retune failed: " + err); return false; }
    setNote(err, true);
    if (restartIfRateChanged(tune)) return running_;
    { std::lock_guard<std::mutex> lk(tuneMu_); lastTune_ = tune; }
    resetReq_ = true;
    return true;
}

void Engine::applyReset() {
    ring_.clear();
    holdDelivery(300);
    rxDvbs_.reset();
    autoMark_ = nSamp_ / std::max(1.0, rate_.load()); lastLockSec_ = autoMark_;
    analyzer_.reset();
    {
        std::lock_guard<std::mutex> lk(tsMu_);
        demux_.reset();
        unpack_ = BbUnpacker();
        unpack_.setSink([this](const uint8_t* pkt) { demux_.feed(pkt); ttx_.feedTs(pkt); frameBuf_.insert(frameBuf_.end(), pkt, pkt + 188); });
        tsSnap_ = TsSnapshot();
        lastT2Frame_ = -1;
        frameBuf_.clear();
    }
    {
        std::lock_guard<std::mutex> lk(specMu_);
        spec_ = SpectrumFrame();
    }
    {
        std::lock_guard<std::mutex> lk(rxMu_);
        rxTel_ = RxTelemetry();
    }
    logP1Count_ = 0; logGi_ = -2; logState_ = -1; logFrameSyms_ = 0;
    resets_++;
}

void Engine::onTsPackets(const uint8_t* pk, size_t n, double secs) {
    std::lock_guard<std::mutex> lk(tsMu_);
    for (size_t i = 0; i < n; i++) {
        const uint8_t* q = pk + i * 188;
        demux_.feed(q);
        ttx_.feedTs(q);
    }
    demux_.advance(secs);
    tsSnap_ = demux_.snapshot();
    outputs_.packets(pk, n, &tsSnap_);
    if (tap_) tap_(pk, n, &tsSnap_);
    if (player_.selected() >= 0) player_.push(pk, n, tsSnap_);
    outputs_.burstDone(secs);
}

// Auto mode alternates between the two receivers until one locks, then stays with it. Only the active receiver sees samples,
// so idle searching costs one receiver, not two.
void Engine::feedRx(const cf32* x, size_t n) {
    rxDvbs_.feed(x, n);
}

// While nothing is locked: average the spectrum, measure the width of the flat top and switch to that channel bandwidth

namespace {
// How long the analysis thread waits for, and holds, the transport-stream lock while it hands a decoded frame on (see engineWaitProfile()).
std::atomic<uint64_t> gPlpFrames{0}, gPlpWaitUs{0}, gPlpHoldUs{0}, gPlpPushUs{0};
inline uint64_t usSince(std::chrono::steady_clock::time_point t) { return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t).count(); }
}

std::string engineWaitProfile() {
    char b[200];
    snprintf(b, sizeof b, "analysis thread, per decoded frame: lock wait %.2f ms, lock held %.2f ms (player push %.2f ms); %llu frames", 1e-3 * gPlpWaitUs / std::max<uint64_t>(1, gPlpFrames), 1e-3 * gPlpHoldUs / std::max<uint64_t>(1, gPlpFrames), 1e-3 * gPlpPushUs / std::max<uint64_t>(1, gPlpFrames), (unsigned long long)gPlpFrames.load());
    return b;
}

void Engine::setPacketTap(std::function<void(const uint8_t*, size_t, const TsSnapshot*)> f) { std::lock_guard<std::mutex> lk(tsMu_); tap_ = std::move(f); }
TsSnapshot Engine::tsSnapshot() { std::lock_guard<std::mutex> lk(tsMu_); return tsSnap_; }
std::map<int, std::vector<EpgEvent>> Engine::epg() {
    std::lock_guard<std::mutex> lk(tsMu_);
    return demux_.epg();
}

BbStats Engine::bbStats() { std::lock_guard<std::mutex> lk(tsMu_); return unpack_.stats(); }

bool Engine::latestRx(RxTelemetry& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(rxMu_);
    if (rxTel_.seq <= lastSeq) return false;
    out = rxTel_;
    return true;
}

bool Engine::latestSpectrum(SpectrumFrame& out, uint64_t lastSeq) {
    std::lock_guard<std::mutex> lk(specMu_);
    if (spec_.seq <= lastSeq || spec_.dbfs.empty()) return false;
    out = spec_;
    return true;
}

// A radio that was delivering samples and stops (the cable was pulled) is reported, and a HackRF is opened again with the settings in use
// as soon as it is back, so that the receiver carries on by itself.
void Engine::watchRadio() {
    if (!src_ || !src_->realtimeHardware()) return;
    using namespace std::chrono;
    const auto now = steady_clock::now();
    if (!radioLost_) {
        if (now - lastSamples_ > seconds(2)) {
            radioLost_ = true;
            nextReconnect_ = now + seconds(1);
            log(lastDev_.kind == DeviceInfo::HackRF ? "radio stopped sending samples (unplugged?) - waiting for it to come back" : "radio stopped sending samples - press Stop and Start to open it again");
        }
        return;
    }
    if (lastDev_.kind != DeviceInfo::HackRF || now < nextReconnect_) return;
    nextReconnect_ = now + seconds(2);
    TuneSettings tune;
    { std::lock_guard<std::mutex> lk(tuneMu_); tune = lastTune_; }
    src_->stop();
    std::string err;
    if (src_->start(radioTune(tune), ring_, err)) {
        radioLost_ = false;
        lastSamples_ = steady_clock::now();
        reconnectErr_.clear();
        resetReq_ = true;   // the receiver starts again from the new samples
        log("radio reconnected");
    } else if (err != reconnectErr_) {
        reconnectErr_ = err;
        log("radio not available yet: " + err);
    }
}

// The receiver cannot keep up (the ring is more than half backlog): jump to the newest samples in one go. One clean gap that the receiver
// resynchronises after is far better than the radio thread dropping a few samples out of every chunk, which ruins every frame.
// Only for live radios: a recording or the synthetic signal waits for a slow receiver instead (see the paced source), and must never lose samples.
void Engine::catchUp() {
    if (!src_ || !src_->realtimeHardware()) return;
    const size_t skipped = ring_.dropBacklog();
    if (!skipped) return;
    skippedSamples_ += skipped;
    skipEvents_++;
    const auto now = std::chrono::steady_clock::now();
    if (now - lastSkipLog_ > std::chrono::seconds(10)) {
        lastSkipLog_ = now;
        char b[200];
        snprintf(b, sizeof b, "the receiver cannot keep up with the sample rate: skipped %.2f s of signal (%llu skips so far). Close other programs, use a narrower channel or a lower sample rate", skipped / std::max(1.0, srcRate_), (unsigned long long)skipEvents_);
        log(b);
    }
}

static int64_t steadyNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void Engine::holdDelivery(int ms) {
    delivHoldNs_ = steadyNs() + (int64_t)ms * 1000000;
    delivReanchor_ = true;
}

// Samples that never came out of the radio (delivery_check.h)
void Engine::checkDelivery() {
    if (!src_ || !src_->realtimeHardware() || srcRate_ <= 0) return;
    if (radioLost_) { holdDelivery(500); return; }   // unplugged: that is reported as such, and the gap is not a loss of the USB link
    uint64_t off; int64_t ns; size_t block;
    if (!ring_.lastWrite(off, ns, block)) return;
    if (delivReanchor_.exchange(false)) deliv_.restart(delivHoldNs_.load());
    const double step = deliv_.observe(off, ns, srcRate_, (double)block);
    if (step > 0) {
        radioMissing_ = radioMissing_.load() + step;
        radioMissEvents_++;
        const int64_t now = steadyNs();
        lastRadioMissNs_ = now;
        if (now - lastRadioLogNs_.load() > 10000000000LL) {
            lastRadioLogNs_ = now;
            char b[240];
            snprintf(b, sizeof b, "the radio did not deliver %.0f ms of signal (%.2f s in all): its USB connection or driver could not keep up. Try another USB port (not a hub), a shorter cable or a lower sample rate",
                     1000 * step / srcRate_, radioMissing_.load() / srcRate_);
            log(b);
        }
    }
}

SampleLoss Engine::sampleLoss() const {
    SampleLoss l;
    l.live = live_.load();
    const double r = std::max(1.0, srcRate_);   // the ring holds the radio's samples (before any resampling)
    l.radioSec = radioMissing_.load() / r;
    l.cpuSec = (double)ring_.dropped() / r;
    l.radioEvents = radioMissEvents_.load();
    l.cpuEvents = cpuDropEvents_.load();
    const int64_t now = steadyNs();
    if (lastRadioMissNs_.load()) l.sinceRadioSec = (double)(now - lastRadioMissNs_.load()) * 1e-9;
    if (lastCpuDropNs_.load()) l.sinceCpuSec = (double)(now - lastCpuDropNs_.load()) * 1e-9;
    l.loadPct = loadPct_.load();
    return l;
}

void Engine::feedSpectrum(const cf32* x, size_t n) {
    {
        std::lock_guard<std::mutex> lk(specQMu_);
        if (specQ_.size() >= 6) return;   // the display is statistical: when the spectrum thread is behind, skip a piece
        specQ_.emplace_back(x, x + n);
    }
    specQCv_.notify_one();
}

void Engine::spectrumLoop() {
    std::vector<cf32> chunk;
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(specQMu_);
            specQCv_.wait(lk, [this] { return specStop_ || !specQ_.empty(); });
            if (specQ_.empty()) return;   // stopping
            chunk = std::move(specQ_.front());
            specQ_.pop_front();
        }
        const auto a0 = std::chrono::steady_clock::now();
        analyzer_.feed(chunk.data(), chunk.size());
        tSpec_ += std::chrono::duration<double>(std::chrono::steady_clock::now() - a0).count();
    }
}

void Engine::analysisLoop() {
    setThreadPriority(ThreadPriority::Realtime); // the sample path must never wait for decoders or the UI
    std::vector<cf32> buf(1 << 16);
    auto next = std::chrono::steady_clock::now();
    SpectrumFrame f;
    uint64_t rxSeq = 0;
    while (!stopReq_) {
        if (resetReq_.exchange(false)) { applyReset(); rxSeq = 0; }
        size_t n;
        catchUp();
        while ((n = ring_.read(buf.data(), buf.size())) > 0) { ingest(buf.data(), n); if (std::chrono::steady_clock::now() > next + std::chrono::milliseconds(250)) break; } // keep publishing spectrum/telemetry even when the receiver is behind
        next += std::chrono::milliseconds(33);
        watchRadio();
        checkDelivery();
        if (const uint64_t d = ring_.dropped(); d != ringDroppedSeen_.load()) { ringDroppedSeen_ = d; cpuDropEvents_++; lastCpuDropNs_ = steadyNs(); }
        {   // the receiver's load: its time on this thread against the signal time it handled, over about two seconds
            const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
            if (loadT_ == 0 || nSamp_ < loadSamp_) { loadT_ = now; loadRx_ = tRx_; loadSamp_ = nSamp_; }
            else if (now - loadT_ >= 2.0) {
                const double sig = (nSamp_ - loadSamp_) / std::max(1.0, rate_.load());
                if (sig > 0.05) loadPct_ = (float)(100.0 * (tRx_ - loadRx_) / sig);
                loadT_ = now; loadRx_ = tRx_; loadSamp_ = nSamp_;
            }
        }
        {
            RxTelemetry t;
                DvbsTelemetry mt;
                if (rxDvbs_.telemetry(mt, modeSeq_[0])) {
                    modeSeq_[0] = mt.seq;
                    t.standard = 7;
                    t.seq = mt.seq;
                    t.state = mt.state;
                    t.cfoHz = mt.cfoHz;
                    t.dataValid = mt.dataValid;
                    t.dataSnrDb = mt.snrDb;
                    t.blocksOk = mt.blocksOk; t.blocksBad = mt.blocksBad;
                    t.rateOk = rxDvbs_.ready();
                    t.dvbs = std::move(mt);
                    std::lock_guard<std::mutex> lk(rxMu_);
                    publishRx(std::move(t));
                }

        }
        if (analyzer_.takeFrame(f)) {
            {
                std::lock_guard<std::mutex> lk(specMu_);
                spec_ = f;
            }
        }
        // keep reading while we wait so the ring never fills
        while (std::chrono::steady_clock::now() < next && !stopReq_) {
            catchUp();
            size_t m = ring_.read(buf.data(), buf.size());
            if (m) ingest(buf.data(), m);
            else std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }
}

} // namespace dect2
