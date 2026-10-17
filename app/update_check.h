// Tells the page when a newer SDR-Tiouner is out: a background thread asks GitHub, a few seconds after the start and then every 12 hours, which
// release is the latest. It reads the version number only: it downloads nothing, sends nothing about the user, and the link the page shows is fixed
// in the program. No internet (or no answer) means no message. Switched off with  check_updates=0  in the settings.
#pragma once
#include <atomic>
#include <memory>
#include <mutex>
#include <string>

// "v0.1.2" or "0.1.2" against the version that runs: true when `latest` is higher
bool updateIsNewer(const std::string& current, const std::string& latest);

class UpdateCheck {
public:
    ~UpdateCheck() { stop(); }
    void start(const std::string& currentVersion);
    void stop();
    // For the page: {"available":true,"version":"0.1.2","url":"https://github.com/HB9IIU/SDR-Tiouner/releases/latest"}
    std::string json() const;

private:
    struct State {
        std::mutex mutex;
        std::atomic<bool> quit{false};
        bool available = false;
        std::string latest;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
    bool started_ = false;
};
