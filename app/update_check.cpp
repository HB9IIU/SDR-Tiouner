#include "update_check.h"
#include <chrono>
#include <cstdlib>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
constexpr sock_t kBadSock = INVALID_SOCKET;
static void netInit() { static const bool once = [] { WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }(); (void)once; }
static int sockClose(sock_t s) { return closesocket(s); }
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using sock_t = int;
constexpr sock_t kBadSock = -1;
static void netInit() {}
static int sockClose(sock_t s) { return ::close(s); }
#endif

#include <openssl/ssl.h>

namespace {
constexpr const char* kHost = "api.github.com";
constexpr const char* kRequest = "GET /repos/HB9IIU/SDR-Tiouner/releases/latest HTTP/1.0\r\nHost: api.github.com\r\nUser-Agent: SDR-Tiouner\r\n"
                                 "Accept: application/vnd.github+json\r\nConnection: close\r\n\r\n";
constexpr const char* kReleasesPage = "https://github.com/HB9IIU/SDR-Tiouner/releases/latest";

bool parseVersion(const std::string& text, int out[3]) {
    size_t i = 0;
    if (i < text.size() && (text[i] == 'v' || text[i] == 'V')) ++i;
    for (int part = 0; part < 3; ++part) {
        if (i >= text.size() || text[i] < '0' || text[i] > '9') return false;
        int value = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') { value = value * 10 + (text[i] - '0'); if (value > 100000) return false; ++i; }
        out[part] = value;
        if (part < 2) { if (i >= text.size() || text[i] != '.') return false; ++i; }
    }
    return true;
}

// The tag of the latest release ("v0.1.2"), empty when GitHub cannot be reached or does not answer as expected.
std::string fetchLatestTag() {
    if (const char* fake = std::getenv("SDR_TIOUNER_FAKE_LATEST")) return fake;   // to see the message without a new release (testing)
    netInit();
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(kHost, "443", &hints, &res) != 0 || !res) return {};
    sock_t sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == kBadSock) { freeaddrinfo(res); return {}; }
#ifdef _WIN32
    DWORD timeout = 6000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof timeout);
#else
    timeval timeout{6, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#endif
    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) { freeaddrinfo(res); sockClose(sock); return {}; }
    freeaddrinfo(res);

    std::string reply;
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    SSL* ssl = context ? SSL_new(context) : nullptr;
    if (ssl) {
        SSL_set_fd(ssl, (int)sock);
        SSL_set_tlsext_host_name(ssl, kHost);
        // (the answer is only a version number and the link shown is fixed in the program: nothing is gained by a forged answer)
        if (SSL_connect(ssl) == 1 && SSL_write(ssl, kRequest, (int)std::char_traits<char>::length(kRequest)) > 0) {
            char buffer[4096];
            for (;;) {
                const int got = SSL_read(ssl, buffer, sizeof buffer);
                if (got <= 0) break;
                reply.append(buffer, (size_t)got);
                if (reply.size() > 262144) break;
            }
        }
        SSL_free(ssl);
    }
    if (context) SSL_CTX_free(context);
    sockClose(sock);

    if (reply.compare(0, 9, "HTTP/1.0 ") != 0 && reply.compare(0, 9, "HTTP/1.1 ") != 0) return {};
    if (reply.compare(9, 3, "200") != 0) return {};
    const std::string key = "\"tag_name\"";
    size_t at = reply.find(key);
    if (at == std::string::npos) return {};
    at = reply.find('"', at + key.size());   // the opening quote of the value
    if (at == std::string::npos) return {};
    const size_t end = reply.find('"', at + 1);
    if (end == std::string::npos || end - at > 40) return {};
    return reply.substr(at + 1, end - at - 1);
}

// Sleeps `seconds`, in small steps; false when the program is ending.
bool nap(const std::shared_ptr<std::atomic<bool>>& quit, int seconds) {
    for (int i = 0; i < seconds * 4; ++i) {
        if (quit->load()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    return !quit->load();
}
} // namespace

bool updateIsNewer(const std::string& current, const std::string& latest) {
    int a[3], b[3];
    if (!parseVersion(current, a) || !parseVersion(latest, b)) return false;
    for (int i = 0; i < 3; ++i) if (a[i] != b[i]) return b[i] > a[i];
    return false;
}

void UpdateCheck::start(const std::string& currentVersion) {
    if (started_) return;
    started_ = true;
    auto state = state_;
    // The thread keeps its own share of the state: it may still be waiting for GitHub when the program ends, and nothing waits for it.
    std::thread([state, currentVersion] {
        auto quit = std::shared_ptr<std::atomic<bool>>(state, &state->quit);
        if (!nap(quit, 8)) return;   // the program is up first
        for (;;) {
            const std::string tag = fetchLatestTag();
            if (state->quit.load()) return;
            if (!tag.empty()) {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->available = updateIsNewer(currentVersion, tag);
                state->latest = tag.size() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
            }
            if (!nap(quit, tag.empty() ? 30 * 60 : 12 * 3600)) return;
        }
    }).detach();
}

void UpdateCheck::stop() { state_->quit = true; }

std::string UpdateCheck::json() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    std::string version;
    for (char c : state_->latest) if ((c >= '0' && c <= '9') || c == '.') version += c;   // digits and dots only: it goes into the page
    return std::string("{\"available\":") + (state_->available ? "true" : "false") + ",\"version\":\"" + version + "\",\"url\":\"" + kReleasesPage + "\"}";
}
