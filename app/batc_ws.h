// BATC QO-100 wideband spectrum via WebSocket.
// Connects to wss://eshail.batc.org.uk/wb/fft (TLS, subprotocol "fft"),
// receives binary Uint16 FFT frames and exposes the latest one for display.
#pragma once
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

struct BatcSpectrum {
    static constexpr double START_MHZ = 10490.5;
    static constexpr double END_MHZ   = 10499.5;
    static constexpr double DISPLAY_FLOOR   = 0.12;
    static constexpr double DISPLAY_CEILING = 0.58;

    void start();   // launch the background reader thread
    void stop();    // shut down and join

    // Thread-safe: returns the latest FFT frame (empty if none or stale >5 s).
    std::vector<uint16_t> latest() const;
    bool isConnected() const { return connected_.load(std::memory_order_relaxed); }

    ~BatcSpectrum() { stop(); }

private:
    void run();                     // the thread entry point
    bool connectAndRead();          // one connection attempt; returns false on error

    mutable std::mutex mu_;
    std::vector<uint16_t> frame_;   // guarded by mu_
    double frameTime_ = 0;         // steady-clock seconds of last frame

    std::atomic<bool> connected_{false};
    std::atomic<bool> quit_{false};
    std::thread thread_;
};
