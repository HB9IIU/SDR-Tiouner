// The native radio drivers (source_native.cpp) against fake vendor libraries: listing, streaming a known tone, retuning, changing the rate, stopping.
// (The SDRplay fake checks the driver's calls and settings; the struct layouts themselves are checked in sdrplay_api_min.h.)
#include "dect2/source.h"
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>
#include <dlfcn.h>
using namespace dect2;

static int failures = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); failures++; } } while (0)

static bool copyFile(const std::string& from, const std::string& to) {
    std::ifstream i(from, std::ios::binary);
    std::ofstream o(to, std::ios::binary);
    if (!i || !o) return false;
    o << i.rdbuf();
    return true;
}

static double (*fakeState)(const char*) = nullptr;
static double st(const char* k) { return fakeState ? fakeState(k) : -1e300; }

// reads from the ring for `ms` milliseconds; returns the number of samples and the mean of the first block
static size_t drain(IqRing& ring, int ms, std::complex<float>& mean) {
    size_t total = 0;
    std::vector<cf32> buf(1 << 16);
    bool first = true;
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < end) {
        const size_t n = ring.read(buf.data(), buf.size());
        if (n && first) { std::complex<double> a = 0; for (size_t i = 0; i < n; i++) a += std::complex<double>(buf[i]); mean = std::complex<float>((float)(a.real() / n), (float)(a.imag() / n)); first = false; }
        total += n;
        if (!n) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return total;
}

// ---- native-robust: library lookup, open retries, reconnect after an unplug, USB error texts and the Linux USB hints
#include "../core/src/native_common.h"
#include "dect2/usb_diag.h"
#include <fcntl.h>
#include <functional>

