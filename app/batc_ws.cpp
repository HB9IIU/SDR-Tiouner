// BATC QO-100 wideband spectrum — WebSocket client over TLS (OpenSSL).
// Modelled after EasyDATVclone/pluto_spectrum.py: manual HTTP upgrade,
// binary WebSocket frame reader, background thread with auto-reconnect.
#include "batc_ws.h"
#include <chrono>
static double batcNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

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
#include <openssl/err.h>
#include <cstring>
#include <cstdio>
#include <random>

static constexpr const char* kHost = "eshail.batc.org.uk";
static constexpr int kPort = 443;
static constexpr const char* kPath = "/wb/fft";
static constexpr double kStaleSeconds = 5.0;
static constexpr int kReconnectMs = 2000;

// Read exactly n bytes from SSL.  Returns false on error or quit.
static bool sslReadFull(SSL* ssl, void* buf, int n, const std::atomic<bool>& quit) {
    char* p = (char*)buf;
    int left = n;
    while (left > 0 && !quit.load(std::memory_order_relaxed)) {
        int r = SSL_read(ssl, p, left);
        if (r <= 0) return false;
        p += r;
        left -= r;
    }
    return left == 0;
}

// Generate a random base64-encoded 16-byte key for the WebSocket handshake.
static std::string wsKey() {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<int> dist(0, 63);
    std::string key;
    for (int i = 0; i < 22; i++) key += b64[dist(gen)];
    key += "==";
    return key;
}

void BatcSpectrum::start() {
    if (thread_.joinable()) return;
    quit_.store(false, std::memory_order_relaxed);
    thread_ = std::thread(&BatcSpectrum::run, this);
}

void BatcSpectrum::stop() {
    quit_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) thread_.join();
}

std::vector<uint16_t> BatcSpectrum::latest() const {
    std::lock_guard<std::mutex> lk(mu_);
    if (frame_.empty()) return {};
    if (batcNow() - frameTime_ > kStaleSeconds) return {};
    return frame_;
}

void BatcSpectrum::run() {
    while (!quit_.load(std::memory_order_relaxed)) {
        connectAndRead();
        connected_.store(false, std::memory_order_relaxed);
        // wait before reconnecting, but check quit_ every 100 ms
        for (int i = 0; i < kReconnectMs / 100 && !quit_.load(std::memory_order_relaxed); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

bool BatcSpectrum::connectAndRead() {
    netInit();

    // Resolve host
    struct addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(kHost, "443", &hints, &res) != 0 || !res) return false;

    sock_t sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock == kBadSock) { freeaddrinfo(res); return false; }

    // Set a connect timeout (5 seconds)
#ifdef _WIN32
    DWORD tv = 5000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof tv);
#else
    struct timeval tv{5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif

    if (connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        freeaddrinfo(res);
        sockClose(sock);
        return false;
    }
    freeaddrinfo(res);

    // Set up TLS
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { sockClose(sock); return false; }
    SSL* ssl = SSL_new(ctx);
    if (!ssl) { SSL_CTX_free(ctx); sockClose(sock); return false; }
    SSL_set_fd(ssl, (int)sock);
    SSL_set_tlsext_host_name(ssl, kHost);
    if (SSL_connect(ssl) != 1) {
        SSL_free(ssl); SSL_CTX_free(ctx); sockClose(sock);
        return false;
    }

    // Increase receive timeout for the data phase (30 seconds to handle slow periods)
#ifdef _WIN32
    tv = 30000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);
#else
    tv = {30, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#endif

    // WebSocket handshake
    std::string key = wsKey();
    char req[512];
    snprintf(req, sizeof req,
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: fft\r\n"
        "\r\n", kPath, kHost, key.c_str());

    if (SSL_write(ssl, req, (int)strlen(req)) <= 0) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); sockClose(sock);
        return false;
    }

    // Read HTTP response (expect "101 Switching Protocols")
    char hdr[2048];
    int hdrLen = 0;
    bool found101 = false;
    while (hdrLen < (int)sizeof(hdr) - 1) {
        int r = SSL_read(ssl, hdr + hdrLen, 1);
        if (r <= 0) break;
        hdrLen++;
        if (hdrLen >= 4 && hdr[hdrLen-1] == '\n' && hdr[hdrLen-2] == '\r' && hdr[hdrLen-3] == '\n' && hdr[hdrLen-4] == '\r') {
            hdr[hdrLen] = 0;
            found101 = strstr(hdr, " 101 ") != nullptr;
            break;
        }
    }
    if (!found101) {
        SSL_shutdown(ssl); SSL_free(ssl); SSL_CTX_free(ctx); sockClose(sock);
        return false;
    }

    connected_.store(true, std::memory_order_relaxed);

    // Read WebSocket frames (binary, unmasked from server)
    std::vector<uint8_t> message;
    while (!quit_.load(std::memory_order_relaxed)) {
        uint8_t head[2];
        if (!sslReadFull(ssl, head, 2, quit_)) break;

        bool fin = head[0] & 0x80;
        int opcode = head[0] & 0x0F;
        uint64_t len = head[1] & 0x7F;

        if (len == 126) {
            uint8_t ext[2];
            if (!sslReadFull(ssl, ext, 2, quit_)) break;
            len = (uint64_t)ext[0] << 8 | ext[1];
        } else if (len == 127) {
            uint8_t ext[8];
            if (!sslReadFull(ssl, ext, 8, quit_)) break;
            len = 0;
            for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
        }

        if (len > 16 * 1024 * 1024) break;  // sanity limit

        std::vector<uint8_t> payload((size_t)len);
        if (len > 0 && !sslReadFull(ssl, payload.data(), (int)len, quit_)) break;

        if (opcode == 8) break;  // close frame
        if (opcode == 9) {       // ping -> pong
            uint8_t pong[2] = {0x8A, 0x00};  // fin + pong, 0 length
            SSL_write(ssl, pong, 2);
            continue;
        }

        if (opcode == 2 || opcode == 0) {  // binary or continuation
            message.insert(message.end(), payload.begin(), payload.end());
            if (fin) {
                size_t count = message.size() / 2;
                if (count > 0) {
                    std::vector<uint16_t> bins(count);
                    memcpy(bins.data(), message.data(), count * 2);
                    // data is little-endian Uint16, same as x86 native
                    {
                        std::lock_guard<std::mutex> lk(mu_);
                        frame_ = std::move(bins);
                        frameTime_ = batcNow();
                    }
                }
                message.clear();
            }
        }
    }

    SSL_shutdown(ssl);
    SSL_free(ssl);
    SSL_CTX_free(ctx);
    sockClose(sock);
    return true;
}
