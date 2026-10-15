#include "browser_stream.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>
#include <cstdlib>
#include <memory>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char** environ;
#endif

namespace {
namespace fs = std::filesystem;
fs::path utf8Path(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
std::string utf8String(const fs::path& p) {
    auto s = p.u8string(); return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}
fs::path executableDir() {
#ifdef _WIN32
    wchar_t path[32768];
    DWORD n = GetModuleFileNameW(nullptr, path, 32768);
    return fs::path(std::wstring(path, n)).parent_path();
#elif defined(__APPLE__)
    uint32_t n = 0; _NSGetExecutablePath(nullptr, &n);
    std::vector<char> path(n); _NSGetExecutablePath(path.data(), &n);
    return fs::absolute(path.data()).parent_path();
#else
    return fs::read_symlink("/proc/self/exe").parent_path();
#endif
}
fs::path helper(const char* name) {
#ifdef _WIN32
    std::string filename = std::string(name) + ".exe";
#else
    std::string filename = name;
#endif
    auto beside = executableDir() / filename;
    if (fs::is_regular_file(beside)) return beside;
#ifdef _WIN32
    wchar_t found[32768];
    auto wide = fs::path(filename).wstring();
    DWORD n = SearchPathW(nullptr, wide.c_str(), nullptr, 32768, found, nullptr);
    if (n && n < 32768) return fs::path(std::wstring(found, n));
#else
    std::string paths = std::getenv("PATH") ? std::getenv("PATH") : "";
    size_t offset = 0;
    do {
        size_t end = paths.find(':', offset);
        auto path = fs::path(paths.substr(offset, end - offset)) / filename;
        if (access(path.c_str(), X_OK) == 0) return fs::absolute(path);
        if (end == std::string::npos) break;
        offset = end + 1;
    } while (offset <= paths.size());
#endif
    return {};
}

struct Process {
#ifdef _WIN32
    HANDLE process = nullptr, job = nullptr;
    // Windows command-line quoting, including embedded quotes and trailing slashes.
    static std::wstring quote(const std::wstring& s) {
        std::wstring out = L"\""; size_t slashes = 0;
        for (wchar_t c : s) {
            if (c == L'\\') { ++slashes; continue; }
            out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0; out += c;
        }
        out.append(slashes * 2, L'\\'); return out + L"\"";
    }
#else
    pid_t process = -1;
#endif
    bool launch(const fs::path& exe, const std::vector<std::string>& args, const fs::path& log) {
#ifdef _WIN32
        std::wstring cmd = quote(exe.wstring());
        for (auto& a : args) cmd += L" " + quote(utf8Path(a).wstring());
        SECURITY_ATTRIBUTES security{sizeof security, nullptr, TRUE};
        HANDLE output = CreateFileW(log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                   &security, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  &security, OPEN_EXISTING, 0, nullptr);
        if (output == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
            if (output != INVALID_HANDLE_VALUE) CloseHandle(output);
            if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
            return false;
        }
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            CloseHandle(output); CloseHandle(input); return false;
        }
        STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = input; si.hStdOutput = si.hStdError = output;
        PROCESS_INFORMATION pi{};
        bool ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE,
                                 CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr, &si, &pi);
        CloseHandle(output); CloseHandle(input);
        if (!ok) return false;
        process = pi.hProcess;
        if (!AssignProcessToJobObject(job, process)) {
            TerminateProcess(process, 1); CloseHandle(pi.hThread); return false;
        }
        ResumeThread(pi.hThread); CloseHandle(pi.hThread); return true;
#else
        std::vector<std::string> strings{exe.string()};
        strings.insert(strings.end(), args.begin(), args.end());
        std::vector<char*> argv; for (auto& s : strings) argv.push_back(s.data()); argv.push_back(nullptr);
        posix_spawn_file_actions_t actions; posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
        posix_spawn_file_actions_addopen(&actions, 1, log.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
        int result = posix_spawn(&process, exe.c_str(), &actions, nullptr, argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions); return result == 0;
#endif
    }
    bool alive() {
#ifdef _WIN32
        return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
#else
        if (process <= 0) return false;
        if (waitpid(process, nullptr, WNOHANG) == 0) return true;
        process = -1; return false;
#endif
    }
    ~Process() {
#ifdef _WIN32
        if (job) CloseHandle(job);
        if (process) { WaitForSingleObject(process, 2000); CloseHandle(process); }
#else
        if (process > 0) {
            kill(process, SIGTERM);
            for (int i = 0; i < 20; ++i) {
                if (!alive()) return;
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
            kill(process, SIGKILL); waitpid(process, nullptr, 0);
        }
#endif
    }
};
}

void BrowserStream::message(const std::string& s) { std::lock_guard<std::mutex> lock(mu_); status_ = s; }
std::string BrowserStream::status() const { std::lock_guard<std::mutex> lock(mu_); return status_; }
std::string BrowserStream::url(const std::string& host) {
    return "http://" + host + ":8889/onair?controls=false&muted=true&autoplay=true&playsInline=true";
}
void BrowserStream::stop() {
    bool wasActive = active_;
    quit_ = true;
    if (worker_.joinable()) worker_.join();
    active_ = false;
    if (wasActive) message("Browser video stopped.");
}
void BrowserStream::setSource(const std::string& source, const std::string& identity) {
    std::lock_guard<std::mutex> lock(mu_);
    if (source_ == source && identity_ == identity) return;
    source_ = source; identity_ = identity; ++sourceRevision_;
}
bool BrowserStream::start(const std::string& source, bool lan, const std::string& workDir,
                          int webPort, int rtspPort, int mediaPort, bool lowResource) {
    stop();
    try {
        auto mtx = helper("mediamtx"), ffmpeg = helper("ffmpeg");
        if (mtx.empty() || ffmpeg.empty()) {
            message("Browser video needs mediamtx and ffmpeg beside OnAir or on PATH. See README: Browser video.");
            return false;
        }
        auto dir = utf8Path(workDir); fs::create_directories(dir);
        auto config = dir / "browser-mediamtx.yml";
        std::ofstream file(config);
        file << "logLevel: warn\nrtsp: yes\nrtspAddress: 127.0.0.1:" << rtspPort << "\nrtspTransports: [tcp]\n"
                "rtmp: no\nhls: no\nsrt: no\nmoq: no\napi: no\nmetrics: no\npprof: no\nplayback: no\n"
                "webrtc: yes\nwebrtcAddress: " << (lan ? ":" : "127.0.0.1:") << webPort << "\n"
                "webrtcLocalUDPAddress: " << (lan ? ":" : "127.0.0.1:") << mediaPort << "\n"
                "webrtcLocalTCPAddress: ''\nwebrtcIPsFromInterfaces: " << (lan ? "yes" : "no") << "\n"
                "webrtcAdditionalHosts: [127.0.0.1]\n"
                "authInternalUsers:\n"
                "- user: any\n  ips: [127.0.0.1]\n  permissions:\n  - action: publish\n    path: onair\n"
                "- user: any\n  permissions:\n  - action: read\n    path: onair\n"
                "paths:\n  onair:\n    source: publisher\n";
        file.close();
        if (!file) { message("Cannot write browser video configuration."); return false; }
        setSource(source);
        quit_ = false; active_ = true; message("Starting browser video...");
        worker_ = std::thread([this, mtx, ffmpeg, config, dir, rtspPort, lowResource] {
            {
                Process server;
                auto log = dir / "browser-video.log";
                { std::ofstream clear(log); }
                if (!server.launch(mtx, {utf8String(config)}, log)) message("Cannot start MediaMTX. See browser-video.log.");
                else {
                    // MediaMTX starts quickly; retrying the publisher also handles a short startup race.
                    for (int i = 0; i < 10 && !quit_ && server.alive(); ++i)
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    std::unique_ptr<Process> encoder;
                    uint64_t revision = 0;
                    auto retryAt = std::chrono::steady_clock::now();
                    while (!quit_ && server.alive()) {
                        std::string source;
                        uint64_t wantedRevision;
                        { std::lock_guard<std::mutex> lock(mu_); source = source_; wantedRevision = sourceRevision_; }
                        if (wantedRevision != revision) {
                            encoder.reset(); revision = wantedRevision;
                            retryAt = std::chrono::steady_clock::now();
                            message(source.empty() ? "Waiting for reception; browser reconnects automatically." : "Switching browser video to the current channel...");
                        }
                        if (encoder && !encoder->alive()) {
                            encoder.reset();
                            retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                            message("Waiting for video; reconnecting automatically.");
                        }
                        if (!source.empty() && !encoder && std::chrono::steady_clock::now() >= retryAt) {
                            std::vector<std::string> args = {
                        "-hide_banner", "-nostdin", "-loglevel", "warning"};
                            if (lowResource) {
                                args.insert(args.end(), {"-threads", "2", "-filter_threads", "1"});
                            }
                            const bool presented = source.find("/video.nv12") != std::string::npos;
                            if (presented) {
                                const std::vector<std::string> input = {"-f", "rawvideo", "-pixel_format", "nv12",
                                    "-video_size", "960x540", "-framerate", "30", "-probesize", "32", "-analyzeduration", "0"};
                                args.insert(args.end(), input.begin(), input.end());
                            } else {
                                // discardcorrupt + -output_corrupt: drop damaged frames (hold the last good picture until the next
                                // keyframe) instead of showing concealed mosaic blocks.
                                const std::vector<std::string> input = {"-fflags", "+nobuffer+discardcorrupt", "-flags", "-output_corrupt",
                                    "-probesize", "262144", "-analyzeduration", "1000000"};
                                args.insert(args.end(), input.begin(), input.end());
                            }
                            std::vector<std::string> output = {
                        "-rw_timeout", "10000000", "-i", source, "-map", "0:v:0", "-map", "0:a:0?",
                        "-vf", "scale=trunc(iw/2)*2:trunc(ih/2)*2", "-c:v", "libx264",
                        "-preset", "ultrafast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
                        "-profile:v", "baseline", "-bf", "0", "-g", "30", "-crf", "23",
                        "-c:a", "libopus", "-ar", "48000", "-ac", "2", "-b:a", "96k",
                        "-flush_packets", "1", "-f", "rtsp", "-rtsp_transport", "tcp"};
                            if (lowResource) {
                                for (size_t i = 0; i + 1 < output.size(); ++i)
                                    if (output[i] == "-vf") output[i + 1] = "scale=960:540:force_original_aspect_ratio=decrease:force_divisible_by=2";
                                output.insert(output.end(), {"-threads", "2"});
                            }
                            args.insert(args.end(), output.begin(), output.end());
                            if (presented) {
                                const std::vector<std::string> colour = {"-color_primaries", "bt709", "-color_trc", "bt709",
                                    "-colorspace", "bt709", "-color_range", "tv"};
                                args.insert(args.end(), colour.begin(), colour.end());
                            }
                            args.push_back("rtsp://127.0.0.1:" + std::to_string(rtspPort) + "/onair");
                            encoder = std::make_unique<Process>();
                            if (encoder->launch(ffmpeg, args, log))
                                message(presented ? "Web receiver running; video connection stays open across retunes." :
                                    "Browser video running; follows tuning and reconnects automatically (muted autoplay).");
                            else {
                                encoder.reset(); retryAt = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                                message("Cannot start encoder; retrying automatically. See browser-video.log.");
                            }
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    }
                    if (!quit_) message("Cannot start browser video: MediaMTX stopped or a port is busy. See browser-video.log.");
                }
            }
            active_ = false;
        });
        return true;
    } catch (const std::exception& e) { active_ = false; message(e.what()); return false; }
}