// runs f with standard error going to a file, and returns what was written there
static std::string captureStderr(const std::string& file, const std::function<void()>& f) {
    fflush(stderr);
    const int saved = dup(2);
    const int fd = open(file.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (saved < 0 || fd < 0) { f(); return {}; }
    dup2(fd, 2);
    close(fd);
    f();
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    std::ifstream i(file);
    return std::string((std::istreambuf_iterator<char>(i)), std::istreambuf_iterator<char>());
}

static void writeFile(const std::string& path, const std::string& text) { std::ofstream o(path); o << text << "\n"; }

static void nativeRobustChecks(const std::string& dir, const char* ext, const std::vector<DeviceInfo>& list) {
    using namespace dect2::native;
    printf("native-robust:\n");
    const std::string logFile = dir + "/stderr.txt";

    // #12: a missing required function is reported once, naming the library; a missing optional one is not
    {
        DynLib lib;
        const std::string log = captureStderr(logFile, [&] {
            CHECK(lib.open({dir + "/librtlsdr" + ext}), "the fake library does not open");
            void (*fn)() = nullptr;
            CHECK(!lib.opt(fn, "rtlsdr_no_such_optional"), "optional lookup found a missing function");
            CHECK(!lib.get(fn, "rtlsdr_no_such_function") && !lib.get(fn, "rtlsdr_another_missing"), "lookup found a missing function");
            CHECK(lib.get(fn, "rtlsdr_open"), "rtlsdr_open not found");
        });
        CHECK(log.find("lacks rtlsdr_no_such_function (too old, or another library with the same name): rtlsdr radio disabled") != std::string::npos, "no message for the missing function: %s", log.c_str());
        CHECK(log.find("rtlsdr_no_such_optional") == std::string::npos && log.find("rtlsdr_another_missing") == std::string::npos, "logged more than once, or an optional function: %s", log.c_str());
    }

    // #27: versioned library names, newest first
    {
        CHECK(newerVersion("4.9.0", "4.8.0") && newerVersion("1.10", "1.9") && newerVersion("0.6.0", "0.6") && !newerVersion("2.0", "10.0") && !newerVersion("1.2", "1.2"), "version order");
        const std::string vd = dir + "/versions";
        mkdir(vd.c_str(), 0755);
#ifdef __APPLE__
        for (const char* v : {"1.2", "1.10", "0"}) writeFile(vd + "/libfoo." + v + ".dylib", "");
        writeFile(vd + "/libfoobar.9.dylib", "");
        const std::vector<std::string> want = {vd + "/libfoo.1.10.dylib", vd + "/libfoo.1.2.dylib", vd + "/libfoo.0.dylib"};
#else
        for (const char* v : {"1.2", "1.10", "0"}) writeFile(vd + "/libfoo.so." + v, "");
        writeFile(vd + "/libfoobar.so.9", "");
        const std::vector<std::string> want = {vd + "/libfoo.so.1.10", vd + "/libfoo.so.1.2", vd + "/libfoo.so.0"};
#endif
        const auto got = versionedLibs("foo", {vd + "/"});
        CHECK(got == want, "versioned libraries: %zu found, first %s", got.size(), got.empty() ? "-" : got[0].c_str());
    }

    // #9, #3: USB errors keep libusb's name, say what it means, and on Windows point at Zadig where a driver is the likely cause
    {
        const std::string a = usbErrorText(-3, true), b = usbErrorText(-6, true), c = usbErrorText(-12, false), d = usbErrorText(-12, true);
        CHECK(a.find("LIBUSB_ERROR_ACCESS: ") == 0 && a.find(windowsUsbDriverHint()) != std::string::npos, "access on Windows: %s", a.c_str());
        CHECK(b.find("LIBUSB_ERROR_BUSY: ") == 0 && b.find("Zadig") == std::string::npos, "busy: %s", b.c_str());
        CHECK(c.find("LIBUSB_ERROR_NOT_SUPPORTED: ") == 0 && c.find("Zadig") == std::string::npos, "not supported elsewhere: %s", c.c_str());
        CHECK(d.find("zadig.akeo.ie") != std::string::npos, "not supported on Windows: %s", d.c_str());
    }

    const DeviceInfo* rtl = nullptr;
    for (const auto& d : list) if (d.board == "rtlsdr") rtl = &d;
    void* h = dlopen((dir + "/librtlsdr" + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
    auto set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
    fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
    CHECK(rtl && set && fakeState, "the RTL-SDR fake is not there");
    if (!rtl || !set || !fakeState) return;
    TuneSettings t;
    t.centerHz = 522e6; t.sampleRate = 2.4e6; t.gainDb = 30; t.bandwidthMhz = 8;
    std::complex<float> mean(0, 0);

    // #28: an open that fails with "busy" is tried again; one that keeps failing gives up after 3 more tries
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        set("rtl.busyOpens", 2);
        const double opens0 = std::max(0.0, st("rtl.opens"));
        const auto t0 = std::chrono::steady_clock::now();
        bool ok = false;
        const std::string log = captureStderr(logFile, [&] { ok = src->start(t, ring, e); });
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        CHECK(ok, "start after two busy opens failed: %s", e.c_str());
        CHECK(st("rtl.opens") - opens0 == 3, "%.0f opens (3 expected)", st("rtl.opens") - opens0);
        CHECK(secs > 0.9 && secs < 3, "the retries took %.2f s", secs);
        CHECK(log.find("trying again (1 of 3)") != std::string::npos && log.find("trying again (2 of 3)") != std::string::npos && log.find("(3 of 3)") == std::string::npos, "retry log: %s", log.c_str());
        if (ok) src->stop();
        set("rtl.busyOpens", 10);
        const double opens1 = st("rtl.opens");
        const auto t1 = std::chrono::steady_clock::now();
        captureStderr(logFile, [&] { ok = src->start(t, ring, e); });
        const double secs1 = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
        CHECK(!ok && st("rtl.opens") - opens1 == 4, "a radio that stays busy: started %d, %.0f opens (4 expected)", ok, st("rtl.opens") - opens1);
        CHECK(secs1 > 1.3 && secs1 < 3, "giving up took %.2f s (about 1.5 expected)", secs1);
        set("rtl.busyOpens", 0);
    }

    // #13: the radio is unplugged while streaming and comes back: the source opens it again with the settings in use, into the same ring
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        bool ok = false;
        size_t before = 0, away = 0, back = 0;
        const std::string log = captureStderr(logFile, [&] {
            ok = src->start(t, ring, e);
            if (!ok) return;
            before = drain(ring, 300, mean);
            set("rtl.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            std::vector<cf32> sink(1 << 16);
            while (ring.read(sink.data(), sink.size())) {}
            away = drain(ring, 1500, mean);
            t.centerHz = 600e6; t.gainDb = 40;   // changed while the radio is away: used when it is back
            std::string e2;
            CHECK(src->retune(t, e2), "retune while the radio is away: %s", e2.c_str());
            set("rtl.gone", 0);
            const auto t0 = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4) && ring.available() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            back = drain(ring, 400, mean);
            src->stop();
        });
        CHECK(ok, "start failed: %s", e.c_str());
#ifdef __APPLE__
        CHECK(st("rtl.syncReads") > 0, "macOS should read the RTL-SDR block by block (issue #14), not with read_async");
#else
        CHECK(st("rtl.syncReads") <= 0, "only macOS reads the RTL-SDR block by block");
#endif
        CHECK(before > 8000, "only %zu samples before the unplug", before);
        CHECK(away == 0, "%zu samples while the radio was away", away);
        CHECK(back > 8000, "only %zu samples after the radio came back", back);
        CHECK(std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "samples after the reconnect wrong: %.3f %.3f", mean.real(), mean.imag());
        CHECK(st("rtl.freq") == 600e6 && st("rtl.gain") == 370, "reopened with freq %.0f gain %.0f (the settings of the retune expected)", st("rtl.freq"), st("rtl.gain"));
        CHECK(log.find("radio lost") != std::string::npos, "no \"radio lost\": %s", log.c_str());
        CHECK(log.find("radio reconnected") != std::string::npos, "no \"radio reconnected\": %s", log.c_str());
        const size_t na = log.find("radio not available yet");
        CHECK(na != std::string::npos && log.find("radio not available yet", na + 1) == std::string::npos, "the reason the radio is not back should be logged once: %s", log.c_str());
        const size_t after = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(ring.available() == after, "samples kept arriving after stop()");
    }
    // a radio that is away when stop() is called: stop() returns at once (no reconnect attempt holds it up)
    {
        IqRing ring(1 << 22);
        auto src = makeSource(*rtl);
        std::string e;
        captureStderr(logFile, [&] {
            if (!src->start(t, ring, e)) return;
            set("rtl.gone", 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            const auto t0 = std::chrono::steady_clock::now();
            src->stop();
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            CHECK(secs < 0.5, "stop() while the radio is away took %.2f s", secs);
        });
        set("rtl.gone", 0);
    }
    fakeState = nullptr;

    // #8: the Linux USB check against a fake /sys and /dev tree
    {
        const std::string root = dir + "/root";
        for (const char* d : {"", "/sys", "/sys/bus", "/sys/bus/usb", "/sys/bus/usb/devices", "/sys/module", "/sys/module/dvb_usb_rtl28xxu", "/dev", "/dev/bus", "/dev/bus/usb", "/dev/bus/usb/001",
                              "/sys/bus/usb/devices/1-1", "/sys/bus/usb/devices/1-2", "/sys/bus/usb/devices/1-2:1.0", "/sys/bus/usb/devices/1-3", "/sys/bus/usb/devices/usb1",
                              "/sys/bus/usb/devices/1-4", "/sys/bus/usb/devices/1-4:1.0"})
            mkdir((root + d).c_str(), 0755);
        auto dev = [&](const char* name, const char* vid, const char* pid, int devnum, int mode) {
            const std::string p = root + "/sys/bus/usb/devices/" + name;
            writeFile(p + "/idVendor", vid); writeFile(p + "/idProduct", pid); writeFile(p + "/busnum", "1"); writeFile(p + "/devnum", std::to_string(devnum));
            char node[64];
            snprintf(node, sizeof node, "/dev/bus/usb/001/%03d", devnum);
            writeFile(root + node, "");
            chmod((root + node).c_str(), mode);
        };
        dev("1-1", "1d50", "6089", 5, 0);       // a HackRF this user may not open
        dev("1-2", "0bda", "2838", 6, 0666);    // an RTL-SDR held by the TV driver
        dev("1-3", "046d", "c52b", 7, 0);       // a mouse
        dev("usb1", "1d6b", "0002", 1, 0);      // the root hub
        dev("1-4", "1df7", "2500", 8, 0666);    // an RSP1 held by the kernel's Mirics driver (#17)
        symlink("../../../../bus/usb/drivers/dvb_usb_rtl28xxu", (root + "/sys/bus/usb/devices/1-2:1.0/driver").c_str());
        symlink("../../../../bus/usb/drivers/msi2500", (root + "/sys/bus/usb/devices/1-4:1.0/driver").c_str());
        setenv("DECT2_SYSFS_ROOT", root.c_str(), 1);
        const auto hints = usbRadioHints();
        unsetenv("DECT2_SYSFS_ROOT");
        for (const auto& s : hints) printf("  hint: %s\n", s.c_str());
        const bool root0 = geteuid() == 0;   // root may open anything
        CHECK(hints.size() == (root0 ? 2u : 3u), "%zu hints", hints.size());
        bool hack = false, tv = false, miri = false;
        for (const auto& s : hints) {
            if (s.find("HackRF is plugged in but this user may not open it: install OnAir's udev rules") == 0) hack = true;
            if (s.find("The Linux TV driver dvb_usb_rtl28xxu holds the RTL-SDR: run sudo modprobe -r dvb_usb_rtl28xxu") == 0) tv = true;
            if (s.find("The Linux driver msi2500 holds the SDRplay RSP") == 0) miri = true;
        }
        CHECK((hack || root0) && tv && miri, "hints: HackRF permission %d, RTL TV driver %d, SDRplay held by msi2500 %d", hack, tv, miri);
    }
}
// ---- end native-robust

// ---- radios: the RTL-SDR's direct sampling below 24 MHz, tuning ranges and their error, the RTL-SDR's rate limit
static void radiosChecks(const char* dir, const char* ext) {
    printf("radios:\n");
    void (*set)(const char*, double) = nullptr;
    auto use = [&](const char* stem) {
        void* h = dlopen((std::string(dir) + "/lib" + stem + ext).c_str(), RTLD_NOW | RTLD_NOLOAD);
        set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(set && fakeState, "the fake lib%s is not loaded", stem);
        return set && fakeState;
    };
    auto find = [](const char* board) {
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) return d;
        return DeviceInfo{};
    };
    auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
    std::complex<float> mean(0, 0);

    if (use("rtlsdr")) {   // a generic R820T dongle has no HF input: 24-1766 MHz; the Blog V3: 0.5-1766 MHz, direct sampling (Q branch) below 24 MHz, switched live
        {
            const DeviceInfo g = find("rtlsdr");
            CHECK(g.minFreqHz == 24e6 && g.maxFreqHz == 1766e6, "generic RTL range %.0f-%.0f", g.minFreqHz, g.maxFreqHz);
            IqRing ring(1 << 22);
            auto src = makeSource(g);
            TuneSettings t;
            t.centerHz = 7e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
            std::string e;
            const double calls = std::max(0.0, st("rtl.dsCalls"));
            CHECK(!src->start(t, ring, e) && has(e, "RTL-SDR could not tune to 7 MHz (range 24-1766 MHz)") && std::max(0.0, st("rtl.dsCalls")) == calls, "generic dongle at 7 MHz: %s", e.c_str());
            src.reset();
            // a dongle modified for HF: the Q input picked in the radio settings
            t.radio["ds"] = "q";
            auto q = makeSource(g);
            e.clear();
            const bool ok = q->start(t, ring, e);
            CHECK(ok && st("rtl.ds") == 2 && st("rtl.freq") == 7e6, "generic dongle, Q input picked: %s (ds %.0f)", e.c_str(), st("rtl.ds"));
            if (ok) q->stop();
        }
        set("rtl.v3", 1);
        const DeviceInfo d = find("rtlsdr");
        CHECK(d.minFreqHz == 0.5e6 && d.maxFreqHz == 1766e6, "RTL Blog V3 range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 7e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        std::string e;
        const bool ok = src && src->start(t, ring, e);
        CHECK(ok, "RTL start at 7 MHz: %s", e.c_str());
        if (ok) {
            CHECK(st("rtl.ds") == 2 && st("rtl.freq") == 7e6, "7 MHz: direct sampling %.0f, freq %.0f", st("rtl.ds"), st("rtl.freq"));
            CHECK(drain(ring, 150, mean) > 1000, "no samples with direct sampling");
            t.centerHz = 100e6;
            CHECK(src->retune(t, e) && st("rtl.ds") == 0 && st("rtl.freq") == 100e6, "100 MHz: direct sampling %.0f (%s)", st("rtl.ds"), e.c_str());
            t.centerHz = 14e6;
            CHECK(src->retune(t, e) && st("rtl.ds") == 2 && st("rtl.freq") == 14e6, "back to 14 MHz: direct sampling %.0f", st("rtl.ds"));
            t.centerHz = 2000e6;
            e.clear();
            CHECK(!src->retune(t, e) && has(e, "RTL-SDR could not tune to 2000 MHz (range 0.5-1766 MHz)"), "out of range: %s", e.c_str());
            CHECK(drain(ring, 100, mean) > 1000, "the stream stopped after a refused retune");
            src->stop();
        }
        src.reset();
        // a TV channel at 10 Msps: refused with the reason, not squeezed into 2.56 Msps
        t.centerHz = 522e6; t.sampleRate = 10e6; t.bandwidthMhz = 8;
        auto tv = makeSource(d);
        e.clear();
        CHECK(!tv->start(t, ring, e) && has(e, "RTL-SDR gives at most 3.2 Msps; DVB-T/T2 needs about 10 Msps: use a faster radio"), "10 Msps: %s", e.c_str());
        tv.reset();
        // the Blog V4 converts HF up itself: no direct sampling
        set("rtl.v4", 1);
        const double calls = std::max(0.0, st("rtl.dsCalls"));
        auto v4 = makeSource(d);
        t.centerHz = 7e6; t.sampleRate = 2.048e6; t.bandwidthMhz = 0.2;
        e.clear();
        CHECK(v4->start(t, ring, e), "V4 at 7 MHz: %s", e.c_str());
        CHECK(std::max(0.0, st("rtl.dsCalls")) == calls && st("rtl.freq") == 7e6, "V4: direct sampling switched (%.0f calls)", st("rtl.dsCalls") - calls);
        v4->stop();
        v4.reset();
        set("rtl.v4", 0);
        // the V4 Lite (an R820T, USB product "Blog V4L") upconverts HF in its library as well: no direct sampling, listed from 0.5 MHz
        set("rtl.v3", 0); set("rtl.v4l", 1);
        const DeviceInfo dl = find("rtlsdr");
        CHECK(dl.minFreqHz == 0.5e6, "V4 Lite range from %.0f", dl.minFreqHz);
        auto v4l = makeSource(dl);
        e.clear();
        CHECK(v4l->start(t, ring, e), "V4 Lite at 7 MHz: %s", e.c_str());
        CHECK(std::max(0.0, st("rtl.dsCalls")) == calls && st("rtl.freq") == 7e6, "V4 Lite: direct sampling switched (%.0f calls)", st("rtl.dsCalls") - calls);
        v4l->stop();
        v4l.reset();
        set("rtl.v4l", 0);
    }

    if (use("iio")) {   // over USB a Pluto is offered at 4 Msps at most; a stock one tunes 325-3800 MHz
        const DeviceInfo d = find("pluto");
        CHECK(d.maxRateHz == 4e6, "Pluto over USB: max rate %.0f", d.maxRateHz);
        IqRing ring(1 << 22);
        auto src = makeSource(d);
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 4e6; t.gainDb = 30;
        std::string e;
        CHECK(!src->start(t, ring, e) && has(e, "PlutoSDR could not tune to 100 MHz (range 325-3800 MHz)") && has(e, "AD9364"), "Pluto at 100 MHz: %s", e.c_str());
    }

    fakeState = nullptr;
}
// ---- end radios

// ---- radio settings
// The radios' own settings (DeviceInfo::settings, TuneSettings::radio): each reaches its vendor call, and a radio started without any
// calls nothing new (what OnAir did before the settings existed).
namespace dect2 {
uint32_t hackrfAutoFilterHz(double sampleRate, double channelMhz);   // hackrf_usb.cpp
void hackrfBoardRange(int id, double& lo, double& hi);
}
static bool hasSetting(const DeviceInfo& d, const char* key) {
    for (const auto& r : d.settings) if (r.key == key) return true;
    return false;
}
static void radioSettingsChecks(const char* dir, const char* ext) {
    printf("radio settings:\n");
    void (*set)(const char*, double) = nullptr;
    auto use = [&](const char* file) {
        void* h = dlopen((std::string(dir) + "/" + file).c_str(), RTLD_NOW | RTLD_NOLOAD);
        set = h ? (void (*)(const char*, double))dlsym(h, "fake_set") : nullptr;
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(set && fakeState, "the fake %s is not loaded", file);
        return set && fakeState;
    };
    auto lib = [&](const char* stem) { return std::string("lib") + stem + ext; };
    auto entries = [](const char* board) {
        std::vector<DeviceInfo> l;
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) l.push_back(d);
        return l;
    };
    auto first = [&](const char* board) { const auto l = entries(board); return l.empty() ? DeviceInfo{} : l[0]; };
    auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
    const double kNone = -1e300;   // a call that never happened
    IqRing ring(1 << 22);
    std::string e;

    // the helpers: an untouched correction leaves every frequency exactly as it was
    {
        TuneSettings t;
        CHECK(radioPpm(t) == 0 && ppmCorrectedHz(522e6, radioPpm(t)) == 522e6, "no correction: %.3f", ppmCorrectedHz(522e6, radioPpm(t)));
        t.radio["ppm"] = "20";
        CHECK(std::fabs(ppmCorrectedHz(100e6, radioPpm(t)) - 100e6 / 1.00002) < 1e-3, "20 ppm: %.3f", ppmCorrectedHz(100e6, radioPpm(t)));
        t.radio["ppm"] = "nonsense";
        CHECK(radioPpm(t) == 0, "a value that is not a number counts as none");
    }
    // HackRF: the automatic baseband filter is unchanged for 7-8 MHz at 10 Msps and narrow channels, and wide enough for a 6 MHz channel at 8 Msps
    CHECK(hackrfAutoFilterHz(10e6, 8) == 5000000 && hackrfAutoFilterHz(10e6, 7) == 5000000, "HackRF filter 8/7 MHz at 10 Msps: %u / %u", hackrfAutoFilterHz(10e6, 8), hackrfAutoFilterHz(10e6, 7));
    CHECK(hackrfAutoFilterHz(8e6, 6) == 5000000, "HackRF filter 6 MHz at 8 Msps: %u (5 MHz expected)", hackrfAutoFilterHz(8e6, 6));
    CHECK(hackrfAutoFilterHz(8e6, 5) == 3500000 && hackrfAutoFilterHz(8e6, 1.7) == 3500000 && hackrfAutoFilterHz(4e6, 0.25) == 1750000, "HackRF filter narrow channels: %u %u %u", hackrfAutoFilterHz(8e6, 5), hackrfAutoFilterHz(8e6, 1.7), hackrfAutoFilterHz(4e6, 0.25));
    {
        double lo = 0, hi = 0;
        hackrfBoardRange(2, lo, hi);
        CHECK(lo == 1e6 && hi == 6e9, "HackRF One range %.0f-%.0f", lo, hi);
        hackrfBoardRange(5, lo, hi);
        CHECK(lo == 0.1e6 && hi == 6e9, "HackRF Pro range %.0f-%.0f", lo, hi);
    }

    if (use(lib("rtlsdr").c_str())) {
        DeviceInfo d = first("rtlsdr");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "ds") && !hasSetting(d, "offset"), "R820T settings: ppm %d ds %d offset %d", hasSetting(d, "ppm"), hasSetting(d, "ds"), hasSetting(d, "offset"));
        TuneSettings t;
        t.centerHz = 100e6; t.sampleRate = 2.048e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        const double ds0 = std::max(0.0, st("rtl.dsCalls"));
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("rtl.ppmCalls") == kNone && st("rtl.offsetCalls") == kNone && std::max(0.0, st("rtl.dsCalls")) == ds0 && st("rtl.freq") == 100e6,
              "RTL defaults: ppm calls %.0f, offset calls %.0f, ds calls %.0f, freq %.0f (%s)", st("rtl.ppmCalls"), st("rtl.offsetCalls"), st("rtl.dsCalls") - ds0, st("rtl.freq"), e.c_str());
        if (ok) {
            t.radio["ppm"] = "12.4";
            CHECK(src->retune(t, e) && st("rtl.ppm") == 12 && st("rtl.ppmCalls") == 1 && st("rtl.freq") == 100e6, "RTL 12.4 ppm: %.0f (%.0f calls), freq %.0f", st("rtl.ppm"), st("rtl.ppmCalls"), st("rtl.freq"));
            CHECK(src->retune(t, e) && st("rtl.ppmCalls") == 1, "RTL: the same correction set again (%.0f calls)", st("rtl.ppmCalls"));
            t.radio.erase("ppm");
            CHECK(src->retune(t, e) && st("rtl.ppm") == 0, "RTL: correction back to 0: %.0f", st("rtl.ppm"));
            src->stop();
        }
        src.reset();
        // a Blog V3 with direct sampling switched off: below 24 MHz is out of range, and says why
        set("rtl.v3", 1);
        t.centerHz = 7e6; t.radio["ds"] = "off";
        auto off = makeSource(first("rtlsdr"));
        e.clear();
        CHECK(!off->start(t, ring, e) && has(e, "(range 24-1766 MHz; direct sampling is off)"), "V3, direct sampling off, 7 MHz: %s", e.c_str());
        off.reset();
        // the I input
        t.radio["ds"] = "i";
        auto iin = makeSource(first("rtlsdr"));
        e.clear();
        ok = iin->start(t, ring, e);
        CHECK(ok && st("rtl.ds") == 1, "V3, I input: ds %.0f (%s)", st("rtl.ds"), e.c_str());
        if (ok) iin->stop();
        iin.reset();
        set("rtl.v3", 0);
        t.radio.clear();
        // an E4000 takes offset tuning (an R820T does not: the Blog library switches the antenna power with that call). The listing keeps
        // the tuner it read first (a streaming dongle cannot be opened to ask), so only the driver, which reads it at open, is checked here
        set("rtl.tuner", 1);
        t.centerHz = 100e6;
        auto e4 = makeSource(d);
        e.clear();
        ok = e4->start(t, ring, e);
        CHECK(ok && st("rtl.offsetCalls") == kNone, "E4000 default: offset tuning called (%.0f) %s", st("rtl.offsetCalls"), e.c_str());
        if (ok) {
            t.radio["offset"] = "1";
            CHECK(e4->retune(t, e) && st("rtl.offset") == 1 && st("rtl.offsetCalls") == 1, "E4000 offset tuning: %.0f (%.0f calls)", st("rtl.offset"), st("rtl.offsetCalls"));
            e4->stop();
        }
        e4.reset();
        set("rtl.tuner", 0);
        set("rtl.v4", 1);
        CHECK(!hasSetting(first("rtlsdr"), "ds"), "the Blog V4 offers direct sampling");
        set("rtl.v4", 0);
    }

    if (use("libsdrplay_api.so")) {
        const unsigned kPpm = 0x2, kNotch1a = 0x20, kDab1a = 0x40;
        DeviceInfo d = first("sdrplay");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "rfnotch") && hasSetting(d, "dabnotch") && !hasSetting(d, "hdr"), "RSP1B settings");
        CHECK(d.minFreqHz == 1e3 && d.maxFreqHz == 2e9, "RSP1B range %.0f-%.0f", d.minFreqHz, d.maxFreqHz);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 8e6; t.gainDb = 40; t.bandwidthMhz = 8;
        auto src = makeSource(d);
        e.clear();
        bool ok = src->start(t, ring, e);
        CHECK(ok && st("sdrplay.rfNotch1a") == 0 && st("sdrplay.dabNotch1a") == 0 && st("sdrplay.ppm") == 0, "RSP1B defaults: notch %.0f dab %.0f ppm %.2f", st("sdrplay.rfNotch1a"), st("sdrplay.dabNotch1a"), st("sdrplay.ppm"));
        if (ok) {
            t.centerHz = 530e6;
            CHECK(src->retune(t, e) && ((unsigned)st("sdrplay.reasons") & (kPpm | kNotch1a | kDab1a)) == 0, "RSP1B: an untouched setting was updated (reasons 0x%08x)", (unsigned)st("sdrplay.reasons"));
            t.radio["rfnotch"] = "1"; t.radio["ppm"] = "2.5";
            CHECK(src->retune(t, e) && st("sdrplay.rfNotch1a") == 1 && st("sdrplay.ppm") == 2.5 && ((unsigned)st("sdrplay.reasons") & (kPpm | kNotch1a)) == (kPpm | kNotch1a),
                  "RSP1B FM/MW notch and 2.5 ppm live: notch %.0f ppm %.2f reasons 0x%08x", st("sdrplay.rfNotch1a"), st("sdrplay.ppm"), (unsigned)st("sdrplay.reasons"));
            src->stop();
        }
        src.reset();
        t.radio = {{"dabnotch", "1"}};
        auto s2 = makeSource(d);
        e.clear();
        ok = s2->start(t, ring, e);
        CHECK(ok && st("sdrplay.dabNotch1a") == 1 && st("sdrplay.rfNotch1a") == 0, "RSP1B DAB notch at start: %.0f (FM/MW %.0f)", st("sdrplay.dabNotch1a"), st("sdrplay.rfNotch1a"));
        if (ok) s2->stop();
        s2.reset();
        t.radio.clear();
        // the RSP2 and RSPduo list their Hi-Z input last; it selects AMPORT_1 (the 50 ohm entries keep AMPORT_2)
        for (int model : {2, 3}) {
            set("sdrplay.model", model);
            const auto l = entries("sdrplay");
            CHECK(l.size() == 3 && has(l[2].name, "Hi-Z") && l[2].maxFreqHz == 30e6, "%s: %zu entries (%s)", model == 2 ? "RSP2" : "RSPduo", l.size(), l.size() > 2 ? l[2].name.c_str() : "");
            if (l.size() != 3) continue;
            const char* key = model == 2 ? "sdrplay.amPort2" : "sdrplay.amPortDuo";
            t.centerHz = 7e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2;
            for (int i : {0, 2}) {
                auto s3 = makeSource(l[(size_t)i]);
                e.clear();
                ok = s3->start(t, ring, e);
                CHECK(ok && st(key) == (i == 2 ? 1 : 0), "%s entry %d: AM port %.0f", model == 2 ? "RSP2" : "RSPduo", i, st(key));
                if (ok && i == 2) {
                    t.centerHz = 100e6;
                    e.clear();
                    CHECK(s3->retune(t, e) && has(e, "Hi-Z input is for 1 kHz to 30 MHz"), "Hi-Z at 100 MHz: no note (%s)", e.c_str());
                    t.centerHz = 7e6;
                    if (model == 3) {   // the RSPduo's Hi-Z input has its own MW notch
                        t.radio["rfnotch"] = "1";
                        CHECK(s3->retune(t, e) && st("sdrplay.amNotchDuo") == 1 && st("sdrplay.rfNotchDuo") == 0, "RSPduo Hi-Z notch: AM %.0f RF %.0f", st("sdrplay.amNotchDuo"), st("sdrplay.rfNotchDuo"));
                        t.radio.clear();
                    }
                }
                if (ok) s3->stop();
            }
        }
        // the RSPdx: HDR below 2 MHz only
        set("sdrplay.model", 4);
        d = first("sdrplay");
        CHECK(hasSetting(d, "hdr"), "RSPdx: no HDR setting");
        t.centerHz = 1e6; t.sampleRate = 2e6; t.bandwidthMhz = 0.2; t.radio = {{"hdr", "1"}};
        auto dx = makeSource(d);
        e.clear();
        ok = dx->start(t, ring, e);
        CHECK(ok && st("sdrplay.hdr") == 1, "RSPdx HDR at 1 MHz: %.0f", st("sdrplay.hdr"));
        if (ok) {
            t.centerHz = 10e6;
            CHECK(dx->retune(t, e) && st("sdrplay.hdr") == 0 && ((unsigned)st("sdrplay.reasons1") & 0x1), "RSPdx HDR at 10 MHz: %.0f (ext1 0x%x)", st("sdrplay.hdr"), (unsigned)st("sdrplay.reasons1"));
            dx->stop();
        }
        dx.reset();
        set("sdrplay.model", 0);
    }

    if (use(lib("iio").c_str())) {
        const DeviceInfo d = first("pluto");
        CHECK(hasSetting(d, "ppm") && hasSetting(d, "gainmode"), "Pluto settings");
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 4e6; t.gainDb = 30; t.bandwidthMhz = 1.7;
        auto src = makeSource(d);
        e.clear();
        const bool ok = src->start(t, ring, e);
        CHECK(ok && st("pluto.xoWrites") == kNone && st("pluto.phy:voltage0.gain_control_mode=manual") == 1, "Pluto defaults: xo writes %.0f (%s)", st("pluto.xoWrites"), e.c_str());
        if (ok) {
            t.radio["ppm"] = "10";
            CHECK(src->retune(t, e) && st("pluto.phy.xo_correction") == 40000400 && st("pluto.phy:altvoltage0.frequency") == 522e6, "Pluto 10 ppm: xo %.0f, freq %.0f", st("pluto.phy.xo_correction"), st("pluto.phy:altvoltage0.frequency"));
            t.radio["gainmode"] = "fast_attack"; t.gainDb = 50;
            CHECK(src->retune(t, e) && st("pluto.phy:voltage0.gain_control_mode=fast_attack") == 1 && st("pluto.phy:voltage0.hardwaregain") == 30, "Pluto fast attack: gain written %.0f", st("pluto.phy:voltage0.hardwaregain"));
            src->stop();
            CHECK(st("pluto.phy.xo_correction") == 40000000, "Pluto: reference clock not restored at stop (%.0f)", st("pluto.phy.xo_correction"));
        }
    }

    fakeState = nullptr;
}
// ---- end radio settings

