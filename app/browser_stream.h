#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>

// Owns only the helper processes it starts. The HTTP source is supplied by NetTuner.
class BrowserStream {
public:
    ~BrowserStream() { stop(); }
    bool start(const std::string& source, bool lan, const std::string& workDir,
               int webPort = 8889, int rtspPort = 8554, int mediaPort = 8189,
               bool lowResource = false);
    // An empty source pauses publishing while the page/server stay available.
    // Identity also changes on a retune with the same service ID and URL.
    void setSource(const std::string& source, const std::string& identity = "");
    void stop();
    bool active() const { return active_; }
    std::string status() const;
    static std::string url(const std::string& host = "127.0.0.1");
private:
    std::atomic<bool> quit_{false}, active_{false};
    std::thread worker_;
    mutable std::mutex mu_;
    std::string status_;
    std::string source_, identity_;
    uint64_t sourceRevision_ = 0;
    void message(const std::string& s);
};
