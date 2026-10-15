#ifdef _WIN32
#define FD_SETSIZE 512   // one select() for a whole /24 network scan
#endif
// Native driver for the PlutoSDR: the radio's own library is loaded at run time when it is installed. See native_common.h.
#include "dect2/rate_choice.h"
#include "native_common.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#include <cerrno>
#include <map>

namespace dect2 {
namespace native {


struct IioApi {
    struct iio_context; struct iio_device; struct iio_channel; struct iio_buffer; struct iio_scan_context; struct iio_context_info;
    iio_scan_context* (DECT2_CALL* create_scan_context)(const char*, unsigned) = nullptr;
    void (DECT2_CALL* scan_context_destroy)(iio_scan_context*) = nullptr;
    long long (DECT2_CALL* scan_context_get_info_list)(iio_scan_context*, iio_context_info***) = nullptr;
    void (DECT2_CALL* context_info_list_free)(iio_context_info**) = nullptr;
    const char* (DECT2_CALL* context_info_get_description)(const iio_context_info*) = nullptr;
    const char* (DECT2_CALL* context_info_get_uri)(const iio_context_info*) = nullptr;
    iio_context* (DECT2_CALL* create_context_from_uri)(const char*) = nullptr;
    void (DECT2_CALL* context_destroy)(iio_context*) = nullptr;
    iio_device* (DECT2_CALL* context_find_device)(const iio_context*, const char*) = nullptr;
    int (DECT2_CALL* context_set_timeout)(iio_context*, unsigned) = nullptr;
    iio_channel* (DECT2_CALL* device_find_channel)(const iio_device*, const char*, bool) = nullptr;
    long long (DECT2_CALL* channel_attr_write)(const iio_channel*, const char*, const char*) = nullptr;
    int (DECT2_CALL* channel_attr_write_longlong)(const iio_channel*, const char*, long long) = nullptr;
    void (DECT2_CALL* channel_enable)(iio_channel*) = nullptr;
    void (DECT2_CALL* channel_disable)(iio_channel*) = nullptr;
    iio_buffer* (DECT2_CALL* device_create_buffer)(const iio_device*, size_t, bool) = nullptr;
    void (DECT2_CALL* buffer_destroy)(iio_buffer*) = nullptr;
    long long (DECT2_CALL* buffer_refill)(iio_buffer*) = nullptr;
    void (DECT2_CALL* buffer_cancel)(iio_buffer*) = nullptr;
    void* (DECT2_CALL* buffer_start)(const iio_buffer*) = nullptr;
    void* (DECT2_CALL* buffer_end)(const iio_buffer*) = nullptr;
    long long (DECT2_CALL* channel_attr_read)(const iio_channel*, const char*, char*, size_t) = nullptr;   // optional
    void (DECT2_CALL* strerror_)(int, char*, size_t) = nullptr;                                            // optional
    // optional: device attributes (the AD9361's reference clock, "xo_correction", for the frequency correction)
    long long (DECT2_CALL* device_attr_read)(const iio_device*, const char*, char*, size_t) = nullptr;
    int (DECT2_CALL* device_attr_write_longlong)(const iio_device*, const char*, long long) = nullptr;
    long long (DECT2_CALL* device_attr_write)(const iio_device*, const char*, const char*) = nullptr;   // optional: the AD9361's FIR filter is loaded as text
    // optional: the context attributes, where the firmware names itself ("fw_version", "tezuka-v0.3.21" on a Tezuka)
    const char* (DECT2_CALL* context_get_attr_value)(const iio_context*, const char*) = nullptr;
    int (DECT2_CALL* device_set_kernel_buffers_count)(const iio_device*, unsigned) = nullptr;   // optional: more buffers queued in the kernel
    bool ok = false;
    bool v1 = false;   // libiio 1.x: another API (iio_create_context(params, uri), blocks instead of buffers), not supported yet
    DynLib lib;
    IioApi() {
        if (nativeDisabled() || !lib.open(libNames("iio", {".0", ".1"}, {"libiio.dll", "iio.dll"}))) return;
        {
            void (DECT2_CALL* get_version)(unsigned*, unsigned*, char*) = nullptr;
            void* (DECT2_CALL* scan1)(const void*, const char*) = nullptr;
            unsigned major = 0, minor = 0;
            char tag[8] = {0};
            if (lib.opt(get_version, "iio_library_get_version")) get_version(&major, &minor, tag);
            if (major >= 1 || (!lib.opt(create_scan_context, "iio_create_scan_context") && lib.opt(scan1, "iio_scan"))) {
                v1 = true;
                fprintf(stderr, "%s\n", kV1Msg);
                fflush(stderr);
                return;
            }
        }
        ok = lib.get(create_scan_context, "iio_create_scan_context") && lib.get(scan_context_destroy, "iio_scan_context_destroy") &&
             lib.get(scan_context_get_info_list, "iio_scan_context_get_info_list") && lib.get(context_info_list_free, "iio_context_info_list_free") &&
             lib.get(context_info_get_description, "iio_context_info_get_description") && lib.get(context_info_get_uri, "iio_context_info_get_uri") &&
             lib.get(create_context_from_uri, "iio_create_context_from_uri") && lib.get(context_destroy, "iio_context_destroy") &&
             lib.get(context_find_device, "iio_context_find_device") && lib.get(context_set_timeout, "iio_context_set_timeout") &&
             lib.get(device_find_channel, "iio_device_find_channel") && lib.get(channel_attr_write, "iio_channel_attr_write") &&
             lib.get(channel_attr_write_longlong, "iio_channel_attr_write_longlong") && lib.get(channel_enable, "iio_channel_enable") &&
             lib.get(channel_disable, "iio_channel_disable") && lib.get(device_create_buffer, "iio_device_create_buffer") &&
             lib.get(buffer_destroy, "iio_buffer_destroy") && lib.get(buffer_refill, "iio_buffer_refill") && lib.get(buffer_cancel, "iio_buffer_cancel") &&
             lib.get(buffer_start, "iio_buffer_start") && lib.get(buffer_end, "iio_buffer_end");
        if (ok) {
            lib.opt(channel_attr_read, "iio_channel_attr_read"); lib.opt(strerror_, "iio_strerror");
            lib.opt(device_attr_read, "iio_device_attr_read"); lib.opt(device_attr_write_longlong, "iio_device_attr_write_longlong"); lib.opt(device_attr_write, "iio_device_attr_write");
            lib.opt(context_get_attr_value, "iio_context_get_attr_value");
            lib.opt(device_set_kernel_buffers_count, "iio_device_set_kernel_buffers_count");
        }
    }
    // libiio reports failures as negative errno values (or NULL and errno)
    std::string text(int code) const {
        const int e = code < 0 ? -code : code;
        char b[200] = {0};
        if (strerror_) strerror_(e, b, sizeof b);
        else snprintf(b, sizeof b, "%s", strerror(e));
        return std::string(b) + " (" + std::to_string(e) + ")";
    }
    static constexpr const char* kV1Msg = "libiio 1.x found: OnAir needs libiio 0.23-0.26 for now";
};
IioApi& iio() { static IioApi a; return a; }

namespace {

std::string mhz(double hz) {
    char b[32];
    const double m = hz / 1e6;
    snprintf(b, sizeof b, std::fabs(m - std::round(m)) < 1e-6 ? "%.0f" : "%.6g", m);
    return b;
}

// Below 2.083 Msps the AD9361 needs its decimating FIR: the converter runs at twice the rate and the FIR (16 taps is all its clock chain allows
// here) halves it. This one is a lowpass at 0.23 of the converter rate (0.46 of the output): -1 dB at 0.38, 42 dB down from 0.61. It gives DAB its
// 2.048 Msps straight from the radio: no resampling in software, half the data over the cable. Taps are in 1/32768 (the same for RX and TX).
constexpr long long kFirTaps[16] = {-282, -25, 891, 389, -2075, -1732, 5360, 13694, 13694, 5360, -1732, -2075, 389, 891, -25, -282};
constexpr double kFirMaxRate = 2.1e6, kFirMinRate = 2.048e6;   // the output rates the FIR is used for (the converter itself runs 2.083 Msps and up)
bool wantsFir(double rateHz) { return rateHz >= kFirMinRate - 1 && rateHz < kFirMaxRate; }
std::string firConfig() {
    std::string c = "RX 3 GAIN 0 DEC 2\nTX 3 GAIN 0 INT 2\n";
    for (long long t : kFirTaps) c += std::to_string(t) + "," + std::to_string(t) + "\n";
    return c;
}

constexpr double kUsbMaxRate = 4e6;   // USB 2: the continuous rate a Pluto keeps up without losing samples (measured)
constexpr double kUsbNetRate = 3e6;   // the same cable as a USB-gadget network (ip:192.168.2.1, pluto.local): iiod over it keeps up with less
// The link runs over the Pluto's USB cable: "usb:...", and its default network address ip:192.168.2.1 (pluto.local), which is the USB-gadget
// Ethernet on the same USB 2 cable and keeps up with even less than libusb does. A Pluto on a real network (another address) may go faster.
bool overUsbCable(const std::string& uri) { return uri.rfind("usb:", 0) == 0 || uri == "ip:192.168.2.1" || uri == "ip:pluto.local"; }
// the steady rate of the link at uri (0 = a real network, not limited here): the USB-gadget network carries less than libusb, so a mode that
// fits in a few Msps (DVB-S/S2 of the 1.5 Msym/s QO-100 beacon, #59) is not run faster than it keeps up with there
double usbLinkRate(const std::string& uri) { return !overUsbCable(uri) ? 0 : uri.rfind("usb:", 0) == 0 ? kUsbMaxRate : kUsbNetRate; }

// Whether the board at each URI runs Tezuka firmware (known once it was opened or probed): only then the IQ format can be chosen
std::map<std::string, bool> gTezuka;
bool isTezukaFw(const char* fw) {
    if (!fw) return false;
    std::string low = fw;
    for (auto& c : low) c = (char)tolower((unsigned char)c);
    return low.find("tezuka") != std::string::npos;
}

// The RX LO range the radio reported when it was last opened (stock AD9363 firmware: 325-3800 MHz, AD9364 mode: 70-6000 MHz)
std::mutex gRangeMu;
std::map<std::string, std::pair<double, double>> gRange;
// The serial number of the Pluto at each USB URI, from the list: a USB URI is "usb:<bus>.<device address>.<interface>", and the address
// changes every time the radio is plugged in again
std::map<std::string, std::string> gSerial;

// "0456:b673 (Analog Devices Inc. PlutoSDR (ADALM-PLUTO)), serial=104473..." -> "104473..."
std::string serialIn(const std::string& desc) {
    const size_t p = desc.find("serial=");
    if (p == std::string::npos) return {};
    std::string s = desc.substr(p + 7);
    const size_t e = s.find_first_of(" ,)");
    if (e != std::string::npos) s.resize(e);
    return s;
}

// The USB URI the Pluto with this serial number has now ("" when it is not there)
std::string usbUriOf(const std::string& serial) {
    std::string found;
    if (serial.empty()) return found;
    if (IioApi::iio_scan_context* sc = iio().create_scan_context("usb", 0)) {
        IioApi::iio_context_info** info = nullptr;
        const auto n = iio().scan_context_get_info_list(sc, &info);
        for (long i = 0; i < (long)n && found.empty(); i++) {
            const char* desc = iio().context_info_get_description(info[i]);
            const char* uri = iio().context_info_get_uri(info[i]);
            if (desc && uri && serialIn(desc) == serial) found = uri;
        }
        if (info) iio().context_info_list_free(info);
        iio().scan_context_destroy(sc);
    }
    return found;
}

// "[-3 1 71]" (first, step, last) -> the first and last value (a and b stay as they are when s is not such a range)
bool rangeIn(const char* s, double& a, double& b) {
    double x = 0, st = 0, y = 0;
    if (sscanf(s, " [ %lf %lf %lf ]", &x, &st, &y) != 3 || !(y > x)) return false;
    a = x; b = y;
    return true;
}

// Whether something answers on the IIO network port (30431) of this address within ms: a Pluto on the network (or its USB-gadget Ethernet)
bool iiodAnswers(const char* ip, int ms) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    SOCKET sk = socket(AF_INET, SOCK_STREAM, 0);
    if (sk == INVALID_SOCKET) return false;
    u_long nb = 1;
    ioctlsocket(sk, FIONBIO, &nb);
#else
    int sk = socket(AF_INET, SOCK_STREAM, 0);
    if (sk < 0) return false;
    fcntl(sk, F_SETFL, fcntl(sk, F_GETFL, 0) | O_NONBLOCK);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(30431);
    bool ok = inet_pton(AF_INET, ip, &a.sin_addr) == 1;
    if (ok) {
        connect(sk, (sockaddr*)&a, sizeof a);   // non-blocking: the answer comes through select()
        fd_set w;
        FD_ZERO(&w);
        FD_SET(sk, &w);
        timeval tv{ms / 1000, (ms % 1000) * 1000};
        ok = select((int)sk + 1, nullptr, &w, nullptr, &tv) > 0;
        if (ok) {
            int err = 0;
            socklen_t len = sizeof err;
            ok = getsockopt(sk, SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0;
        }
    }
#ifdef _WIN32
    closesocket(sk);
#else
    close(sk);
#endif
    return ok;
}

// The IPv4 addresses of this PC's own network cards (not loopback, not link-local)
std::vector<uint32_t> ownAddresses() {
    std::vector<uint32_t> v;   // host byte order
#ifdef _WIN32
    ULONG size = 0;
    GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, nullptr, &size);
    std::vector<char> buf(size + 1024);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, list, &size) == NO_ERROR) {
        for (auto* a = list; a; a = a->Next) {
            if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next)
                if (u->Address.lpSockaddr && u->Address.lpSockaddr->sa_family == AF_INET)
                    v.push_back(ntohl(reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr));
        }
    }