// ---- bias-tee
// Antenna power: set at start when asked, changed on a live retune, and off at stop, for the radios whose library has the call.
static void biasTeeChecks(const char* dir, const char* ext) {
    printf("bias-tee:\n");
    auto find = [](const char* board) {
        std::string e;
        for (const auto& d : listNativeDevices(e)) if (d.board == board) return d;
        return DeviceInfo{};
    };
    struct Radio { const char* board; const char* file; const char* key; };
    const Radio radios[] = {{"rtlsdr", "librtlsdr", "rtl.bias"}, {"sdrplay", "libsdrplay_api", "sdrplay.bias"}};
    for (const auto& r : radios) {
        const DeviceInfo d = find(r.board);
        CHECK(d.hasBiasTee, "%s: hasBiasTee is false", r.board);
        const std::string file = std::string(dir) + "/" + r.file + (std::string(r.board) == "sdrplay" ? ".so" : ext);
        void* h = dlopen(file.c_str(), RTLD_NOW | RTLD_NOLOAD);
        fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
        CHECK(fakeState != nullptr, "%s: the fake library is not loaded", r.board);
        auto src = makeSource(d);
        if (!src || !fakeState) continue;
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 2.5e6; t.gainDb = 30; t.bandwidthMhz = 0.2;
        t.biasTee = true;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "%s: start failed: %s", r.board, e.c_str());
        if (!ok) continue;
        CHECK(st(r.key) == 1, "%s: antenna power %.0f after a start with it on", r.board, st(r.key));
        t.biasTee = false;
        CHECK(src->retune(t, e), "%s: retune failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after a retune with it off", r.board, st(r.key));
        t.biasTee = true;
        CHECK(src->retune(t, e), "%s: retune failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 1, "%s: antenna power %.0f after a retune with it on", r.board, st(r.key));
        if (std::string(r.board) == "sdrplay") {   // the update that carried it names the RSP1A/1B flag
            CHECK(((unsigned)st("sdrplay.reasons") & 0x00000010) != 0, "SDRplay: update reasons 0x%08x lack Rsp1a_BiasTControl", (unsigned)st("sdrplay.reasons"));
        }
        src->stop();
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after stop (must be off)", r.board, st(r.key));
        // with it off at start the radio is not powered
        t.biasTee = false;
        auto again = makeSource(d);
        CHECK(again && again->start(t, ring, e), "%s: second start failed: %s", r.board, e.c_str());
        CHECK(st(r.key) == 0, "%s: antenna power %.0f after a start with it off", r.board, st(r.key));
        if (again) again->stop();
    }
    for (const char* b : {"pluto"}) CHECK(!find(b).hasBiasTee, "%s: hasBiasTee is true (no antenna power on this radio)", b);
}
// libraries without the functions: no radio claims the option and a start with it asked for still works
static void biasTeeMissingChecks() {
    printf("bias-tee (library without it):\n");
    for (const char* b : {"rtlsdr"}) {
        std::string e;
        DeviceInfo d;
        for (const auto& x : listNativeDevices(e)) if (x.board == b) d = x;
        CHECK(d.board == b, "%s not listed", b);
        CHECK(!d.hasBiasTee, "%s: hasBiasTee is true although the library lacks the call", b);
        auto src = makeSource(d);
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = 2.5e6; t.gainDb = 30; t.bandwidthMhz = 0.2; t.biasTee = true;
        CHECK(src && src->start(t, ring, e), "%s: start with the bias-tee asked failed: %s", b, e.c_str());
        if (src) src->stop();
    }
}
// ---- end bias-tee

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: test_native <fake library>\n"); return 2; }
    const std::string fake = argv[1];
    char tmpl[] = "/tmp/onair-native-XXXXXX";
    const char* dir = mkdtemp(tmpl);
    if (!dir) { printf("no temp dir\n"); return 2; }
