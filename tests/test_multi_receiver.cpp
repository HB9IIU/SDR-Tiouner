#include "dect2/multi_receiver.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
using namespace dect2;
#define CHECK(condition) do { if (!(condition)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#condition); return 1; } } while (0)
int main() {
    MultiConfig config; config.maxChannels = 3;
    MultiReceiver receiver(config);
    ChannelConfig first{10491.5,500000,0,true}, second{10496.5,500000,0,true};
    auto a = receiver.create(first), b = receiver.create(second);
    CHECK(a && b && a != b);
    CHECK(receiver.create({10494.5,250000,0,false}));
    CHECK(!receiver.create(first)); // configured pool cap, rather than hardcoded two
    auto original = receiver.channels(); std::string error;
    auto invalid = first; invalid.satelliteMhz = NAN;
    CHECK(!receiver.configure(a,invalid,error));
    invalid = first; invalid.satelliteMhz = 10490.5; invalid.symbolRate = 4000000;
    CHECK(!receiver.configure(a,invalid,error)); // occupied channel crosses the capture edge
    CHECK(!receiver.configure(999,first,error));
    auto stream = receiver.subscribe(a); CHECK(stream);
    first.enabled = false; CHECK(receiver.configure(a,first,error));
    { std::lock_guard<std::mutex> lock(stream->mutex); CHECK(stream->closed); }
    auto after = receiver.channels();
    CHECK(after[0].revision > original[0].revision);
    CHECK(after[1].revision == original[1].revision && after[1].config.enabled);
    first.enabled = true; CHECK(receiver.configure(a,first,error));
    // Two unequal tones 100 kHz above each channel's centre. Channel filtering
    // must reject the other RF signal; frequencies and levels are independently known.
    std::vector<cf32> block(32768);
    const double tau = 6.2831853071795864769;
    size_t sample = 0;
    for (int batch = 0; batch < 24; ++batch) {
        for (auto& value : block) {
            const double time = sample++ / 10e6;
            value = cf32(.2f*(float)std::cos(tau*-3.4e6*time),.2f*(float)std::sin(tau*-3.4e6*time))
                  + cf32(.01f*(float)std::cos(tau*1.6e6*time),.01f*(float)std::sin(tau*1.6e6*time));
        }
        receiver.feed(block.data(),block.size());
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    auto status = receiver.channels();
    CHECK(status[0].outputRate == 2.5e6 && status[1].outputRate == 2.5e6);
    CHECK(std::abs(status[0].levelDbfs + 13.9794) < 1.5);
    CHECK(std::abs(status[1].levelDbfs + 40.0) < 1.5);
    CHECK(status[0].droppedSamples == 0 && status[1].droppedSamples == 0);
    SpectrumFrame spectrum; CHECK(receiver.spectrum(spectrum)); CHECK(spectrum.dbfs.size() == 4096);
    auto peakNear = [&](double offset) {
        int bin = (int)std::round((offset/10e6+.5)*spectrum.dbfs.size());
        float peak = -140;
        for (int i = bin-2; i <= bin+2; ++i) peak = std::max(peak,spectrum.dbfs[i]);
        return peak;
    };
    CHECK(peakNear(-3.4e6) > -20 && peakNear(1.6e6) > -46);
    puts("PASS: dynamic pool cap, isolated retune/stop, validation, shared spectrum and DDC rejection");
}