#else
    ifaddrs* l = nullptr;
    if (getifaddrs(&l) == 0) {
        for (auto* a = l; a; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !(a->ifa_flags & IFF_LOOPBACK))
                v.push_back(ntohl(reinterpret_cast<sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr));
        freeifaddrs(l);
    }
#endif
    std::vector<uint32_t> r;
    for (uint32_t a : v) if ((a >> 16) != 0xA9FE && (a >> 24) != 127) r.push_back(a);
    return r;
}

// Every address in the /24 around this PC's own addresses that answers on the IIO port (30431), found with one parallel connect round
std::vector<std::string> lanIiod(int ms) {
    std::vector<std::string> found;
    const std::vector<uint32_t> own = ownAddresses();
    std::vector<uint32_t> targets;
    for (uint32_t a : own) {
        const uint32_t net = a & 0xFFFFFF00u;
        for (uint32_t h = 1; h < 255; h++) {
            const uint32_t t = net | h;
            bool mine = false, dup = false;
            for (uint32_t o : own) if (o == t) mine = true;
            for (uint32_t x : targets) if (x == t) dup = true;
            if (!mine && !dup && targets.size() < 500) targets.push_back(t);
        }
    }
    if (targets.empty()) return found;
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return found;
    typedef SOCKET sock_t;
    const sock_t kBad = INVALID_SOCKET;
#else
    typedef int sock_t;
    const sock_t kBad = -1;
#endif
    std::vector<sock_t> socks(targets.size(), kBad);
    fd_set w;
    FD_ZERO(&w);
    sock_t top = 0;
    for (size_t i = 0; i < targets.size(); i++) {
        sock_t sk = socket(AF_INET, SOCK_STREAM, 0);
        if (sk == kBad) continue;
#ifdef _WIN32
        u_long nb = 1;
        ioctlsocket(sk, FIONBIO, &nb);
#else
        if (sk >= FD_SETSIZE) { close(sk); continue; }
        fcntl(sk, F_SETFL, fcntl(sk, F_GETFL, 0) | O_NONBLOCK);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(30431);
        a.sin_addr.s_addr = htonl(targets[i]);
        connect(sk, (sockaddr*)&a, sizeof a);
        socks[i] = sk;
        FD_SET(sk, &w);
        if (sk > top) top = sk;
    }
    timeval tv{ms / 1000, (ms % 1000) * 1000};
    if (select((int)top + 1, nullptr, &w, nullptr, &tv) > 0) {
        for (size_t i = 0; i < targets.size(); i++) {
            if (socks[i] == kBad || !FD_ISSET(socks[i], &w)) continue;
            int err = 0;
            socklen_t len = sizeof err;
            if (getsockopt(socks[i], SOL_SOCKET, SO_ERROR, (char*)&err, &len) == 0 && err == 0) {
                char b[32];
                const uint32_t t = targets[i];
                snprintf(b, sizeof b, "%u.%u.%u.%u", (t >> 24) & 255, (t >> 16) & 255, (t >> 8) & 255, t & 255);
                found.push_back(b);
            }
        }
    }
    for (sock_t sk : socks) {
        if (sk == kBad) continue;
#ifdef _WIN32
        closesocket(sk);
#else
        close(sk);
#endif
    }
    return found;
}

} // namespace

