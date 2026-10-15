#include "dect2/device_claims.h"
#include <map>
#include <mutex>

namespace dect2 {
namespace {
std::mutex gMu;
std::map<std::string, int> gHeld;
}

std::string deviceClaimKey(const DeviceInfo& d) {
    if (!d.isRadio()) return "";
    std::string id = !d.serial.empty() ? d.serial : !d.nativeArgs.empty() ? d.nativeArgs : !d.soapyArgs.empty() ? d.soapyArgs : d.name;
    return std::to_string((int)d.kind) + "|" + d.board + "|" + id;
}

bool claimDevice(const DeviceInfo& d, int owner, int* holder) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return true;
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gHeld.find(k);
    if (it != gHeld.end() && it->second != owner) { if (holder) *holder = it->second; return false; }
    gHeld[k] = owner;
    return true;
}

void releaseDevice(int owner) {
    std::lock_guard<std::mutex> lk(gMu);
    for (auto it = gHeld.begin(); it != gHeld.end();) it = it->second == owner ? gHeld.erase(it) : std::next(it);
}

int deviceHolder(const DeviceInfo& d) {
    const std::string k = deviceClaimKey(d);
    if (k.empty()) return 0;
    std::lock_guard<std::mutex> lk(gMu);
    auto it = gHeld.find(k);
    return it == gHeld.end() ? 0 : it->second;
}

}