#ifdef __APPLE__
    const char* ext = ".dylib";
#else
    const char* ext = ".so";
#endif
    for (const char* stem : {"rtlsdr", "iio"})
        if (!copyFile(fake, std::string(dir) + "/lib" + stem + ext)) { printf("cannot copy the fake library\n"); return 2; }
    // the SDRplay API installs libsdrplay_api.so.3 on macOS too (not .dylib); the driver looks for libsdrplay_api.so in the test folder
    if (!copyFile(fake, std::string(dir) + "/libsdrplay_api.so")) { printf("cannot copy the fake library\n"); return 2; }
    setenv("DECT2_NATIVE_LIBDIR", dir, 1);
    auto fileOf = [&](const std::string& board) {
        if (board == "sdrplay") return std::string("libsdrplay_api.so");
        const std::string stem = board == "pluto" ? "iio" : board;
        return "lib" + stem + ext;
    };

    if (argc > 2 && std::string(argv[2]) == "--nobias") {   // the fake libraries lack the bias-tee functions
        biasTeeMissingChecks();
        printf("%d failure(s)\n", failures);
        return failures ? 1 : 0;
    }

    std::string err;
    const auto list = listNativeDevices(err);
    printf("native radios found: %zu\n", list.size());
    for (const auto& d : list) printf("  %-8s %s\n", d.board.c_str(), d.name.c_str());
    {   // a radio with several antenna inputs is listed once per input (the same board)
        std::set<std::string> boards;
        for (const auto& d : list) boards.insert(d.board);
        CHECK(boards.size() == 3, "expected 3 radios, found %zu", boards.size());
    }

    struct Case { const char* board; double wantRate; double wantMinRate; };
    const Case cases[] = {{"rtlsdr", 2.4e6, 0}, {"pluto", 10e6, 0}, {"sdrplay", 10e6, 0}};
    for (const auto& c : cases) {
        printf("%s:\n", c.board);
        const DeviceInfo* dev = nullptr;
        for (const auto& d : list) if (d.board == c.board && !dev) dev = &d;   // the first entry: the input OnAir always used
        CHECK(dev, "radio not listed");
        if (!dev) continue;
        CHECK(dev->kind == DeviceInfo::Native && dev->isRadio() && dev->isGeneric(), "wrong kind");
        auto src = makeSource(*dev);
        CHECK(src != nullptr, "no source");
        if (!src) continue;
        IqRing ring(1 << 22);
        TuneSettings t;
        t.centerHz = 522e6; t.sampleRate = c.wantRate; t.gainDb = 30; t.bandwidthMhz = 8;
        std::string e;
        const bool ok = src->start(t, ring, e);
        CHECK(ok, "start failed: %s", e.c_str());
        if (!ok) continue;
        CHECK(src->realtimeHardware(), "not flagged as hardware");
        std::complex<float> mean(0, 0);
        const size_t n = drain(ring, 400, mean);
        CHECK(n > 8000, "only %zu samples in 400 ms", n);
        CHECK(std::fabs(mean.real() - 0.5f) < 0.02f && std::fabs(mean.imag() + 0.25f) < 0.02f, "samples wrong: mean %.3f %.3f (expected 0.5 -0.25)", mean.real(), mean.imag());
        const std::string b = c.board;
        {   // each radio's copy of the fake library keeps its own record of what it was asked to do
            void* h = dlopen((std::string(dir) + "/" + fileOf(b)).c_str(), RTLD_NOW | RTLD_NOLOAD);
            fakeState = h ? (double (*)(const char*))dlsym(h, "fake_state") : nullptr;
            CHECK(fakeState != nullptr, "the fake library of %s is not loaded", c.board);
        }
        if (b == "rtlsdr") { CHECK(st("rtl.freq") == 522e6, "freq %.0f", st("rtl.freq")); CHECK(st("rtl.gain") == 270, "gain %.0f (the 30 dB request should pick the 27 dB step)", st("rtl.gain")); CHECK(st("rtl.gainmode") == 1, "not manual gain"); }
        if (b == "pluto") { CHECK(st("pluto.connected") == 1, "uri"); CHECK(st("pluto.phy:altvoltage0.frequency") == 522e6, "freq"); CHECK(st("pluto.phy:voltage0.sampling_frequency") == 10e6, "rate"); CHECK(st("pluto.phy:voltage0.hardwaregain") == 30, "gain"); CHECK(st("pluto.phy:voltage0.gain_control_mode=manual") == 1, "not manual"); }
        if (b == "sdrplay") {
            CHECK(dev->name.find("RSP1B") != std::string::npos && dev->serial == "2305012345", "name/serial: %s", dev->name.c_str());
            CHECK(dev->gainMaxDb == 103 && dev->maxRateHz == 10e6 && dev->minRateHz == 2e6, "ranges: gain to %.0f, rate %.0f..%.0f", dev->gainMaxDb, dev->minRateHz, dev->maxRateHz);
            CHECK(st("sdrplay.listLocked") == 1, "the device list was not read under LockDeviceApi");
            CHECK(st("sdrplay.selected") == 1, "not selected");
            CHECK(st("sdrplay.fs") == 10e6, "fsHz %.0f", st("sdrplay.fs"));
            CHECK(st("sdrplay.rf") == 522e6, "rfHz %.0f", st("sdrplay.rf"));
            CHECK(st("sdrplay.bw") == 8000, "IF bandwidth %.0f kHz (8000 expected for an 8 MHz channel)", st("sdrplay.bw"));
            CHECK(st("sdrplay.if") == 0, "not zero IF");
            CHECK(st("sdrplay.decim") == 0, "decimation %.0f at 10 Msps", st("sdrplay.decim"));
            CHECK(st("sdrplay.agc") == 0, "the API's AGC is on");
            CHECK(st("sdrplay.dc") == 1 && st("sdrplay.iq") == 1, "DC/IQ correction off");
            // RSP1B, 420-1000 MHz LNA table {0 7 13 19 20 27 33 39 45 64}: 30 of 103 dB = 93 dB reduction = LNAstate 8 (45 dB) + gRdB 48
            CHECK(st("sdrplay.lna") == 8 && st("sdrplay.grdb") == 48, "gain 30 dB: LNAstate %.0f gRdB %.0f (8 and 48 expected)", st("sdrplay.lna"), st("sdrplay.grdb"));
            CHECK(st("sdrplay.overloadAcks") >= 1, "the power overload report was not acknowledged");
            std::string e2;   // a rescan while streaming keeps the radio in the list (the API no longer lists a selected one)
            bool still = false;
            for (const auto& d : listNativeDevices(e2)) if (d.board == "sdrplay" && d.serial == "2305012345") still = true;
            CHECK(still, "the radio left the list while streaming");
        }

        // live retune: frequency and gain
        t.centerHz = 474e6; t.gainDb = 40;
        CHECK(src->retune(t, e), "retune failed: %s", e.c_str());
        const size_t n2 = drain(ring, 200, mean);
        CHECK(n2 > 3000, "stream stopped after a retune (%zu samples)", n2);
        if (b == "rtlsdr") { CHECK(st("rtl.freq") == 474e6, "retune freq"); CHECK(st("rtl.gain") == 370, "retune gain %.0f", st("rtl.gain")); }
        if (b == "pluto") { CHECK(st("pluto.phy:altvoltage0.frequency") == 474e6, "retune freq"); CHECK(st("pluto.phy:voltage0.hardwaregain") == 40, "retune gain"); }
        if (b == "sdrplay") {   // one live update: frequency and gain (40 of 103 dB = 83 dB reduction = LNAstate 6 (33 dB) + gRdB 50)
            const unsigned why = (unsigned)st("sdrplay.reasons");
            CHECK((why & 0x00020000) && (why & 0x00008000), "update reasons 0x%08x (Tuner_Frf and Tuner_Gr expected)", why);
            CHECK(st("sdrplay.updTuner") == 1, "update not for tuner A");
            CHECK(st("sdrplay.rf") == 474e6, "retune rfHz %.0f", st("sdrplay.rf"));
            CHECK(st("sdrplay.lna") == 6 && st("sdrplay.grdb") == 50, "gain 40 dB: LNAstate %.0f gRdB %.0f (6 and 50 expected)", st("sdrplay.lna"), st("sdrplay.grdb"));
            CHECK(st("sdrplay.inits") == 1, "the stream was restarted for a retune");
        }

        // a different sample rate while running
        t.sampleRate = 2.5e6;
        CHECK(src->retune(t, e) || !e.empty(), "rate change failed");
        const size_t n3 = drain(ring, 200, mean);
        CHECK(n3 > 1000, "stream stopped after a rate change (%zu samples)", n3);
        if (b == "pluto") CHECK(st("pluto.phy:voltage0.sampling_frequency") == 2.5e6, "pluto rate %.0f", st("pluto.phy:voltage0.sampling_frequency"));
        if (b == "sdrplay") {   // restarted at the new rate; the 8 MHz channel no longer fits, so the widest filter below 2.5 Msps
            CHECK(st("sdrplay.fs") == 2.5e6 && st("sdrplay.decim") == 0, "fsHz %.0f decimation %.0f", st("sdrplay.fs"), st("sdrplay.decim"));
            CHECK(st("sdrplay.bw") == 1536, "IF bandwidth %.0f kHz at 2.5 Msps (1536 expected)", st("sdrplay.bw"));
            CHECK(st("sdrplay.inits") == 2 && st("sdrplay.rf") == 474e6, "restart: %.0f inits, rfHz %.0f", st("sdrplay.inits"), st("sdrplay.rf"));
        }

        src->stop();
        const size_t drained = ring.available();
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        CHECK(ring.available() == drained, "samples kept arriving after stop()");
        if (b == "sdrplay") CHECK(st("sdrplay.init") == 0 && st("sdrplay.selected") == 0, "not stopped and released (init %.0f, selected %.0f)", st("sdrplay.init"), st("sdrplay.selected"));
        src.reset();
    }
    radiosChecks(dir, ext);   // ---- radios
    biasTeeChecks(dir, ext);   // ---- bias-tee
    radioSettingsChecks(dir, ext);   // ---- radio settings
    nativeRobustChecks(dir, ext, list);   // ---- native-robust
    // with the feature switched off nothing is listed
    setenv("DECT2_NO_NATIVE", "1", 1);
    (void)listNativeDevices(err);   // (the libraries are already loaded, the switch is read when they are first touched)
    printf(failures ? "FAILED (%d)\n" : "native radio drivers: all checks passed\n", failures);
    return failures ? 1 : 0;
}