class PlutoSource : public NativeSource {
public:
    explicit PlutoSource(std::string uri) : uri_(std::move(uri)) {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto it = gSerial.find(uri_);
        if (it != gSerial.end()) serial_ = it->second;
    }
    ~PlutoSource() override { stop(); }
    // (the multi-channel receiver's shared gain control shows this)
    std::string gainDescription() const override {
        if (!gainMode_.empty() && gainMode_ != "manual") return "Radio AGC: " + gainMode_;
        const double g = gainShown_.load();
        return g < 0 ? std::string() : "RF gain " + std::to_string((int)std::lround(g)) + " dB (manual)";
    }

protected:
    bool openDevice(const TuneSettings& s, std::string& err) override {
        if (iio().v1) { err = IioApi::kV1Msg; openFailure_ = OpenFailure::Final; return false; }
        errno = 0;
        ctx_ = iio().create_context_from_uri(uri_.c_str());
        if (!ctx_ && uri_.rfind("usb:", 0) == 0 && !serial_.empty()) {
            // plugged in again (or the USB bus was reset): the same radio has another USB address now, found by its serial number
            const std::string now = usbUriOf(serial_);
            if (!now.empty() && now != uri_) {
                fprintf(stderr, "PlutoSDR %s is now at %s\n", serial_.c_str(), now.c_str());
                fflush(stderr);
                uri_ = now;
                errno = 0;
                ctx_ = iio().create_context_from_uri(uri_.c_str());
            }
        }
        if (!ctx_) {
            const int e = errno;
            err = "PlutoSDR: cannot connect (" + uri_ + ")" + (e ? ": " + iio().text(e) : std::string());
            if (kOnWindows && uri_.rfind("usb:", 0) == 0) err += std::string(". ") + windowsUsbDriverHint();
            return false;
        }
        iio().context_set_timeout(ctx_, 3000);
        phy_ = iio().context_find_device(ctx_, "ad9361-phy");
        rx_ = iio().context_find_device(ctx_, "cf-ad9361-lpc");
        if (!phy_ || !rx_) { err = "PlutoSDR: the AD9361 devices were not found"; return false; }
        rxCh_ = iio().device_find_channel(phy_, "voltage0", false);
        lo_ = iio().device_find_channel(phy_, "altvoltage0", true);
        i_ = iio().device_find_channel(rx_, "voltage0", false);
        q_ = iio().device_find_channel(rx_, "voltage1", false);
        if (!rxCh_ || !lo_ || !i_ || !q_) { err = "PlutoSDR: the receive channels were not found"; return false; }
        // the reference clock the driver computes with (Hz; 40 MHz plus the radio's stored calibration): the frequency correction moves it
        xoBase_ = 0; xoPpm_ = 0;
        gainMode_.clear();
        fw_.clear(); tezuka_ = false;
        if (iio().context_get_attr_value) {
            if (const char* fw = iio().context_get_attr_value(ctx_, "fw_version")) {
                fw_ = fw;
                tezuka_ = isTezukaFw(fw);
                { std::lock_guard<std::mutex> lk(gRangeMu); gTezuka[uri_] = tezuka_; }
                fprintf(stderr, "PlutoSDR firmware: %s%s\n", fw_.c_str(), tezuka_ ? " (Tezuka)" : "");
                fflush(stderr);
            }
        }
        if (iio().device_attr_read && iio().device_attr_write_longlong) {
            char v[64] = {0};
            double hz = 0;
            if (iio().device_attr_read(phy_, "xo_correction", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &hz) == 1 && hz > 1e6) xoBase_ = hz;
        }
        loHz_ = 70e6; hiHz_ = 6e9;
        char avail[128] = {0};   // "[70000000 1 6000000000]": first, step, last
        if (iio().channel_attr_read && iio().channel_attr_read(lo_, "frequency_available", avail, sizeof avail - 1) > 0) {
            double a = 0, b = 0;
            if (rangeIn(avail, a, b)) {
                loHz_ = a; hiHz_ = b;
                std::lock_guard<std::mutex> lk(gRangeMu);
                gRange[uri_] = {a, b};
            }
        }
        return configure(s, err, false);
    }
    // The highest rate this Pluto's link (USB 2, 100 Mbit Ethernet, gigabit...) carries without losing samples, at most `want`: each candidate
    // rate is run for half a second and the bytes that arrive are counted. A radio that cannot be kept up with loses blocks on its own side
    // without any error, and a decoder cannot lock on a stream full of holes. The result is remembered per address for this run.
    double probeLink(double want, std::string& note) {
        static std::mutex mu;
        static std::map<std::string, std::pair<double, std::chrono::steady_clock::time_point>> known;
        {   // a result is used for 15 s (a radio that is opened again at once, after a stop or an unplug), then it is measured again
            std::lock_guard<std::mutex> lk(mu);
            auto it = known.find(uri_);
            if (it != known.end() && std::chrono::steady_clock::now() - it->second.second < std::chrono::seconds(15)) return std::min(want, it->second.first);
        }
        std::vector<double> cand{want};
        for (double c : {10e6, 8e6, 6e6, 5e6, 4e6, 3e6, 2.5e6}) if (c < want * 0.999) cand.push_back(c);
        iio().channel_enable(i_);
        iio().channel_enable(q_);
        double best = 2.1e6;
        for (double c : cand) {
            if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(c)) != 0) continue;
            IioApi::iio_buffer* b = iio().device_create_buffer(rx_, kBlock, false);
            if (!b) continue;
            iio().buffer_refill(b);   // the first one only fills the pipe
            const auto t0 = std::chrono::steady_clock::now();
            double bytes = 0, dt = 0;
            while (dt < 0.5) {
                const auto r = iio().buffer_refill(b);
                if (r <= 0) break;
                bytes += (double)r;
                dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            }
            iio().buffer_destroy(b);
            const double got = dt > 0 ? bytes / 4.0 / dt : 0;   // 4 bytes per I/Q sample
            if (got >= c * 0.96) { best = c; break; }
        }
        {
            std::lock_guard<std::mutex> lk(mu);
            known[uri_] = {best, std::chrono::steady_clock::now()};
        }
        if (best < want * 0.999) {
            char b[200];
            snprintf(b, sizeof b, "PlutoSDR: this connection carries %.1f Msps without losing samples: running at %.1f Msps instead of %.1f", best / 1e6, best / 1e6, want / 1e6);
            note = b;
        }
        return std::min(want, best);
    }
    bool configure(const TuneSettings& s, std::string& err, bool live) override {
        if (!ctx_) return false;
        if (!live) {
            if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
            // the converter runs 0.52 - 61.44 Msps; below 2.083 Msps it needs a decimation filter: stay at or above that
            double want = std::min(std::max(s.sampleRate > 0 ? s.sampleRate : 10e6, 2.1e6), 61.44e6);
            if (firOn_) setFir(false);   // a rate chosen now may not be one for the FIR
            if (radioFlag(s, "linklimit") && !wantsFir(s.sampleRate)) {   // the receiver needs every sample: no more than the link carries
                std::string note;
                want = probeLink(want, note);
                if (!note.empty()) { fprintf(stderr, "%s\n", note.c_str()); fflush(stderr); if (err.empty()) err = note; }
            }
            if (wantsFir(s.sampleRate)) {
                // 2.048 Msps: the converter at twice that, the FIR (loaded and switched on there) halves it
                if (setFirRate(s.sampleRate)) want = s.sampleRate;
                else { fprintf(stderr, "PlutoSDR: the radio did not accept the decimating FIR: running at 2.1 Msps, resampled in software\n"); fflush(stderr); }
            }
            if (!firOn_)
                if (const int r = iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(want)); r != 0) { err = "PlutoSDR: the sample rate was refused: " + iio().text(r); return false; }
            // Tezuka: with only the first channel enabled the FPGA sends 8-bit I/Q pairs (half the bytes on the cable, twice the rate); both: 16-bit
            cs8_ = tezuka_ && radioOption(s, "iqformat", "cs16") == "cs8";
            if (overUsbCable(uri_) && !radioFlag(s, "linklimit")) {   // (with linklimit the link was measured: no guess to warn about)
                const std::string n = usbLinkNote(want, usbLinkRate(uri_) * (cs8_ ? 2 : 1), tezuka_, cs8_);
                if (!n.empty()) { fprintf(stderr, "%s\n", n.c_str()); fflush(stderr); if (err.empty()) err = n; }
            }
            double bw = s.basebandFilterHz > 0 ? s.basebandFilterHz : want * 0.95;
            if (radioFlag(s, "linklimit")) bw = std::min(bw, want * 0.95);   // the link made the rate lower: the filter follows it (no aliasing)
            iio().channel_attr_write_longlong(rxCh_, "rf_bandwidth", (long long)std::llround(std::min(56e6, std::max(0.2e6, bw))));
            rate_ = want;
            {   // the rate the converter clocks really give (the receiver is set up with what the radio reports)
                char v[64] = {0};
                double got = 0;
                if (iio().channel_attr_read && iio().channel_attr_read(rxCh_, "sampling_frequency", v, sizeof v - 1) > 0 && sscanf(v, "%lf", &got) == 1 && got > 0) rate_ = got;
            }
            requested_ = s.sampleRate;
            if (!radioFlag(s, "linklimit")) checkRate(s.sampleRate, err);   // (with linklimit the lower rate is the measured link, said once)
            iio().channel_enable(i_);
            if (cs8_) iio().channel_disable(q_); else iio().channel_enable(q_);
            // 8 kernel buffers (as SDR++ does) to ride out a late reader, on every firmware (libiio 0.x has the call, 1.x does not)
            if (iio().device_set_kernel_buffers_count) iio().device_set_kernel_buffers_count(rx_, kKernelBuffers);
            buf_ = iio().device_create_buffer(rx_, kBlock, false);
            if (!buf_) { err = "PlutoSDR: cannot allocate the sample buffer"; return false; }
        }
        // the gain control: manual, with OnAir's gain (the default), or the AD9361's own AGC (TuneSettings "gainmode": slow_attack, fast_attack)
        const std::string gm = radioOption(s, "gainmode", "manual");
        if (!live || gm != gainMode_) {
            if (iio().channel_attr_write(rxCh_, "gain_control_mode", gm.c_str()) >= 0) gainMode_ = gm;
            else { fprintf(stderr, "PlutoSDR: the gain mode %s was refused\n", gm.c_str()); fflush(stderr); }
        }
        // Frequency correction: the reference clock the driver computes with is moved by the clock error (a clock that runs fast by ppm is
        // told so), which corrects the tuning and the sample rate; a driver without xo_correction: the frequency asked for
        const double ppm = radioPpm(s);
        double tuneHz = s.centerHz;
        if (xoBase_ > 0) {
            if (ppm != xoPpm_) {
                if (iio().device_attr_write_longlong(phy_, "xo_correction", std::llround(xoBase_ * (1.0 + ppm * 1e-6))) == 0) xoPpm_ = ppm;
                else { fprintf(stderr, "PlutoSDR: the reference clock correction of %.2f ppm was refused\n", ppm); fflush(stderr); }
            }
            if (ppm != xoPpm_) tuneHz = ppmCorrectedHz(s.centerHz, ppm - xoPpm_);
        } else tuneHz = ppmCorrectedHz(s.centerHz, ppm);
        const double f = s.centerHz;
        // A live update that only changes the gain (the receiver's AGC does that often) must not write the LO again: that re-programs the
        // synthesizer, and the carrier's phase jumps, which breaks the lock of the decoders.
        const bool sameLo = live && tunedHz_ == tuneHz;
        if (f < loHz_ || f > hiHz_ || (!sameLo && iio().channel_attr_write_longlong(lo_, "frequency", (long long)std::llround(tuneHz)) != 0)) {
            err = "PlutoSDR could not tune to " + mhz(f) + " MHz (range " + mhz(loHz_) + "-" + mhz(hiHz_) + " MHz)";
            if (hiHz_ <= 3.8e9) err += ": the stock firmware tunes 325-3800 MHz, 70-6000 MHz needs the AD9364 setting (fw_setenv attr_name compatible; fw_setenv attr_val ad9364)";
            openFailure_ = OpenFailure::Final;
            return false;
        }
        tunedHz_ = tuneHz;
        // The highest gain depends on the frequency (the AD9361's gain tables: 73 dB below 1300 MHz, 71 dB to 4000 MHz, 62 dB above), and a
        // gain above it is refused (EINVAL) with the old gain left in place: the driver's own range for this frequency, else the tables
        double gmin = -1, gmax = f < 1.3e9 ? 73 : f < 4e9 ? 71 : 62;
        {
            char v[64] = {0};
            if (iio().channel_attr_read && iio().channel_attr_read(rxCh_, "hardwaregain_available", v, sizeof v - 1) > 0) rangeIn(v, gmin, gmax);
        }
        const double g = std::min(gmax, std::max(std::max(0.0, gmin), s.gainDb));
        if (gainMode_ != "manual" && !gainMode_.empty()) return true;   // the radio's AGC sets the gain (hardwaregain is for manual mode only)
        gainShown_ = g;
        if (const int r = iio().channel_attr_write_longlong(rxCh_, "hardwaregain", (long long)std::llround(g)); r != 0) {
            fprintf(stderr, "PlutoSDR: the gain %.0f dB was refused at %s MHz: %s\n", g, mhz(f).c_str(), iio().text(r).c_str());
            fflush(stderr);
        }
        return true;
    }
    // Switches the AD9361's FIR on or off (it is one switch for both directions); only ever turned off when this source turned it on
    void setFir(bool on) {
        if (on == firOn_) return;
        if (iio().channel_attr_write_longlong(rxCh_, "filter_fir_en", on ? 1 : 0) == 0) firOn_ = on;
        else if (!on) firOn_ = false;
    }
    // The FIR for `outHz`: converter at 2 * outHz with the filter off, filter loaded, switched on, then the output rate. False if the radio refuses
    bool setFirRate(double outHz) {
        if (!iio().device_attr_write || !phy_) return false;
        if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(2 * outHz)) != 0) return false;
        const std::string cfg = firConfig();
        if (iio().device_attr_write(phy_, "filter_fir_config", cfg.c_str()) < 0) return false;
        if (iio().channel_attr_write_longlong(rxCh_, "filter_fir_en", 1) != 0) return false;
        firOn_ = true;
        if (iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(outHz)) != 0) { setFir(false); return false; }
        return true;
    }
    void closeDevice() override {
        tunedHz_ = -1;
        if (firOn_ && rxCh_) {   // the radio goes back to a rate it can run without the FIR
            iio().channel_attr_write_longlong(rxCh_, "sampling_frequency", (long long)std::llround(2 * rate_));
            setFir(false);
        }
        // the Pluto keeps its settings after OnAir lets go of it: the reference clock goes back to what it was
        if (phy_ && xoBase_ > 0 && xoPpm_ != 0) iio().device_attr_write_longlong(phy_, "xo_correction", std::llround(xoBase_));
        xoPpm_ = 0;
        if (buf_) { iio().buffer_destroy(buf_); buf_ = nullptr; }
        if (i_) iio().channel_disable(i_);
        if (q_) iio().channel_disable(q_);
        if (ctx_) iio().context_destroy(ctx_);
        ctx_ = nullptr; phy_ = rx_ = nullptr; rxCh_ = lo_ = i_ = q_ = nullptr;
    }
    void streamLoop() override {
        conv_.resize(kBlock);
        int failures = 0;
        while (run_) {
            const auto got = iio().buffer_refill(buf_);
            if (got <= 0) { if (++failures > 20) { fprintf(stderr, "PlutoSDR: stream error, stopping the source\n"); break; } continue; }
            failures = 0;
            size_t n;
            if (cs8_) {
                const int8_t* p = static_cast<const int8_t*>(iio().buffer_start(buf_));
                n = std::min<size_t>((size_t)got / 2, kBlock);   // 2 bytes per I/Q sample
                for (size_t k = 0; k < n; k++) conv_[k] = cf32(p[2 * k] / 128.f, p[2 * k + 1] / 128.f);
            } else {
                const int16_t* p = static_cast<const int16_t*>(iio().buffer_start(buf_));
                n = std::min<size_t>((size_t)got / 4, kBlock);   // 4 bytes per I/Q sample
                for (size_t k = 0; k < n; k++) conv_[k] = cf32(p[2 * k] / 2048.f, p[2 * k + 1] / 2048.f);   // 12-bit samples in 16 bits
            }
            push(conv_.data(), n);
        }
    }
    void interrupt() override { if (buf_) iio().buffer_cancel(buf_); }

