#include "../app/beacon_calibration.h"
#include <cstdio>
#include <cstdlib>
#include <limits>

int main() {
    BeaconMeasurements m;
    double result = 42;
    auto require = [](bool ok) { if (!ok) { std::fputs("Beacon calibration test failed\n", stderr); std::exit(1); } };
    for (int i = 0; i < 7; ++i) require(!m.add(120000 + i * 100, result));
    require(result == 42); // No premature change to the saved correction.
    require(m.add(120700, result) && std::fabs(result - 120350) < 0.01);
    m.clear();
    for (int i = 0; i < 8; ++i) m.add(-85000, result);
    require(result == -85000); // Both signs must be preserved.
    require(!m.add(std::numeric_limits<double>::quiet_NaN(), result) && m.hz.empty());
    require(!m.add(1100000, result) && m.hz.empty());
    for (int i = 0; i < 7; ++i) m.add(120000, result);
    require(!m.add(180000, result) && m.hz.size() == 1); // A jump starts a new streak.
    m.clear();
    for (int i = 0; i < 8; ++i) require(!m.add(i * 1000, result)); // Slow drift also fails stability.
    std::puts("Beacon calibration tests passed");
}
