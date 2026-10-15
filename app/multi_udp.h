// The untouched MPEG transport stream of every running channel over UDP (7 TS packets per datagram, as MiniTiouner and VLC/ffplay expect it):
// the browser view is re-encoded to at most 960x540, this is the stream as it came out of the demodulator, at its own resolution and codec.
#pragma once
#include "dect2/multi_receiver.h"
#include "netcompat.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class UdpTsOutputs {
public:
    ~UdpTsOutputs() { stopAll(); }

    // Called from the main loop: one sender per channel that is running, ended again when the channel stops or is retuned (its TS subscribers
    // are closed then) and started afresh for the new tuning. `basePort` + id - 1 is the channel's port.
    void service(dect2::MultiReceiver& receiver, const std::string& host, int basePort, bool enabled) {
        for (auto it = senders_.begin(); it != senders_.end();) {
            if (it->second->done) { if (it->second->th.joinable()) it->second->th.join(); it = senders_.erase(it); }
            else ++it;
        }
        for (const auto& channel : receiver.channels()) {
            const bool want = enabled && receiver.running() && channel.config.enabled;
            auto found = senders_.find(channel.id);
            if (!want) {
                if (found != senders_.end()) { found->second->stop = true; if (found->second->th.joinable()) found->second->th.join(); senders_.erase(found); }
                continue;
            }
            if (found != senders_.end()) continue;
            auto stream = receiver.subscribe(channel.id);
            if (!stream) continue;   // not subscribable yet (the channel just started): the next pass
            auto sender = std::make_unique<Sender>();
            sender->port = basePort + (int)channel.id - 1;
            sender->host = host;
            Sender* raw = sender.get();
            sender->th = std::thread([raw, stream] { run(*raw, stream); });
            senders_[channel.id] = std::move(sender);
        }
    }
    void stopAll() {
        for (auto& [id, sender] : senders_) sender->stop = true;
        for (auto& [id, sender] : senders_) if (sender->th.joinable()) sender->th.join();
        senders_.clear();
    }
    // "udp://127.0.0.1:19000" while the channel's stream is going out, empty otherwise
    std::string address(unsigned id) const {
        const auto it = senders_.find(id);
        if (it == senders_.end() || it->second->done || !it->second->open) return {};
        return "udp://" + it->second->host + ":" + std::to_string(it->second->port);
    }

private:
    struct Sender {
        std::thread th;
        std::atomic<bool> stop{false}, done{false}, open{false};
        std::string host;
        int port = 0;
    };

    static void run(Sender& s, std::shared_ptr<dect2::ChannelStream> stream) {
        using namespace dect2;
        netInit();
        sock_t sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons((uint16_t)s.port);
        if (sock == kBadSock || inet_pton(AF_INET, s.host.c_str(), &to.sin_addr) != 1) {
            if (sock != kBadSock) sockClose(sock);
            { std::lock_guard<std::mutex> lock(stream->mutex); stream->closed = true; }
            s.done = true;
            return;
        }
        s.open = true;
        if ((ntohl(to.sin_addr.s_addr) >> 28) == 0xE) {   // 224.0.0.0/4: multicast; the default TTL of 1 does not leave the first router/switch
            const int ttl = 8; setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
        }
        constexpr size_t kDatagram = 7 * 188;   // the usual size of a TS-over-UDP datagram
        std::vector<uint8_t> pending;
        while (!s.stop) {
            std::vector<uint8_t> packets;
            {
                std::unique_lock<std::mutex> lock(stream->mutex);
                stream->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return stream->closed || !stream->queue.empty() || s.stop.load(); });
                if (stream->closed) break;
                if (!stream->queue.empty()) { packets = std::move(stream->queue.front()); stream->queue.pop_front(); stream->bytes -= packets.size(); }
            }
            if (packets.empty()) continue;
            pending.insert(pending.end(), packets.begin(), packets.end());
            size_t offset = 0, sent = 0;
            while (pending.size() - offset >= kDatagram) {
                sockSendTo(sock, pending.data() + offset, kDatagram, to);
                offset += kDatagram;
                // a long burst (one demodulated frame) goes out in small steps: a player's socket buffer is small
                if (++sent % 64 == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            pending.erase(pending.begin(), pending.begin() + (std::ptrdiff_t)offset);
        }
        if (!pending.empty() && pending.size() % 188 == 0) sockSendTo(sock, pending.data(), pending.size(), to);
        sockClose(sock);
        { std::lock_guard<std::mutex> lock(stream->mutex); stream->closed = true; }
        s.open = false;
        s.done = true;
    }

    std::map<unsigned, std::unique_ptr<Sender>> senders_;
};