private:
    static constexpr size_t kBlock = 1 << 16;
    static constexpr unsigned kKernelBuffers = 8;
    std::string uri_;
    std::string serial_;   // USB: finds the radio again when its URI changed
    IioApi::iio_context* ctx_ = nullptr;
    IioApi::iio_device* phy_ = nullptr;
    IioApi::iio_device* rx_ = nullptr;
    IioApi::iio_channel* rxCh_ = nullptr;
    IioApi::iio_channel* lo_ = nullptr;
    IioApi::iio_channel* i_ = nullptr;
    IioApi::iio_channel* q_ = nullptr;
    IioApi::iio_buffer* buf_ = nullptr;
    double loHz_ = 70e6, hiHz_ = 6e9;
    bool firOn_ = false;              // the decimating FIR was switched on by this source
    double xoBase_ = 0, xoPpm_ = 0;   // the reference clock at open (0 = cannot be corrected) and the correction applied to it
    std::string gainMode_;            // the gain_control_mode written
    double tunedHz_ = -1;             // the LO frequency written last (a gain-only update leaves the LO alone)
    std::atomic<double> gainShown_{-1};   // the hardwaregain written (manual mode), for gainDescription()
    std::string fw_;                  // the firmware version the board reports ("" when it does not say)
    bool tezuka_ = false;             // the firmware is Tezuka
    bool cs8_ = false;                // streaming 8-bit samples (Tezuka only)
};

