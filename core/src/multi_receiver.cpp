#include "dect2/multi_receiver.h"
#include "dect2/dvbs_rx.h"
#include "dect2/dvbs_gen.h"
#include "dvbs_dsp.h"
#include "dect2/wideband_gain.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <optional>

namespace dect2 {
namespace {
using Clock = std::chrono::steady_clock;
using Block = std::shared_ptr<const std::vector<cf32>>;
// ~420 ms at 10 Msps: enough to absorb initial FEC hypothesis searches while
// bounding live backlog. Eight blocks (~26 ms) cannot cover DVB-S acquisition.
constexpr size_t maxBlocks = 128, maxStreamBytes = 1u << 20;
struct Channel {
    unsigned id;
    MultiConfig capture;
    std::atomic<double>& inputRate;
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::deque<Block> blocks;
    ChannelStatus status;
    std::vector<std::weak_ptr<ChannelStream>> subscribers;
    uint64_t revision = 1;
    bool quit = false, discontinuity = false;
    Clock::time_point lastPacket{};
    std::thread worker;
    DvbsReceiver receiver;
    TsDemux demux;

    Channel(unsigned number, const MultiConfig& config, std::atomic<double>& rate, ChannelConfig channel)
        : id(number), capture(config), inputRate(rate) {
        status.id = id; status.config = channel; status.revision = revision;
        receiver.setPacketCallback([this](const uint8_t* pk, size_t count, double secs) {
            std::lock_guard<std::mutex> lock(mutex);
            if (!status.config.enabled || !count) return;
            lastPacket = Clock::now();
            for (size_t i = 0; i < count; ++i) demux.feed(pk + i * 188);
            demux.advance(secs); status.transport = demux.snapshot();
            for (auto it = subscribers.begin(); it != subscribers.end();) {
                auto subscriber = it->lock();
                if (!subscriber) { it = subscribers.erase(it); continue; }
                std::lock_guard<std::mutex> clientLock(subscriber->mutex);
                if (!subscriber->closed) {
                    // A slow publisher reconnects at live time, rather than replaying old video.
                    if (subscriber->bytes + count * 188 > maxStreamBytes) {
                        subscriber->closed = true; subscriber->queue.clear(); subscriber->bytes = 0;
                    } else {
                        subscriber->queue.emplace_back(pk, pk + count * 188);
                        subscriber->bytes += count * 188;
                    }
                    subscriber->cv.notify_all();
                }
                ++it;
            }
        });
        worker = std::thread([this] {
            for (;;) {
                run();
                std::unique_lock<std::mutex> lock(mutex);
                if (quit) break;
                // An explicit new channel command can retry a failed worker.
                const auto failedRevision = revision;
                if (!status.error.empty()) cv.wait(lock, [&] { return quit || revision != failedRevision; });
                if (quit) break;
            }
        });
    }
    ~Channel() {
        { std::lock_guard<std::mutex> lock(mutex); quit = true; closeStreams(); }
        cv.notify_all(); if (worker.joinable()) worker.join();
        // The receiver joins its decoder threads before callback targets are destroyed.
        receiver.setPacketCallback({}); receiver.configure(0);
    }
    void closeStreams() { // mutex held
        for (auto& weak : subscribers) if (auto stream = weak.lock()) {
            std::lock_guard<std::mutex> lock(stream->mutex); stream->closed = true;
            stream->queue.clear(); stream->bytes = 0; stream->cv.notify_all();
        }
        subscribers.clear();
    }
    void set(ChannelConfig config) {
        { std::lock_guard<std::mutex> lock(mutex);
          status.config = config; status.revision = ++revision;
          status.telemetry = {}; status.transport = {}; status.error.clear(); lastPacket = {};
          blocks.clear(); closeStreams(); }
        cv.notify_all();
    }
    // A new frequency correction (the calibration): the channel is configured again from its mixer on.
    void setCorrection(double hz) {
        { std::lock_guard<std::mutex> lock(mutex);
          capture.correctionHz = hz; status.revision = ++revision;
          status.telemetry = {}; status.transport = {}; status.error.clear(); lastPacket = {};
          blocks.clear(); closeStreams(); }
        cv.notify_all();
    }
    // A new centre of the shared capture: the channel's mixer is set up again for it
    void setCenter(double hz) {
        { std::lock_guard<std::mutex> lock(mutex);
          capture.centerIfHz = hz; status.revision = ++revision;
          status.telemetry = {}; status.transport = {}; status.error.clear(); lastPacket = {};
          blocks.clear(); closeStreams(); }
        cv.notify_all();
    }
    void push(const Block& block) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!status.config.enabled || !status.error.empty()) return;
        if (blocks.size() >= maxBlocks) {
            for (const auto& queued : blocks) status.droppedSamples += queued->size();
            blocks.clear(); discontinuity = true;
        }
        blocks.push_back(block); cv.notify_one();
    }
    void run() {
        try {
            uint64_t applied = 0, sequence = 0;
            double appliedRate = 0;
            dvbs::MixDc mixer;
            std::vector<dvbs::Halfband> decimators;
            std::vector<dvbs::SplitBuf> stages;
            std::vector<cf32> output;
            double outputRate = 0;
            for (;;) {
                Block block; ChannelConfig config; uint64_t wanted; bool gap; double correction;
                { std::unique_lock<std::mutex> lock(mutex);
                  cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return quit || revision != applied || !blocks.empty(); });
                  if (quit) break;
                  config = status.config; wanted = revision; gap = discontinuity; discontinuity = false; correction = capture.correctionHz;
                  if (!blocks.empty()) { block = blocks.front(); blocks.pop_front(); } }
                const double rate = inputRate.load();
                if (wanted != applied || rate != appliedRate || gap) {
                    // Configure/reset is owned solely by this channel's worker.
                    receiver.configure(0); // drain/stop the old decoder before clearing TS
                    { std::lock_guard<std::mutex> lock(mutex); demux.reset(); status.transport = {}; status.telemetry = {}; }
                    const double offset = config.satelliteMhz * 1e6 - capture.lnbHz + correction - capture.centerIfHz;
                    if (config.enabled && std::abs(offset) + config.symbolRate * 1.35 / 2 > rate * .49) {
                        // The radio's window moved (a slower radio): the mixer would wrap around and decode another station.
                        const double centreMhz = (capture.lnbHz + capture.centerIfHz - correction) / 1e6, halfMhz = rate * .49 / 1e6; char text[96];
                        std::snprintf(text, sizeof text, "Outside the radio window (%.1f to %.1f MHz)", centreMhz - halfMhz, centreMhz + halfMhz);
                        throw std::runtime_error(text);
                    }
                    mixer.configure(rate, offset);
                    int stagesCount = 0;
                    while (rate / (1u << (stagesCount + 1)) >= std::max(2e6, 3.0 * config.symbolRate) && stagesCount < 8) ++stagesCount;
                    outputRate = rate / (1u << stagesCount);
                    decimators.assign(stagesCount, dvbs::Halfband()); stages.assign(stagesCount + 1, {});
                    if (config.enabled) {
                        receiver.configure(outputRate); receiver.setSymbolRate(config.symbolRate);
                        receiver.setStandardHint(config.standard);
                    }
                    applied = wanted; appliedRate = rate; sequence = 0;
                    { std::lock_guard<std::mutex> lock(mutex); status.outputRate = outputRate; }
                }
                if (!config.enabled || !block) continue;
                for (auto& stage : stages) stage.clear();
                mixer.process(block->data(), block->size(), stages[0]);
                for (size_t i = 0; i < decimators.size(); ++i) decimators[i].process(stages[i], stages[i].size(), stages[i + 1]);
                auto& filtered = stages.back(); output.resize(filtered.size()); double energy = 0;
                for (size_t i = 0; i < filtered.size(); ++i) {
                    output[i] = {filtered.re[i], filtered.im[i]}; energy += std::norm(output[i]);
                }
                receiver.feed(output.data(), output.size());
                DvbsTelemetry telemetry; const bool fresh = receiver.telemetry(telemetry, sequence);
                if (fresh) sequence = telemetry.seq;
                { std::lock_guard<std::mutex> lock(mutex);
                  if (revision == applied) {
                      if (fresh) status.telemetry = std::move(telemetry);
                      status.levelDbfs = 10 * std::log10(std::max(1e-12, energy / std::max<size_t>(1, output.size())));
                  } }
            }
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> lock(mutex); status.error = e.what(); closeStreams();
        } catch (...) {
            std::lock_guard<std::mutex> lock(mutex); status.error = "Channel worker failed"; closeStreams();
        }
    }
};
}

