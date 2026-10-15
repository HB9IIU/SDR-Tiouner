#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Average independent carrier reports only when they agree within 5 kHz.
// A bad report breaks the streak; it never changes the existing correction.
struct BeaconMeasurements {
    std::vector<double> hz;
    void clear() { hz.clear(); }
    bool add(double correction, double& result) {
        if (!std::isfinite(correction) || std::fabs(correction) > 1e6) {
            clear(); return false;
        }
        if (!hz.empty() && std::fabs(correction - hz.back()) > 5000) clear();
        hz.push_back(correction);
        if (hz.size() < 8) return false;
        const auto range = std::minmax_element(hz.begin(), hz.end());
        if (*range.second - *range.first > 5000) { clear(); return false; }
        result = 0;
        for (double v : hz) result += v;
        result /= hz.size();
        return true;
    }
};