namespace {
// The entry of the Pluto at uri. probe: a board on the network is asked which firmware it runs (not for one the user typed in: an address
// that does not answer would hold up the listing; its firmware is learnt when it is opened)
DeviceInfo plutoEntry(const std::string& uri, const std::string& desc, bool probe) {
    DeviceInfo d;
    d.kind = DeviceInfo::Native;
    d.board = "pluto";
    d.nativeArgs = uri;
    d.name = "PlutoSDR " + (desc.empty() ? uri : "(" + uri + ")") + (probe ? " (native, experimental)" : " (network)");
    // over USB 2 a Pluto keeps up with about 4 Msps (3 over its USB-gadget network address); over a real network the converter's full rate is offered
    // a mode that needs more still gets it (linkRateFor: some lost samples beat no decode, e.g. DVB-T/T2 at 64/7 Msps)
    d.maxRateHz = 61.44e6; d.minRateHz = kFirMinRate;   // 2.048 Msps through the FIR; a converter rate from 2.083 Msps up otherwise
    d.steadyRateHz = plutoLinkRate(uri);
    d.gainMinDb = 0; d.gainMaxDb = 73;
    d.minFreqHz = 70e6; d.maxFreqHz = 6e9;
    d.settings = {ppmSetting(0.01),
                  choiceSetting("gainmode", "Gain mode",
                                "Manual: the gain slider (and OnAir's AGC) set the AD9361's gain (the default).\n"
                                "Slow / fast attack: the AD9361's own AGC sets it (fast attack for bursts); the gain slider then has no effect.",
                                {"manual", "slow_attack", "fast_attack"}, {"Manual", "Radio AGC, slow attack", "Radio AGC, fast attack"}, "manual")};
    bool tez = false, known = false;
    {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto t = gTezuka.find(uri);
        if (t != gTezuka.end()) { tez = t->second; known = true; }
    }
    // a board on the network can be asked which firmware it runs without taking anything from a receiver (a USB one is learnt when opened)
    if (probe && !known && uri.rfind("ip:", 0) == 0 && iio().context_get_attr_value && !iio().v1) {
        if (IioApi::iio_context* c = iio().create_context_from_uri(uri.c_str())) {
            tez = isTezukaFw(iio().context_get_attr_value(c, "fw_version"));
            iio().context_destroy(c);
            std::lock_guard<std::mutex> lk(gRangeMu);
            gTezuka[uri] = tez;
        }
    }
    if (tez)
        d.settings.push_back(choiceSetting("iqformat", "IQ format",
                                           "Tezuka firmware only. CS16: 16-bit samples (the 12-bit converter, the default).\n"
                                           "CS8: 8-bit samples, half the data on the cable, so a higher sample rate over USB or a slow network; less dynamic range.",
                                           {"cs16", "cs8"}, {"CS16", "CS8"}, "cs16", true));
    {
        std::lock_guard<std::mutex> lk(gRangeMu);
        auto it = gRange.find(uri);
        if (it != gRange.end()) { d.minFreqHz = it->second.first; d.maxFreqHz = it->second.second; }
        const std::string sn = serialIn(desc);
        if (!sn.empty()) gSerial[uri] = sn;
    }
    return d;
}
} // namespace

