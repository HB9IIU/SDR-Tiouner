// Radios on the network that the user adds by address: see netradio.h
#include "tcp_client.h"
#include "native_common.h"
#include "dect2/netradio.h"
#include <algorithm>
#include <chrono>
#include <map>
#include <mutex>
#ifdef DECT2_HAVE_SOAPY
#include <SoapySDR/Device.hpp>
#endif

namespace dect2 {

#ifdef DECT2_HAVE_SOAPY
DeviceInfo soapyDeviceFor(const std::string& args, const std::string& board, const std::string& name);   // source_soapy.cpp
#endif

namespace {
std::mutex gNetMu;
std::vector<NetRadio> gNet;

const char* typeKey(NetRadio::Type t) {
    return t == NetRadio::Pluto ? "pluto" : t == NetRadio::SoapyRemote ? "soapyremote" : t == NetRadio::SpyServer ? "spyserver" : t == NetRadio::HermesLite2 ? "hl2" : "rtltcp";
}

// the host as typed: no spaces, no "ip:" (a Pluto's libiio URI), no "tcp://" (a SoapyRemote URL)
std::string cleanHost(std::string h) {
    std::string r;
    for (char c : h) if (c != ' ' && c != '\t' && c != ',' && c != ';') r += c;
    for (const char* pre : {"ip:", "tcp://"}) if (r.rfind(pre, 0) == 0) r.erase(0, strlen(pre));
    return r;
}
std::string hostPort(const std::string& host, int port) {
    return (host.find(':') != std::string::npos ? "[" + host + "]" : host) + ":" + std::to_string(port);
}
} // namespace

const char* netRadioTypeName(NetRadio::Type t) {
    return t == NetRadio::Pluto ? "PlutoSDR" : t == NetRadio::SoapyRemote ? "SoapyRemote" : t == NetRadio::SpyServer ? "SpyServer"
         : t == NetRadio::HermesLite2 ? "Hermes Lite 2" : "rtl_tcp";
}
int netRadioDefaultPort(NetRadio::Type t) {
    return t == NetRadio::Pluto ? 0 : t == NetRadio::SoapyRemote ? 55132 : t == NetRadio::SpyServer ? 5555 : t == NetRadio::HermesLite2 ? 1024 : 1234;
}
const char* netRadioDefaultHost(NetRadio::Type t) { return t == NetRadio::Pluto ? "192.168.2.1" : ""; }
int netRadioPort(const NetRadio& r) {
    if (r.port > 0) return r.port;
    return r.type == NetRadio::Pluto ? 30431 : netRadioDefaultPort(r.type);
}

std::string netRadiosToString(const std::vector<NetRadio>& v) {
    std::string s;
    for (const auto& r : v) {
        const std::string h = cleanHost(r.host);
        if (h.empty()) continue;
        std::string rem;
        if (r.type == NetRadio::SoapyRemote) for (char c : r.remote) if (c != ';') rem += c;
        s += (s.empty() ? "" : ";") + std::string(typeKey(r.type)) + "," + h + "," + std::to_string(r.port > 0 ? r.port : 0) + (rem.empty() ? "" : "," + rem);
    }
    return s;
}

std::vector<NetRadio> netRadiosFromString(const std::string& s) {
    std::vector<NetRadio> v;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(';', pos);
        if (e == std::string::npos) e = s.size();
        const std::string item = s.substr(pos, e - pos);
        pos = e + 1;
        const size_t c1 = item.find(','), c2 = c1 == std::string::npos ? c1 : item.find(',', c1 + 1);
        const size_t c3 = c2 == std::string::npos ? c2 : item.find(',', c2 + 1);   // a SoapyRemote's "remote driver" follows (it may hold commas)
        if (c2 == std::string::npos) continue;
        const std::string t = item.substr(0, c1);
        NetRadio r;
        if (t == "pluto") r.type = NetRadio::Pluto;
        else if (t == "rtltcp") r.type = NetRadio::RtlTcp;
        else if (t == "soapyremote") r.type = NetRadio::SoapyRemote;
        else if (t == "spyserver") r.type = NetRadio::SpyServer;
        else if (t == "hl2") r.type = NetRadio::HermesLite2;
        else continue;
        r.host = cleanHost(item.substr(c1 + 1, c2 - c1 - 1));
        r.port = atoi(item.c_str() + c2 + 1);
        if (c3 != std::string::npos && r.type == NetRadio::SoapyRemote) r.remote = item.substr(c3 + 1);
        if (r.port < 0 || r.port > 65535) r.port = 0;
        if (!r.host.empty() && std::find(v.begin(), v.end(), r) == v.end()) v.push_back(r);
    }
    return v;
}

void setNetworkRadios(const std::vector<NetRadio>& v) { std::lock_guard<std::mutex> lk(gNetMu); gNet = v; }
std::vector<NetRadio> networkRadios() { std::lock_guard<std::mutex> lk(gNetMu); return gNet; }

