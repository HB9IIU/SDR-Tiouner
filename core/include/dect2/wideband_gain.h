#pragma once
#include "gain.h"
#include <algorithm>
#include <cmath>
namespace dect2 {
// Reuses the original ADC controller, adding the SDR's RF-overload events.
class WidebandGain {
public:
    void reset(int minimum, int maximum) {
        minimum_ = minimum; maximum_ = std::max(minimum, maximum);
        AutoGain::Config config; config.targetDbfs = -22; config.lowDbfs = -28;
        config.highDbfs = -16; config.maxStepDb = 2; config.settleSec = 1.2;
        config.genericMaxDb = std::max(1,maximum_);
        controller_ = AutoGain(config); inhibitRiseUntil_ = lastCut_ = -1e9;
    }
    bool update(double now, const SignalStats& stats, bool overload, double& gain) {
        if (overload || stats.peak >= .99 || stats.clipFraction > .002) inhibitRiseUntil_ = now + 4;
        if ((overload || stats.peak >= .99 || stats.clipFraction > .002) && now-lastCut_ >= .3) {
            const double next = std::max<double>(minimum_,gain-6);
            lastCut_ = now; controller_.reset();
            if (next != gain) { gain = next; return true; }
            return false;
        }
        GainSetting setting = genericGain((int)std::lround(gain),std::max(1,maximum_));
        if (!controller_.update(now,stats,setting)) return false;
        const double next = std::clamp(setting.total(),minimum_,maximum_);
        if (next > gain && now < inhibitRiseUntil_) return false;
        if (next == gain) return false;
        gain = next; return true;
    }
private:
    AutoGain controller_;
    int minimum_ = 0, maximum_ = 100;
    double inhibitRiseUntil_ = -1e9, lastCut_ = -1e9;
};
}
