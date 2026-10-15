// The browser publisher URL-encodes the tuner's key. Verify decoding and exact parameter matching.
#include "dect2/nettuner.h"
#include "../core/src/netcompat.h"
#include <cstdio>
#include <string>

using namespace dect2;
static std::string httpRequest(int port, const std::string& target, const std::string& method = "GET", const std::string& headers = "") {
    sock_t fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&addr, sizeof addr) != 0) { sockClose(fd); return ""; }
    std::string req = method + " " + target + " HTTP/1.0\r\nHost: localhost\r\n" + headers + "\r\n";
    if (sockSend(fd, req.data(), req.size()) != (long long)req.size()) { sockClose(fd); return ""; }
    std::string response; char data[4096]; long long n;
    while ((n = sockRecv(fd, data, sizeof data)) > 0) response.append(data, (size_t)n);
    sockClose(fd);
    return response;
}
static int code(const std::string& response) {
    int status = 0;
    sscanf(response.c_str(), "HTTP/1.1 %d", &status);
    return status;
}
int main() {
    Engine engine;
    NetTuner tuner(engine);
    NetTunerConfig config; config.key = "a&b +%?";
    bool started = false;
    for (config.port = 19289; config.port < 19309; ++config.port)
        if (tuner.start(config)) { started = true; break; }
    if (!started) return 1;
    const struct { const char* query; int expected; } cases[] = {
        {"key=%61%26%62%20%2B%25%3F", 200},
        {"x=1&key=a%26b+%2B%25%3F&y=2", 200},
        {"key=wrong", 401}, {"notkey=%61%26%62%20%2B%25%3F", 401},
        {"key=%61%26%62%20%2B%25%3Fextra", 401}, {"", 401},
    };
    for (auto& test : cases) {
        int got = code(httpRequest(config.port, std::string("/?") + test.query));
        if (got != test.expected) { fprintf(stderr, "query %s: got %d, expected %d\n", test.query, got, test.expected); return 1; }
    }
    const std::string auth = "key=%61%26%62%20%2B%25%3F";
    auto page = httpRequest(config.port, "/receiver?" + auth);
    const std::string prefix = "const controlToken='";
    auto position = page.find(prefix);
    if (code(page) != 200 || position == std::string::npos) return 1;
    auto token = page.substr(position + prefix.size(), 32);
    const std::string headers = "Origin: http://localhost\r\nX-OnAir-Control: " + token + "\r\n";
    const std::string target = "/api/tune?" + auth + "&frequency=10491.5&symbolRate=1500";
    if (code(httpRequest(config.port, target, "POST", headers)) != 409) return 1;
    WebReceiverState state; state.receiving = true; state.spectrum = {100, 65535}; tuner.setWebState(state);
    auto status = httpRequest(config.port, "/api/receiver?" + auth);
    if (code(status) != 200 || status.find("\"spectrum\":[100,65535]") == std::string::npos) return 1;
    if (code(httpRequest(config.port, target, "POST")) != 403) return 1;
    if (code(httpRequest(config.port, target, "POST", "Origin: http://another-site\r\nX-OnAir-Control: " + token + "\r\n")) != 403) return 1;
    if (code(httpRequest(config.port, "/api/tune?" + auth + "&frequency=NaN&symbolRate=1500", "POST", headers)) != 400) return 1;
    if (code(httpRequest(config.port, "/api/tune?" + auth + "&frequency=10480&symbolRate=1500", "POST", headers)) != 400) return 1;
    if (code(httpRequest(config.port, target, "POST", headers)) != 202) return 1;
    WebTuneRequest tune;
    if (!tuner.takeWebTune(tune) || tune.satelliteMhz != 10491.5 || tune.symbolRateKsps != 1500 || tuner.takeWebTune(tune)) return 1;
    state.receiving = false; state.radios = {{2, "Test radio"}}; state.selectedRadio = 2; tuner.setWebState(state);
    const auto radioTarget = "/api/radio/start?" + auth + "&device=2";
    if (code(httpRequest(config.port, radioTarget, "POST")) != 403) return 1;
    if (code(httpRequest(config.port, "/api/radio/start?" + auth + "&device=0", "POST", headers)) != 400) return 1;
    if (code(httpRequest(config.port, radioTarget, "POST", headers)) != 202) return 1;
    WebRadioRequest radio;
    if (!tuner.takeWebRadio(radio) || radio.action != WebRadioRequest::Start || radio.device != 2 || tuner.takeWebRadio(radio)) return 1;
    state.receiving = true; tuner.setWebState(state);
    if (code(httpRequest(config.port, radioTarget, "POST", headers)) != 409) return 1;
    if (code(httpRequest(config.port, "/api/radio/stop?" + auth, "POST", headers)) != 202) return 1;
    if (!tuner.takeWebRadio(radio) || radio.action != WebRadioRequest::Stop) return 1;
    state.receiving = false; tuner.setWebState(state);
    if (code(httpRequest(config.port, "/api/radio/refresh?" + auth, "POST", headers)) != 202) return 1;
    if (!tuner.takeWebRadio(radio) || radio.action != WebRadioRequest::Refresh) return 1;
    tuner.stop();
    puts("PASS: encoded keys, web spectrum/status, authorised tuning and invalid-input rejection");
}