namespace {
// A Pluto's libiio URI: "ip:host", with ":port" only when one was given
std::string plutoUri(const NetRadio& r) {
    const std::string h = cleanHost(r.host);
    return "ip:" + (r.port > 0 ? hostPort(h, r.port) : h);
}
std::string trimWs(const std::string& v) {
    size_t a = 0, b = v.size();
    while (a < b && (v[a] == ' ' || v[a] == '\t')) a++;
    while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t')) b--;
    return v.substr(a, b - a);
}
std::string lower(std::string v) { for (char& c : v) c = (char)tolower((unsigned char)c); return v; }
std::string remoteBase(const NetRadio& r) { return "driver=remote,remote=tcp://" + hostPort(cleanHost(r.host), netRadioPort(r)); }

std::mutex gEnumMu;
SoapyEnumerateFn gEnum = nullptr;
struct Offered { std::chrono::steady_clock::time_point at; std::vector<RemoteSoapyDevice> all; };
std::map<std::string, Offered> gOffered;   // by the server's "host:port"

std::vector<std::map<std::string, std::string>> defaultEnumerate(const std::string& args) {
    std::vector<std::map<std::string, std::string>> out;
#ifdef DECT2_HAVE_SOAPY
    try { for (const auto& kw : SoapySDR::Device::enumerate(args)) { std::map<std::string, std::string> m; for (const auto& e : kw) m[e.first] = e.second; out.push_back(std::move(m)); } } catch (...) {}
#else
    (void)args;
#endif
    return out;
}
std::string soapyArgs(const NetRadio& r);
} // namespace

std::string soapyRemoteArgs(const NetRadio& r) {
    std::string a = remoteBase(r);
    size_t pos = 0;
    while (pos <= r.remote.size()) {
        size_t e = r.remote.find(',', pos);
        if (e == std::string::npos) e = r.remote.size();
        const std::string kv = trimWs(r.remote.substr(pos, e - pos));
        pos = e + 1;
        if (kv.empty()) continue;
        a += kv.find('=') == std::string::npos ? ",remote:driver=" + kv : ",remote:" + kv;
    }
    return a;
}
namespace { std::string soapyArgs(const NetRadio& r) { return soapyRemoteArgs(r); } }

std::vector<RemoteSoapyDevice> soapyRemoteOffers(const std::vector<std::map<std::string, std::string>>& kwargs, bool includeAudio) {
    std::vector<RemoteSoapyDevice> out;
    auto get = [](const std::map<std::string, std::string>& m, const char* k) {
        const auto it = m.find(std::string("remote:") + k);
        if (it != m.end()) return it->second;
        const auto i2 = m.find(k);
        return i2 == m.end() ? std::string() : i2->second;
    };
    for (const auto& m : kwargs) {
        RemoteSoapyDevice d;
        d.driver = get(m, "driver"); d.serial = get(m, "serial"); d.label = get(m, "label");
        if (d.driver.empty() || d.driver == "remote") continue;
        if (!includeAudio && lower(d.driver) == "audio") continue;
        out.push_back(d);
    }
    return out;
}

void setSoapyEnumerator(SoapyEnumerateFn fn) { std::lock_guard<std::mutex> lk(gEnumMu); gEnum = fn; gOffered.clear(); }

std::vector<RemoteSoapyDevice> soapyRemoteList(const NetRadio& r, bool includeAudio) {
    const std::string key = hostPort(cleanHost(r.host), netRadioPort(r));
    SoapyEnumerateFn fn;
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        fn = gEnum;
        const auto it = gOffered.find(key);
        if (it != gOffered.end() && std::chrono::steady_clock::now() - it->second.at < std::chrono::seconds(60)) {
            std::vector<RemoteSoapyDevice> v;
            for (const auto& d : it->second.all) if (includeAudio || lower(d.driver) != "audio") v.push_back(d);
            return v;
        }
    }
    const auto kw = (fn ? fn : defaultEnumerate)(remoteBase(r));
    Offered o;
    o.at = std::chrono::steady_clock::now();
    o.all = soapyRemoteOffers(kw, true);
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        gOffered[key] = o;
    }
    return soapyRemoteOffers(kw, includeAudio);
}

std::string soapyRemoteFailureHelp(const std::string& args) {
    const size_t p = args.find("remote=tcp://");
    if (args.find("driver=remote") == std::string::npos || p == std::string::npos) return "";
    const size_t e = args.find(',', p);
    const std::string key = args.substr(p + 13, e == std::string::npos ? std::string::npos : e - p - 13);
    std::vector<RemoteSoapyDevice> all;
    {
        std::lock_guard<std::mutex> lk(gEnumMu);
        const auto it = gOffered.find(key);
        if (it != gOffered.end()) all = it->second.all;
    }
    std::string s = "The SoapyRemote server " + key + " did not give OnAir a radio. ";
    if (all.empty()) return s + "It lists no radios: check that SoapySDRServer runs there and that the radio's SoapySDR module (for example mirisdr or sdrplay) is installed on that computer, then pick the radio, or type its driver in the Remote driver box.";
    std::string pick;
    for (const auto& d : all) if (lower(d.driver) != "audio") { pick = d.driver; break; }
    s += "It offers:";
    for (const auto& d : all) s += " " + d.driver + (d.label.empty() && d.serial.empty() ? "" : " (" + (d.label.empty() ? d.serial : d.label) + ")") + ";";
    s += " pick the radio in the list, or type its driver ";
    if (!pick.empty()) s += "(for example " + pick + ") ";
    return s + "in the Remote driver box. A sound card (audio) is not a radio.";
}