// a Pluto the user added by address (Add network radio): over a real network, also at 192.168.2.1 (a Pluto+ on Ethernet), so no USB 2 limit
double plutoLinkRate(const std::string& uri) { return usbLinkRate(uri); }
DeviceInfo plutoDevice(const std::string& uri) { DeviceInfo d = plutoEntry(uri, "", false); d.steadyRateHz = 0; return d; }

void listPluto(std::vector<DeviceInfo>& out) {
    if (!iio().ok && !iio().v1) return;
    auto add = [&](const std::string& uri, const std::string& desc) {
        for (const auto& d : out) if (d.board == "pluto" && d.nativeArgs == uri) return;
        out.push_back(plutoEntry(uri, desc, true));
    };
    // USB, plus the boards that announce themselves on the network (mDNS). A libiio built without DNS-SD fails the whole "usb:ip" scan, and
    // libiio 0.19 knows only one backend per scan: then USB alone
    for (const char* backends : {"usb:ip", "usb"}) {
        IioApi::iio_scan_context* sc = iio().v1 ? nullptr : iio().create_scan_context(backends, 0);
        if (!sc) continue;
        IioApi::iio_context_info** info = nullptr;
        const auto n = iio().scan_context_get_info_list(sc, &info);
        if (n <= 0) { if (info) iio().context_info_list_free(info); iio().scan_context_destroy(sc); continue; }
        for (long i = 0; i < (long)n && i < 16; i++) {
            const char* desc = iio().context_info_get_description(info[i]);
            const char* uri = iio().context_info_get_uri(info[i]);
            if (!uri || !desc) continue;
            const std::string dd = desc;
            if (dd.find("PlutoSDR") != std::string::npos || dd.find("ADALM-PLUTO") != std::string::npos || dd.find("AD9361") != std::string::npos) add(uri, dd);
        }
        if (info) iio().context_info_list_free(info);
        iio().scan_context_destroy(sc);
        break;
    }
    if (!iio().v1) {
        // the Pluto's default address needs no discovery (a quick look at its IIO port) ...
        if (iiodAnswers("192.168.2.1", 300)) add("ip:192.168.2.1", "");
        // ... and anything on this PC's own networks that answers on the IIO port and has the AD9361 (every address of each /24, in parallel)
        for (const std::string& ip : lanIiod(400)) {
            const std::string uri = "ip:" + ip;
            bool known = false;
            for (const auto& d : out) if (d.board == "pluto" && d.nativeArgs == uri) known = true;
            if (known) continue;
            if (IioApi::iio_context* c = iio().create_context_from_uri(uri.c_str())) {
                const bool pluto = iio().context_find_device(c, "ad9361-phy") != nullptr;
                iio().context_destroy(c);
                if (pluto) add(uri, "");
            }
        }
    }
    if (const char* u = getenv("DECT2_PLUTO_URI")) add(u, "");   // a PlutoSDR on the network, e.g. ip:192.168.2.1
}

std::unique_ptr<IqSource> makePluto(const DeviceInfo& d) {
    return std::make_unique<PlutoSource>(d.nativeArgs);
}

} // namespace native
} // namespace dect2