struct MultiReceiver::Impl {
    MultiConfig config;
    std::atomic<double> rate;
    IqRing ring{1u << 20}; // 8 MiB, rather than one full wideband ring per decoder
    std::unique_ptr<IqSource> source;
    std::atomic<bool> active{false}, quit{false};
    std::thread captureThread;
    mutable std::mutex mutex;
    std::vector<std::unique_ptr<Channel>> pool;
    unsigned nextId = 1;
    SpectrumAnalyzer analyzer{4096};
    SpectrumFrame latest;
    Clock::time_point nextSpectrum = Clock::now();
    std::string error;
    SharedGainStatus gain;
    std::optional<std::pair<bool,double>> gainRequest;
    std::optional<double> centerRequest;   // a new IF centre for the running radio, applied by the capture thread
    TuneSettings radioTune;
    bool hackrf = false;
    WidebandGain gainController;
    uint64_t gainSequence = 0, overloadSeen = 0;
    explicit Impl(MultiConfig c) : config(c), rate(c.sampleRate) {}
    bool valid(ChannelConfig channel, std::string& why) const {
        const double offset = channel.satelliteMhz * 1e6 - config.lnbHz + config.correctionHz - config.centerIfHz;
        if (!std::isfinite(channel.satelliteMhz) || channel.satelliteMhz < 10490.5 || channel.satelliteMhz > 10499.5 ||
            !std::isfinite(channel.symbolRate) || channel.symbolRate < 25000 || channel.symbolRate > 4000000 ||
            channel.standard < 0 || channel.standard > 2) { why = "Enter a QO-100 frequency and symbol rate from 25 to 4000 kS/s."; return false; }
        if (std::abs(offset) + channel.symbolRate * 1.35 / 2 > rate.load() * .49) {
            why = "The entire channel must fit inside the shared IQ capture."; return false;
        }
        return true;
    }
};
// The HackRF has three stages; the single number is split by gainForTotal()
static std::string hackrfStages(int total) {
    const auto s = gainForTotal(total);
    return std::string("RF amp ") + (s.amp ? "on" : "off") + " · LNA " + std::to_string(s.lna) + " dB · VGA " + std::to_string(s.vga) + " dB";
}
MultiReceiver::MultiReceiver(MultiConfig config) : p_(std::make_unique<Impl>(config)) {
    if (config.maxChannels < 1 || config.maxChannels > 16 || !std::isfinite(config.sampleRate) || config.sampleRate < 2e6 || config.sampleRate > 20e6 ||
        !std::isfinite(config.centerIfHz) || !std::isfinite(config.lnbHz) || !std::isfinite(config.correctionHz)) throw std::invalid_argument("Invalid shared capture configuration");
}
MultiReceiver::~MultiReceiver() { stop(); }
unsigned MultiReceiver::create(ChannelConfig config) {
    std::lock_guard<std::mutex> lock(p_->mutex); std::string error;
    if (p_->pool.size() >= p_->config.maxChannels || !p_->valid(config, error)) return 0;
    const unsigned id = p_->nextId++;
    p_->pool.push_back(std::make_unique<Channel>(id, p_->config, p_->rate, config)); return id;
}
bool MultiReceiver::configure(unsigned id, ChannelConfig config, std::string& error) {
    std::lock_guard<std::mutex> lock(p_->mutex);
    if (!p_->valid(config, error)) return false;
    for (auto& channel : p_->pool) if (channel->id == id) { channel->set(config); return true; }
    error = "Unknown channel"; return false;
}
std::vector<ChannelStatus> MultiReceiver::channels() const {
    std::lock_guard<std::mutex> lock(p_->mutex); std::vector<ChannelStatus> result;
    for (auto& channel : p_->pool) {
        std::lock_guard<std::mutex> stateLock(channel->mutex); auto status = channel->status;
        if (channel->lastPacket != Clock::time_point{}) status.packetAgeSeconds = std::chrono::duration<double>(Clock::now()-channel->lastPacket).count();
        result.push_back(std::move(status));
    }
    return result;
}
std::shared_ptr<ChannelStream> MultiReceiver::subscribe(unsigned id) {
    std::lock_guard<std::mutex> lock(p_->mutex);
    for (auto& channel : p_->pool) if (channel->id == id) {
        std::lock_guard<std::mutex> stateLock(channel->mutex);
        channel->subscribers.erase(std::remove_if(channel->subscribers.begin(), channel->subscribers.end(),
            [](const auto& weak) { return weak.expired(); }), channel->subscribers.end());
        if (!channel->status.config.enabled || channel->subscribers.size() >= 4) return {};
        auto stream = std::make_shared<ChannelStream>(); channel->subscribers.push_back(stream); return stream;
    }
    return {};
}
void MultiReceiver::feed(const cf32* samples, size_t count) {
    if (!count) return;
    auto block = std::make_shared<std::vector<cf32>>(samples, samples + count);
    for (auto& sample : *block) if (!std::isfinite(sample.real()) || !std::isfinite(sample.imag())) sample = {};
    // Only display-rate snapshots are FFTed; the same unmodified IQ block feeds every channel.
    const auto now = Clock::now();
    if (now >= p_->nextSpectrum) {
        p_->analyzer.feed(block->data(), std::min<size_t>(count, 32768));
        SpectrumFrame frame;
        if (p_->analyzer.takeFrame(frame)) { std::lock_guard<std::mutex> lock(p_->mutex); p_->latest = std::move(frame); }
        p_->nextSpectrum = now + std::chrono::milliseconds(100);
    }
    std::lock_guard<std::mutex> lock(p_->mutex);
    for (auto& channel : p_->pool) channel->push(block);
}
bool MultiReceiver::start(const DeviceInfo& device, TuneSettings tune, std::string& error) {
    stop(); tune.centerHz = p_->config.centerIfHz; tune.sampleRate = p_->config.sampleRate;
    // A radio that cannot reach the full band (an RTL-SDR: 2.56 Msps at most, and above about 2.4 most dongles drop samples) is asked for what
    // it can do, instead of being refused: the capture is then narrower (see MultiReceiver::alignTargetsMhz)
    if (device.maxRateHz > 0 && device.maxRateHz < tune.sampleRate) tune.sampleRate = std::min(device.maxRateHz, device.board == "rtlsdr" ? 2.4e6 : device.maxRateHz);
    tune.bandwidthMhz = 9; tune.basebandFilterHz = p_->config.filterHz > 0 ? p_->config.filterHz : 8e6;
    tune.radio["linklimit"] = "1";   // a radio whose link cannot carry the rate runs at the highest it can (PlutoSDR); the capture is then narrower
    p_->ring.clear(); p_->ring.resetDropped(); p_->source = makeSource(device);
    if (!p_->source || !p_->source->start(tune, p_->ring, error)) { p_->source.reset(); return false; }
    p_->rate = p_->source->sampleRate();
    p_->radioTune = tune; p_->hackrf = device.kind == DeviceInfo::HackRF;
    // (the radio reports its stages after its first configuration)
    { std::lock_guard<std::mutex> lock(p_->mutex);
      p_->gain.minimum = device.isGeneric() ? device.gainMinDb : 0;
      p_->gain.maximum = device.isGeneric() && device.gainMaxDb > 0 ? device.gainMaxDb : p_->hackrf ? 100 : 100;
      p_->gain.gain = std::clamp(tune.gainDb,p_->gain.minimum,p_->gain.maximum);
      p_->gain.detail = p_->hackrf ? hackrfStages((int)p_->gain.gain) : p_->source->gainDescription();
      p_->gain.available = true; p_->gain.status = p_->gain.automatic ? "Auto: monitoring shared band" : "Manual"; }
    setAdcBits(device.board == "sdrplay" ? (p_->rate <= 8.064e6 ? 12 : p_->rate <= 9.216e6 ? 10 : 8) : (device.board == "airspy" || device.board == "pluto" || device.board == "lime") ? 12 : 8);
    p_->gainController.reset((int)p_->gain.minimum,(int)p_->gain.maximum);
    p_->gainSequence = 0; p_->overloadSeen = p_->source->overloadEvents();
    if (p_->rate < 2e6) { error = "The capture needs at least 2 Msps; this source is slower."; p_->source->stop(); p_->source.reset(); return false; }
    // Reinitialise only after the actual hardware rate is known.
    for (auto channel : channels()) { std::string ignored; configure(channel.id, channel.config, ignored); }
    p_->quit = false; p_->active = true;
    { std::lock_guard<std::mutex> lock(p_->mutex); p_->error.clear(); p_->latest = {}; }
    p_->captureThread = std::thread([this] {
        std::vector<cf32> samples(32768); auto last = Clock::now();
        try {
            while (!p_->quit) {
                std::optional<std::pair<bool,double>> requested;
                SharedGainStatus gain;
                { std::lock_guard<std::mutex> lock(p_->mutex); requested = p_->gainRequest; p_->gainRequest.reset(); gain = p_->gain; }
                if (requested) {
                    gain.automatic = requested->first;
                    if (!gain.automatic) gain.gain = std::clamp(requested->second,gain.minimum,gain.maximum);
                    p_->gainController.reset((int)gain.minimum,(int)gain.maximum);
                }
                std::optional<double> center;
                { std::lock_guard<std::mutex> lock(p_->mutex); center = p_->centerRequest; p_->centerRequest.reset(); }
                if (center) {
                    auto update = p_->radioTune; update.centerHz = *center; std::string note;
                    if (p_->source->retune(update,note)) p_->radioTune = update;
                    else { std::lock_guard<std::mutex> lock(p_->mutex); p_->error = "Could not move the capture: " + note; }
                }
                SpectrumFrame frame;
                const auto events = p_->source->overloadEvents();
                const bool overload = p_->source->overloaded() || events > p_->overloadSeen; p_->overloadSeen = events; gain.overloads = events;
                const bool fresh = spectrum(frame,p_->gainSequence);
                if (fresh) p_->gainSequence = frame.seq;
                double desired = gain.gain;
                const double now = std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
                const bool changed = gain.automatic && (fresh || overload) && p_->gainController.update(now,frame.stats,overload,desired);
                if (changed || (requested && !gain.automatic)) {
                    auto update = p_->radioTune; update.gainDb = desired;
                    if (p_->hackrf) { const auto setting = gainForTotal((int)desired); update.lnaDb = setting.lna; update.vgaDb = setting.vga; update.ampOn = setting.amp; }
                    std::string note;
                    if (p_->source->retune(update,note)) {
                        p_->radioTune = update; gain.gain = desired;
                        gain.detail = p_->hackrf ? hackrfStages((int)desired) : p_->source->gainDescription();
                        gain.status = gain.automatic ? (overload ? "Auto: reducing RF overload" : "Auto: adjusting shared gain") : "Manual";
                    } else gain.status = "Gain update failed: " + note;
                } else if (gain.automatic && overload && gain.gain <= gain.minimum) gain.status = "Auto: RF overload at minimum gain";
                else if (!gain.automatic && overload) gain.status = "Manual: RF overload - reduce gain";
                else if (fresh || requested) gain.status = gain.automatic ? "Auto: monitoring shared band" : "Manual";
                { std::lock_guard<std::mutex> lock(p_->mutex); p_->gain = gain; }
                p_->ring.dropBacklog();
                const auto count = p_->ring.read(samples.data(), samples.size());
                if (count) {
                    feed(samples.data(), count);
                    if (Clock::now() - last > std::chrono::seconds(2)) { std::lock_guard<std::mutex> lock(p_->mutex); p_->error.clear(); }
                    last = Clock::now();
                }
                else { if (Clock::now() - last > std::chrono::seconds(2)) { std::lock_guard<std::mutex> lock(p_->mutex); p_->error = "Shared radio is not delivering IQ samples"; }
                       std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
            }
        } catch (const std::exception& e) { std::lock_guard<std::mutex> lock(p_->mutex); p_->error = e.what(); p_->active = false; }
    });
    return true;
}
bool MultiReceiver::startDemo(std::string& error, int secondStandard) {
    stop(); p_->rate = p_->config.sampleRate;
    try {
        // Two independent RF carriers are summed BEFORE the shared capture fan-out.
        // This exercises downconversion and adjacent-channel rejection, not two fake TS inputs.
        dvbs::DvbsSignalConfig a, b; a.sampleRate = b.sampleRate = p_->rate;
        a.tx.symbolRate = b.tx.symbolRate = 1500000;
        if (secondStandard != 1 && secondStandard != 2) throw std::invalid_argument("Demo standard must be DVB-S or DVB-S2");
        a.tx.standard = 2; b.tx.standard = secondStandard; b.tx.rate = secondStandard == 1 ? 2 : 5;
        a.cfoHz = 10491.5e6 - p_->config.lnbHz + p_->config.correctionHz - p_->config.centerIfHz;
        b.cfoHz = 10496.5e6 - p_->config.lnbHz + p_->config.correctionHz - p_->config.centerIfHz;
        b.seed = 7;
        auto first = std::make_shared<dvbs::DvbsSignal>(a), second = std::make_shared<dvbs::DvbsSignal>(b);
        p_->quit = false; p_->active = true;
        { std::lock_guard<std::mutex> lock(p_->mutex); p_->error.clear(); }
        p_->captureThread = std::thread([this, first, second] {
            std::vector<cf32> a(32768), b(a.size()); auto deadline = Clock::now();
            try {
                while (!p_->quit) {
                    first->generate(a.data(), a.size()); second->generate(b.data(), b.size());
                    for (size_t i = 0; i < a.size(); ++i) a[i] += b[i];
                    feed(a.data(), a.size());
                    deadline += std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(a.size() / p_->rate.load()));
                    if (Clock::now() > deadline + std::chrono::milliseconds(100)) deadline = Clock::now();
                    std::this_thread::sleep_until(deadline);
                }
            } catch (const std::exception& e) { std::lock_guard<std::mutex> lock(p_->mutex); p_->error = e.what(); p_->active = false; }
        }); return true;
    } catch (const std::exception& e) { error = e.what(); return false; }
}
void MultiReceiver::stop() {
    p_->quit = true;
    if (p_->captureThread.joinable()) p_->captureThread.join();
    if (p_->source) { p_->source->stop(); p_->source.reset(); }
    p_->active = false;
    { std::lock_guard<std::mutex> lock(p_->mutex); p_->gain.available = false; p_->gainRequest.reset(); }
    for (auto channel : channels()) { channel.config.enabled = false; std::string ignored; configure(channel.id, channel.config, ignored); }
}
void MultiReceiver::setGain(bool automatic, double gain) {
    std::lock_guard<std::mutex> lock(p_->mutex);
    if (p_->gain.available) p_->gainRequest = std::make_pair(automatic,gain);
    else { p_->gain.automatic = automatic; p_->gain.gain = gain; p_->gain.status = "Gain control inactive (no live radio)"; }
}
SharedGainStatus MultiReceiver::gainStatus() const { std::lock_guard<std::mutex> lock(p_->mutex); return p_->gain; }
bool MultiReceiver::running() const { return p_->active; }
double MultiReceiver::sampleRate() const { return p_->rate; }
uint64_t MultiReceiver::sourceDrops() const { return p_->ring.dropped(); }
std::string MultiReceiver::sourceError() const { std::lock_guard<std::mutex> lock(p_->mutex); return p_->error; }
bool MultiReceiver::spectrum(SpectrumFrame& frame, uint64_t sequence) const {
    std::lock_guard<std::mutex> lock(p_->mutex);
    if (p_->latest.seq <= sequence || p_->latest.dbfs.empty()) return false;
    frame = p_->latest; return true;
}
const MultiConfig& MultiReceiver::config() const { return p_->config; }
std::array<double,3> MultiReceiver::alignTargetsMhz() const {
    const double rate = p_->rate.load() > 0 ? p_->rate.load() : p_->config.sampleRate, half = rate / 2e6;
    if (rate >= 9.5e6) return {10490.5 + half, 10495.0, 10499.5 - half};
    const double usable = rate * 0.49 / 1e6;            // what Channel::valid() accepts: |offset| + 0.675 * symbol rate <= 0.49 * rate
    // left: room for a 1.5 MS/s channel at 10491.5. right: the window ends 0.5 MHz beyond the band's end, so that the rounded edge of the radio's
    // filter falls outside the top channels (and a 333 kS/s channel at 10499.4 still fits)
    const double left = 10491.5 + (usable - 1.5 * 1.35 / 2) - 0.02;
    const double right = std::max(10500.0 - half, 10499.4 - (usable - 0.333 * 1.35 / 2) + 0.02);
    return {std::min(left, 10495.0), 10495.0, std::max(right, 10495.0)};
}
bool MultiReceiver::setCenterIf(double hz) {
    if (!std::isfinite(hz) || hz < 100e6 || hz > 3e9) return false;
    std::lock_guard<std::mutex> lock(p_->mutex);
    p_->config.centerIfHz = hz;
    for (auto& channel : p_->pool) channel->setCenter(hz);
    if (p_->active) p_->centerRequest = hz;
    return true;
}
void MultiReceiver::setCorrection(double hz) {
    if (!std::isfinite(hz) || std::fabs(hz) > 1e6) return;
    std::lock_guard<std::mutex> lock(p_->mutex);
    p_->config.correctionHz = hz;
    for (auto& channel : p_->pool) channel->setCorrection(hz);
}
} // namespace dect2