std::string netRadioDeviceName(const NetRadio& r) {
    switch (r.type) {
    case NetRadio::Pluto: return native::plutoDevice(plutoUri(r)).name;
    case NetRadio::SoapyRemote: return "SoapyRemote " + hostPort(cleanHost(r.host), netRadioPort(r)) + (r.remote.empty() ? "" : " " + r.remote) + " (network)";
    case NetRadio::SpyServer: return native::spyServerDevice(cleanHost(r.host), netRadioPort(r)).name;
    case NetRadio::HermesLite2: return native::hermesLite2Device(cleanHost(r.host), netRadioPort(r)).name;
    default: return native::rtlTcpDevice(cleanHost(r.host), netRadioPort(r)).name;
    }
}

std::vector<DeviceInfo> netRadioDevices(const std::vector<NetRadio>& v) {
    std::vector<DeviceInfo> out;
    for (const auto& r : v) {
        if (cleanHost(r.host).empty()) continue;
        switch (r.type) {
        case NetRadio::Pluto: out.push_back(native::plutoDevice(plutoUri(r))); break;
        case NetRadio::RtlTcp: out.push_back(native::rtlTcpDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::SpyServer: out.push_back(native::spyServerDevice(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::HermesLite2: out.push_back(native::hermesLite2Device(cleanHost(r.host), netRadioPort(r))); break;
        case NetRadio::SoapyRemote:
#ifdef DECT2_HAVE_SOAPY
        {
            const std::string hp = hostPort(cleanHost(r.host), netRadioPort(r));
            const auto offers = r.remote.empty() ? soapyRemoteList(r) : std::vector<RemoteSoapyDevice>();
            if (offers.empty()) out.push_back(soapyDeviceFor(soapyArgs(r), "remote", netRadioDeviceName(r)));
            for (const auto& o : offers) {
                NetRadio one = r;
                one.remote = "driver=" + o.driver + (o.serial.empty() ? "" : ",serial=" + o.serial);
                const std::string det = o.label.empty() ? o.serial : o.label;
                out.push_back(soapyDeviceFor(soapyArgs(one), "remote", "SoapyRemote " + hp + ": " + o.driver + (det.empty() ? "" : " (" + det + ")") + " (network)"));
            }
        }
#endif
            break;
        }
    }
    return out;
}

bool probeNetRadio(const NetRadio& r, int timeoutMs, std::string& err) {
    const std::string host = cleanHost(r.host);
    if (host.empty()) { err = "type the radio's address"; return false; }
#ifndef DECT2_HAVE_SOAPY
    if (r.type == NetRadio::SoapyRemote) { err = "this build of OnAir has no SoapySDR, which SoapyRemote needs"; return false; }
#endif
    if (r.type == NetRadio::SpyServer) return native::spyServerProbe(host, netRadioPort(r), timeoutMs, err);   // HELLO, then its DEVICE_INFO
    if (r.type == NetRadio::HermesLite2) return native::hermesLite2Probe(host, netRadioPort(r), timeoutMs, err);   // UDP: a discovery to it
    const sock_t s = tcpConnect(host, netRadioPort(r), timeoutMs, err);
    if (s == kBadSock) return false;
    bool ok = true;
    if (r.type == NetRadio::RtlTcp) {   // an rtl_tcp server greets every client with "RTL0" (or its own magic) and its tuner
        sockRecvTimeout(s, timeoutMs);
        unsigned char hdr[12];
        if (!sockRecvAll(s, hdr, sizeof hdr)) { err = "the server at " + hostPort(host, netRadioPort(r)) + " sent no rtl_tcp greeting (another client may be connected)"; ok = false; }
        else {
            std::string magic;
            bool other = false;
            if (!native::rtlTcpGreeting(hdr, magic, other)) { err = hostPort(host, netRadioPort(r)) + " is not an rtl_tcp server"; ok = false; }
            else native::rtlTcpNoteServer(native::rtlTcpDevice(host, netRadioPort(r)).nativeArgs, other);
        }
    }
    sockShutdown(s);
    sockClose(s);
#ifdef DECT2_HAVE_SOAPY
    if (ok && r.type == NetRadio::SoapyRemote && r.remote.empty() && soapyRemoteList(r).empty()) {   // reachable, but it offers no radio (a sound card is not one)
        err = soapyRemoteFailureHelp(soapyArgs(r));
        ok = false;
    }
#endif
    return ok;
}

} // namespace dect2
