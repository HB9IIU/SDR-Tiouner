#include "dect2/wideband_gain.h"
#include <cstdio>
using namespace dect2;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c); return 1; } } while (0)
int main() {
    setAdcBits(8); WidebandGain controller; controller.reset(0,85);
    SignalStats stats; stats.rmsDbfs=-22; stats.peak=.4;
    double gain=76;
    CHECK(!controller.update(0,stats,false,gain));
    CHECK(controller.update(.1,stats,true,gain) && gain==70); // RF overload despite no ADC clipping
    CHECK(!controller.update(.2,stats,true,gain));
    CHECK(controller.update(.5,stats,true,gain) && gain==64); // sustained hardware overload
    stats.rmsDbfs=-45; stats.peak=.05;
    CHECK(!controller.update(1,stats,false,gain)); // no rapid recovery into overload
    CHECK(controller.update(5,stats,false,gain) && gain==66); // slow 2 dB rise after headroom hold
    CHECK(!controller.update(5.1,stats,false,gain));
    stats.rmsDbfs=-8; stats.peak=1;
    CHECK(controller.update(5.2,stats,false,gain) && gain==60); // rapid response to sample clipping
    controller.reset(10,85); gain=10;
    CHECK(!controller.update(10,stats,true,gain) && gain==10); // actual device gain floor
    puts("PASS: hardware overload, clipping, bounded slow recovery and device limits");
}
