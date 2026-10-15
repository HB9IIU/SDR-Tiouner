// Offset tuning (offset_tune.h): the plan puts the channel clear of DC with the rate it needs, refuses radios that are too slow, and the
// mixer brings a signal at +offset back to DC with no phase jump between blocks and no drift in level over a long run.
#include "dect2/offset_tune.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>
using namespace dect2;
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } } while (0)

int main() {
    {   // QO-100 DATV, 1 Msym/s (about 1.35 MHz wide) on a HackRF at the mode's 2 Msps: the plan raises the rate to clear DC
        const OffsetPlan p = planOffset(1.5e6, 2e6, 20e6);
        printf("DATV 1 Msym/s: offset %.0f Hz, rate %.3f Msps\n", p.offsetHz, p.rateHz / 1e6);
        CHECK(p.ok && p.offsetHz - 1.5e6 / 2 >= 25e3, "DATV: the channel's lower edge is not clear of DC");
        CHECK(p.rateHz / 2 >= p.offsetHz + 1.5e6 / 2, "DATV: the channel does not fit below the upper band edge");
        CHECK(p.rateHz <= 4e6, "DATV: more rate than needed (%.2f Msps)", p.rateHz / 1e6);
    }
    {   // a narrow carrier: the mode's own rate already holds the offset channel
        const OffsetPlan p = planOffset(200e3, 2.4e6, 3.2e6);
        CHECK(p.ok && p.rateHz == 2.4e6, "narrow carrier: the rate changed to %.3f Msps", p.rateHz / 1e6);
    }
    {   // an 8 MHz wide DVB-S carrier: a HackRF can, an SDRplay (10 Msps) cannot
        const OffsetPlan h = planOffset(8e6, 10e6, 20e6), s = planOffset(8e6, 10e6, 10e6);
        printf("8 MHz carrier: HackRF %d (%.2f Msps), SDRplay %d (%s)\n", h.ok, h.rateHz / 1e6, s.ok, s.why.c_str());
        CHECK(h.ok && h.rateHz <= 20e6, "8 MHz on a HackRF refused");
        CHECK(!s.ok && !s.why.empty(), "8 MHz on a 10 Msps radio accepted");
    }
    {   // the mixer: a tone at +offset comes out as a constant at DC, block after block, for 10^8 samples
        const double fs = 3.5e6, off = 845e3;
        OffsetMixer m; m.set(off, fs);
        std::vector<cf32> x(65536);
        double t = 0, maxPhaseErr = 0, minMag = 1e9, maxMag = 0;
        for (int blk = 0; blk < 1600; blk++) {
            for (size_t i = 0; i < x.size(); i++, t += 1) x[i] = std::polar(1.0f, (float)std::fmod(2 * M_PI * off * t / fs, 2 * M_PI));
            m.mix(x.data(), x.size());
            for (size_t i = 0; i < x.size(); i += 997) {
                maxPhaseErr = std::max(maxPhaseErr, (double)std::fabs(std::arg(x[i])));
                minMag = std::min(minMag, (double)std::abs(x[i])); maxMag = std::max(maxMag, (double)std::abs(x[i]));
            }
        }
        printf("mixer: max phase error %.4f rad, level %.5f..%.5f over %.0f samples\n", maxPhaseErr, minMag, maxMag, t);
        CHECK(maxPhaseErr < 0.01, "the tone is not at DC (phase error %.4f rad)", maxPhaseErr);
        CHECK(minMag > 0.999 && maxMag < 1.001, "the level drifted (%.5f..%.5f)", minMag, maxMag);
    }
    printf(fails ? "offset tune: FAILED\n" : "offset tune: ok\n");
    return fails ? 1 : 0;
}
