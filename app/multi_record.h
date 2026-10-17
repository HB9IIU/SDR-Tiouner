// Records the untouched MPEG transport stream of every running channel to a .ts file (VLC plays it): no re-encoding, no work for the decoders.
// The files go to one folder, one per channel and tuning: a retune or a lost station ends its file, a new one starts for the next tuning.
#pragma once
#include "dect2/multi_receiver.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class TsRecorder {
public:
    ~TsRecorder() { stopAll(); }

    // Called from the main loop. Writers run while `enabled` is true and the radio runs; they end when it turns false.
    void service(dect2::MultiReceiver& receiver, bool enabled, const std::filesystem::path& directory) {
        namespace fs = std::filesystem;
        if (enabled && !wasEnabled_) {   // a new recording: counters from zero
            files_ = 0; bytes_ = 0; startedAt_ = std::chrono::steady_clock::now(); nextDiskCheck_ = startedAt_; { std::lock_guard<std::mutex> lock(mutex_); problem_.clear(); }
        }
        wasEnabled_ = enabled;
        for (auto it = writers_.begin(); it != writers_.end();) {
            if (it->second->done) { if (it->second->th.joinable()) it->second->th.join(); it = writers_.erase(it); }
            else ++it;
        }
        if (enabled && std::chrono::steady_clock::now() >= nextDiskCheck_) {   // never fill the disk by accident
            nextDiskCheck_ = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            std::error_code ec; fs::create_directories(directory, ec);
            const auto space = fs::space(directory, ec);
            if (!ec && space.available < kMinFreeBytes) { std::lock_guard<std::mutex> lock(mutex_); problem_ = "Recording stopped: the disk is almost full."; }
        }
        for (const auto& channel : receiver.channels()) {
            const bool want = enabled && receiver.running() && channel.config.enabled && problem().empty();
            auto found = writers_.find(channel.id);
            if (!want) {
                if (found != writers_.end()) { found->second->stop = true; if (found->second->th.joinable()) found->second->th.join(); writers_.erase(found); }
                continue;
            }
            if (found != writers_.end()) continue;
            auto stream = receiver.subscribe(channel.id);
            if (!stream) continue;   // the channel just started: the next pass
            auto writer = std::make_unique<Writer>();
            writer->directory = directory;
            writer->mhz = channel.config.satelliteMhz;
            Writer* raw = writer.get();
            writer->th = std::thread([this, raw, stream] { run(*raw, stream); });
            writers_[channel.id] = std::move(writer);
        }
    }
    void stopAll() {
        for (auto& [id, writer] : writers_) writer->stop = true;
        for (auto& [id, writer] : writers_) if (writer->th.joinable()) writer->th.join();
        writers_.clear();
        wasEnabled_ = false;
    }
    // Why a recording had to stop (empty: nothing wrong)
    std::string problem() const { std::lock_guard<std::mutex> lock(mutex_); return problem_; }
    // For the page: {"on":true,"files":3,"bytes":123456,"seconds":42,"dir":"..."}
    std::string json(bool on, const std::filesystem::path& directory) const {
        std::string dir = directory.generic_string(), escaped;
        for (char c : dir) { if (c == '"' || c == '\\') escaped += '\\'; escaped += c; }
        const auto seconds = on ? std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - startedAt_).count() : 0;
        return std::string("{\"on\":") + (on ? "true" : "false") + ",\"files\":" + std::to_string(files_.load()) + ",\"bytes\":" + std::to_string(bytes_.load()) +
               ",\"seconds\":" + std::to_string((long long)seconds) + ",\"dir\":\"" + escaped + "\"}";
    }

private:
    static constexpr uint64_t kMinFreeBytes = 1ull << 30;   // stop when less than 1 GB is free

    struct Writer {
        std::thread th;
        std::atomic<bool> stop{false}, done{false};
        std::filesystem::path directory;
        double mhz = 0;
    };

    static std::string stamp() {
        const std::time_t now = std::time(nullptr); std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char text[32]; std::strftime(text, sizeof text, "%Y%m%d-%H%M%S", &local);
        return text;
    }

    void fail(const std::string& why) { std::lock_guard<std::mutex> lock(mutex_); if (problem_.empty()) problem_ = why; }

    void run(Writer& w, std::shared_ptr<dect2::ChannelStream> stream) {
        namespace fs = std::filesystem;
        std::ofstream out;   // opened with the first packets: a channel that never locks leaves no empty file

        auto lastFlush = std::chrono::steady_clock::now();
        while (!w.stop) {
            std::vector<uint8_t> packets;
            {
                std::unique_lock<std::mutex> lock(stream->mutex);
                stream->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return stream->closed || !stream->queue.empty() || w.stop.load(); });
                if (!stream->queue.empty()) { packets = std::move(stream->queue.front()); stream->queue.pop_front(); stream->bytes -= packets.size(); }
                else if (stream->closed) break;
            }
            if (packets.empty()) continue;
            if (!out.is_open()) {
                std::error_code ec; fs::create_directories(w.directory, ec);
                char freq[24]; std::snprintf(freq, sizeof freq, "%.3f", w.mhz);
                out.open(w.directory / (stamp() + "_" + freq + "MHz.ts"), std::ios::binary);
                if (!out) { fail("Recording stopped: cannot write to " + w.directory.generic_string()); break; }
                files_++;
            }
            out.write(reinterpret_cast<const char*>(packets.data()), (std::streamsize)packets.size());
            if (!out) { fail("Recording stopped: writing to the disk failed."); break; }
            bytes_ += packets.size();
            if (std::chrono::steady_clock::now() - lastFlush > std::chrono::seconds(1)) { out.flush(); lastFlush = std::chrono::steady_clock::now(); }
        }
        if (out.is_open()) out.close();
        { std::lock_guard<std::mutex> lock(stream->mutex); stream->closed = true; }
        w.done = true;
    }

    std::map<unsigned, std::unique_ptr<Writer>> writers_;
    std::atomic<uint64_t> files_{0}, bytes_{0};
    bool wasEnabled_ = false;
    std::chrono::steady_clock::time_point startedAt_{}, nextDiskCheck_{};
    mutable std::mutex mutex_;
    std::string problem_;
};
