// The load this program puts on the PC: CPU (as a share of the whole machine, all logical cores) and memory of the receiver itself and of the
// helper programs it started (ffmpeg for the browser video, MediaMTX for WebRTC). Called about once a second from the main loop.
#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <sstream>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

struct LoadSample {
    double receiverPct = 0, ffmpegPct = 0, mediamtxPct = 0;   // share of the whole PC, percent
    double receiverMb = 0, ffmpegMb = 0, mediamtxMb = 0;      // working set
    int processes = 0;
    bool valid = false;
    double totalPct() const { return receiverPct + ffmpegPct + mediamtxPct; }
    double totalMb() const { return receiverMb + ffmpegMb + mediamtxMb; }
};

class LoadMeter {
public:
    LoadSample sample() {
        LoadSample out;
#ifdef _WIN32
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_).count();
        SYSTEM_INFO info; GetSystemInfo(&info);
        const double cores = info.dwNumberOfProcessors > 0 ? info.dwNumberOfProcessors : 1;

        struct Entry { DWORD pid, parent; std::wstring exe; };
        std::map<DWORD, Entry> all;
        if (HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0); snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
            for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) all[pe.th32ProcessID] = {pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile};
            CloseHandle(snap);
        }
        // this process and everything below it (the helpers are its children)
        const DWORD self = GetCurrentProcessId();
        std::map<DWORD, bool> mine; mine[self] = true;
        for (bool grew = true; grew;) {
            grew = false;
            for (const auto& [pid, e] : all) if (!mine.count(pid) && mine.count(e.parent) && pid != e.parent) { mine[pid] = true; grew = true; }
        }
        std::map<DWORD, uint64_t> cpu;
        for (const auto& [pid, flag] : mine) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            if (!h) continue;
            FILETIME created, exited, kernel, user;
            uint64_t used = 0;
            if (GetProcessTimes(h, &created, &exited, &kernel, &user))
                used = ((uint64_t(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) + ((uint64_t(user.dwHighDateTime) << 32) | user.dwLowDateTime);   // 100 ns
            PROCESS_MEMORY_COUNTERS counters{}; double mb = 0;
            if (K32GetProcessMemoryInfo(h, &counters, sizeof counters)) mb = counters.WorkingSetSize / 1048576.0;
            CloseHandle(h);
            cpu[pid] = used;
            double pct = 0;
            const auto before = previous_.find(pid);
            if (valid_ && dt > 0.05 && before != previous_.end() && used >= before->second) pct = (used - before->second) / 1e7 / dt / cores * 100.0;
            std::wstring exe = all.count(pid) ? all[pid].exe : L"";
            for (auto& c : exe) c = (wchar_t)towlower(c);
            if (pid == self) { out.receiverPct += pct; out.receiverMb += mb; }
            else if (exe.find(L"ffmpeg") != std::wstring::npos) { out.ffmpegPct += pct; out.ffmpegMb += mb; }
            else if (exe.find(L"mediamtx") != std::wstring::npos) { out.mediamtxPct += pct; out.mediamtxMb += mb; }
            else { out.receiverPct += pct; out.receiverMb += mb; }   // any other helper counts with the receiver
            ++out.processes;
        }
        previous_ = std::move(cpu);
        last_ = now;
        out.valid = valid_;
        valid_ = true;
#endif
        return out;
    }

    // {"seq":N,"receiver":%,"ffmpeg":%,"mediamtx":%,"total":%,"mb":MB,"receiverMb":..,"ffmpegMb":..,"mediamtxMb":..,"processes":N}
    static std::string json(const LoadSample& s, unsigned seq) {
        std::ostringstream o; o.precision(4);
        o << "{\"seq\":" << seq << ",\"valid\":" << (s.valid ? "true" : "false") << ",\"receiver\":" << s.receiverPct << ",\"ffmpeg\":" << s.ffmpegPct
          << ",\"mediamtx\":" << s.mediamtxPct << ",\"total\":" << s.totalPct() << ",\"mb\":" << s.totalMb() << ",\"receiverMb\":" << s.receiverMb
          << ",\"ffmpegMb\":" << s.ffmpegMb << ",\"mediamtxMb\":" << s.mediamtxMb << ",\"processes\":" << s.processes << "}";
        return o.str();
    }

private:
#ifdef _WIN32
    std::map<DWORD, uint64_t> previous_;
#endif
    std::chrono::steady_clock::time_point last_ = std::chrono::steady_clock::now();
    bool valid_ = false;
};
