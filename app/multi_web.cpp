#include "version.h"
#include "multi_web.h"
#include "multi_web_page.h"
#include "auto_monitor_page.h"
#include "playback_page.h"
#include "multi_bootstrap_css.h"
#include "batc_ws.h"
#include "netcompat.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <random>
#include <sstream>
#include <thread>
#include <algorithm>
#include <limits>
#include <cctype>

using namespace dect2;
namespace {
std::string escape(const std::string& input) {
    std::string out;
    for (unsigned char c : input) {
        if (c == '\\' || c == '"') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c < 32) out += ' ';
        else out += c;
    }
    return out;
}
bool sendAll(sock_t socket, const void* data, size_t count) {
    const char* bytes = static_cast<const char*>(data);
    while (count) {
#ifdef MSG_NOSIGNAL
        auto written = ::send(socket, bytes, count, MSG_NOSIGNAL);
#else
        auto written = sockSend(socket, bytes, count);
#endif
        if (written <= 0) return false;
        bytes += written; count -= written;
    }
    return true;
}
void timeout(sock_t socket) {
#ifdef _WIN32
    DWORD milliseconds = 1000;
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, (const char*)&milliseconds, sizeof milliseconds);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, (const char*)&milliseconds, sizeof milliseconds);
#else
    timeval value{1, 0};
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof value);
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof value);
#endif
}
double jsonNumber(double value) { return std::isfinite(value) ? value : 0; }
}
struct MultiWeb::Impl {
    MultiReceiver& receiver;
    BatcSpectrum batc;
    int port = 47321, videoBase = 18900;
    std::string token;
    std::string playbackOwner, playbackPage;
    std::chrono::steady_clock::time_point playbackUntil{};
    sock_t listener = kBadSock;
    std::atomic<bool> running{false};
    std::thread acceptor;
    std::vector<std::thread> workers;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<sock_t> connections;
    std::vector<sock_t> active;
    std::deque<MultiCommand> commands;
    std::vector<DeviceInfo> radios;
    std::map<unsigned, std::string> publishers, udpOutputs;
    bool udpEnabled = true; std::string udpHost = "127.0.0.1"; int udpBase = 19000;
    std::string calibration = "{\"state\":\"none\"}", load = "{}", autoMonitor = "{\"enabled\":false,\"message\":\"Stopped\",\"cards\":[],\"candidates\":[]}";
    int selected = -1;
    double gain = 30;
    std::string message;
    std::string notice;
    bool noticeCentred = false;
    unsigned noticeSeq = 0;
    explicit Impl(MultiReceiver& r) : receiver(r) {}
    std::string state() {
        std::ostringstream out; out.precision(12);
        const auto config = receiver.config();
        const auto gainState = receiver.gainStatus();
        out << "{\"version\":\"" << SDR_TIOUNER_VERSION << "\",\"beta\":" << (SDR_TIOUNER_BETA ? "true" : "false") << ",\"running\":" << (receiver.running() ? "true" : "false")
            << ",\"sampleRate\":" << receiver.sampleRate() << ",\"centerMhz\":" << (config.centerIfHz + config.lnbHz - config.correctionHz) / 1e6 << ",\"alignTargets\":[" << receiver.alignTargetsMhz()[0] << ","
              << receiver.alignTargetsMhz()[1] << "," << receiver.alignTargetsMhz()[2] << "]"
            << ",\"maxChannels\":" << config.maxChannels << ",\"sourceDrops\":" << receiver.sourceDrops()
            << ",\"sourceError\":\"" << escape(receiver.sourceError()) << "\",\"channels\":[";
        auto channels = receiver.channels();
        std::map<unsigned, std::string> publisherCopy, udpCopy;
        { std::lock_guard<std::mutex> lock(mutex); publisherCopy = publishers; udpCopy = udpOutputs; }
        for (size_t i = 0; i < channels.size(); ++i) {
            const auto& ch = channels[i]; const auto& tel = ch.telemetry;
            std::string name, provider;
            for (const auto& service : ch.transport.services)
                if (service.havePmt && std::any_of(service.streams.begin(), service.streams.end(), [](const TsStream& stream) { return stream.kind == "video"; })) { name = service.name; provider = service.provider; break; }
            if (i) out << ',';
            out << "{\"id\":" << ch.id << ",\"revision\":" << ch.revision << ",\"frequency\":" << ch.config.satelliteMhz
                << ",\"symbolRate\":" << ch.config.symbolRate / 1000 << ",\"standardHint\":" << ch.config.standard
                << ",\"enabled\":" << (ch.config.enabled ? "true" : "false") << ",\"locked\":" << (receiver.running() && ch.config.enabled && tel.tsLock && ch.packetAgeSeconds >= 0 && ch.packetAgeSeconds < 3 ? "true" : "false")
                << ",\"cfoHz\":" << jsonNumber(tel.cfoHz) << ",\"snr\":" << jsonNumber(tel.snrDb) << ",\"mer\":" << jsonNumber(tel.merDb) << ",\"level\":" << jsonNumber(ch.levelDbfs)
                << ",\"actualSymbolRate\":" << jsonNumber(tel.symbolRate / 1000) << ",\"modulation\":\"" << escape(tel.modulationName)
                << "\",\"fec\":\"" << escape(tel.codeRate) << "\",\"standard\":" << tel.standard
                << ",\"service\":\"" << escape(name) << "\",\"provider\":\"" << escape(provider) << "\",\"bitrate\":" << jsonNumber(tel.netBitrate)
                << ",\"packets\":" << tel.packets << ",\"droppedSamples\":" << ch.droppedSamples
                << ",\"outputRate\":" << ch.outputRate << ",\"videoPort\":" << videoBase + ch.id - 1
                << ",\"error\":\"" << escape(ch.error) << "\",\"publisher\":\"" << escape(publisherCopy[ch.id]) << "\",\"udp\":\"" << escape(udpCopy[ch.id]) << "\"}";
        }
        SpectrumFrame spectrum; receiver.spectrum(spectrum);
        out << "],\"spectrumSeq\":" << spectrum.seq << ",\"spectrum\":[";
        // Peak-preserving grouping for a compact live display (1024 bins).
        for (size_t i = 0; i < spectrum.dbfs.size(); i += 4) {
            if (i) out << ',';
            float maximum = -140;
            for (size_t j = i; j < std::min(i + 4, spectrum.dbfs.size()); ++j) maximum = std::max(maximum, spectrum.dbfs[j]);
            out << jsonNumber(maximum);
        }
        out << "],\"batcSpectrum\":[";
        const auto batcFrame = batc.latest();
        for (size_t i = 0; i < batcFrame.size(); ++i) { if (i) out << ','; out << batcFrame[i]; }
        out << "],\"batcConnected\":" << (batc.isConnected() ? "true" : "false");
        { std::lock_guard<std::mutex> lock(mutex);
          out << ",\"autoMonitor\":" << autoMonitor;
          out << ",\"selectedRadio\":" << selected << ",\"gain\":" << gainState.gain << ",\"autoGain\":" << (gainState.automatic ? "true" : "false")
              << ",\"gainAvailable\":" << (gainState.available ? "true" : "false") << ",\"gainMin\":" << gainState.minimum
              << ",\"gainMax\":" << gainState.maximum << ",\"overloadEvents\":" << gainState.overloads
              << ",\"gainStatus\":\"" << escape(gainState.status) << "\",\"gainDetail\":\"" << escape(gainState.detail) << "\",\"noticeSeq\":" << noticeSeq << ",\"noticeCentred\":" << (noticeCentred ? "true" : "false") << ",\"notice\":\"" << escape(notice) << "\",\"message\":\"" << escape(message)
              << "\",\"udpEnabled\":" << (udpEnabled ? "true" : "false") << ",\"udpHost\":\"" << escape(udpHost) << "\",\"udpBase\":" << udpBase << ",\"correctionHz\":" << config.correctionHz << ",\"calibration\":" << calibration << ",\"load\":" << load << ",\"radios\":[";
          for (size_t i = 0; i < radios.size(); ++i) out << (i ? "," : "") << "{\"id\":" << i << ",\"name\":\"" << escape(radios[i].name) << "\"}";
          out << "]}"; }
        return out.str();
    }
    void handle(sock_t socket) {
        timeout(socket);
        std::string request; char buffer[2048];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            auto count = sockRecv(socket, buffer, sizeof buffer); if (count <= 0) return; request.append(buffer, count);
        }
        auto reply = [&](int code, const char* type, const std::string& body) {
            std::string header = "HTTP/1.1 " + std::to_string(code) + " Result\r\nContent-Type: " + type + "\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: " + std::to_string(body.size()) + "\r\n\r\n";
            if (sendAll(socket, header.data(), header.size())) sendAll(socket, body.data(), body.size());
        };
        if (request.find("\r\n\r\n") == std::string::npos) { reply(400, "text/plain", "Invalid request"); return; }
        std::istringstream lines(request); std::string method, target, version; lines >> method >> target >> version;
        std::map<std::string, std::string> headers; std::string line; std::getline(lines, line);
        while (std::getline(lines, line) && line != "\r") {
            auto colon = line.find(':'); if (colon == std::string::npos) continue;
            auto name = line.substr(0, colon); for (auto& c : name) c = (char)std::tolower((unsigned char)c);
            auto value = line.substr(colon + 1); auto first = value.find_first_not_of(" \t");
            value = first == std::string::npos ? "" : value.substr(first); if (!value.empty() && value.back() == '\r') value.pop_back();
            headers[name] = value;
        }
        sockaddr_in local{}, peer{};
#ifdef _WIN32
        int localSize=sizeof local, peerSize=sizeof peer;
#else
        socklen_t localSize=sizeof local, peerSize=sizeof peer;
#endif
        if(getsockname(socket,(sockaddr*)&local,&localSize)!=0||getpeername(socket,(sockaddr*)&peer,&peerSize)!=0) {reply(403,"text/plain","Unknown receiver address");return;}
        const auto ip=ntohl(peer.sin_addr.s_addr);
        const bool privatePeer=(ip>>24)==127||(ip>>24)==10||(ip>>20)==0xAC1||(ip>>16)==0xC0A8||(ip>>16)==0xA9FE;
        char localIp[INET_ADDRSTRLEN]{};inet_ntop(AF_INET,&local.sin_addr,localIp,sizeof localIp);
        const auto suffix=":"+std::to_string(port);
        const bool validHost=headers["host"]==std::string(localIp)+suffix||((ip>>24)==127&&headers["host"]=="localhost"+suffix);
        if (!privatePeer||!validHost) {
            reply(403, "text/plain", "Use this receiver's local or LAN IPv4 address"); return;
        }
        const auto question = target.find('?'); const auto path = target.substr(0, question);
        std::map<std::string, std::string> query;
        if (question != std::string::npos) {
            std::istringstream fields(target.substr(question + 1)); std::string field;
            while (std::getline(fields, field, '&')) { auto equal = field.find('='); if (equal != std::string::npos) query[field.substr(0, equal)] = field.substr(equal + 1); }
        }
        auto number = [&](const std::string& key) {
            auto text = query[key]; char* tail = nullptr; double value = strtod(text.c_str(), &tail);
            return !text.empty() && tail && !*tail && std::isfinite(value) ? value : std::numeric_limits<double>::quiet_NaN();
        };
        if (method == "POST") {
            if (headers["x-onair-control"] != token || headers["origin"] != "http://" + headers["host"]) { reply(403, "text/plain", "Open the receiver page to control reception"); return; }
            if (path.rfind("/api/playback/",0)==0) {
                const auto client=query["client"], page=query["page"];
                if(client.empty()||client.size()>64||!std::all_of(client.begin(),client.end(),[](unsigned char c){return std::isalnum(c)||c=='-';})) {reply(400,"text/plain","Invalid playback client");return;}
                if(path!="/api/playback/release"&&(page!="receiver"&&page!="monitor")) {reply(400,"text/plain","Invalid playback page");return;}
                std::lock_guard<std::mutex> lock(mutex);
                const auto now=std::chrono::steady_clock::now();
                if(now>=playbackUntil){playbackOwner.clear();playbackPage.clear();}
                if(path=="/api/playback/claim") {playbackOwner=client;playbackPage=page;playbackUntil=now+std::chrono::seconds(5);}
                else if(path=="/api/playback/heartbeat") {if(playbackOwner==client)playbackUntil=now+std::chrono::seconds(5);}
                else if(path=="/api/playback/release") {if(playbackOwner==client){playbackOwner.clear();playbackPage.clear();}}
                else {reply(404,"text/plain","Unknown playback command");return;}
                reply(200,"application/json",std::string("{\"active\":")+(playbackOwner==client?"true":"false")+",\"page\":\""+playbackPage+"\"}");return;
            }
            MultiCommand command{};
            if (path == "/api/radio/start") {
                double device = number("device");
                { std::lock_guard<std::mutex> lock(mutex);
                  if (!std::isfinite(device) || device != std::floor(device) || device < 0 || device >= (double)radios.size()) { reply(400, "text/plain", "Select a listed radio"); return; } }
                if (receiver.running()) { reply(409, "text/plain", "Stop capture before changing radios"); return; }
                double gain = number("gain");
                if (!std::isfinite(gain) || gain < 0 || gain > 100) { reply(400, "text/plain", "Gain must be from 0 to 100 dB"); return; }
                command.action = MultiCommand::RadioStart; command.device = (int)device; command.gain = gain;
            } else if (path == "/api/capture/align") {
                const auto mode = number("mode");
                if (mode != 0 && mode != 1 && mode != 2) { reply(400, "text/plain", "Align must be left, centre or right"); return; }
                command.action = MultiCommand::AlignCapture; command.align = (int)mode;
            } else if (path == "/api/gain") {
                const auto gain = number("gain"), automatic = number("automatic");
                if (!std::isfinite(gain) || gain < 0 || gain > 100 || (automatic != 0 && automatic != 1)) { reply(400,"text/plain","Invalid gain mode or gain"); return; }
                command.action = MultiCommand::GainSet; command.gain = gain; command.automaticGain = automatic == 1;
            } else if (path == "/api/radio/stop") command.action = MultiCommand::RadioStop;
            else if (path == "/api/radio/refresh") {
                if (receiver.running()) { reply(409, "text/plain", "Stop capture before refreshing radios"); return; }
                command.action = MultiCommand::RadioRefresh;
            } else if (path == "/api/channel" || path == "/api/channel/create") {
                double id = number("id"), frequency = number("frequency"), sr = number("symbolRate"), standard = number("standard"), enabled = number("enabled");
                if (!std::isfinite(frequency) || !std::isfinite(sr) || !std::isfinite(standard) || standard != std::floor(standard) || standard < 0 || standard > 2 || (enabled != 0 && enabled != 1) || frequency < 10490.5 || frequency > 10499.5 || sr < 25 || sr > 4000) { reply(400, "text/plain", "Invalid channel parameters"); return; }
                command.channel = {frequency, sr * 1000, (int)standard, enabled == 1};
                if (path == "/api/channel/create") command.action = MultiCommand::ChannelCreate;
                else {
                    if (!std::isfinite(id) || id < 1 || id > 100000 || id != std::floor(id)) { reply(400, "text/plain", "Invalid channel ID"); return; }
                    command.action = MultiCommand::ChannelSet; command.id = (unsigned)id;
                    // two running channels must not receive the same station
                    if (enabled == 1)
                        for (const auto& other : receiver.channels())
                            if (other.id != command.id && other.config.enabled && std::fabs(other.config.satelliteMhz - frequency) < 0.05) {
                                reply(409, "text/plain", "Channel " + std::to_string(other.id) + " is already on this station"); return;
                            }
                }
            } else if (path == "/api/udp") {
                // where the untouched TS goes: an IPv4 address (this PC, another PC, or a multicast group) and the first port
                const std::string host = query["host"]; const double port = number("port"), enabled = number("enabled");
                in_addr parsed{};
                if (host.empty() || host.size() > 15 || inet_pton(AF_INET, host.c_str(), &parsed) != 1 || parsed.s_addr == 0 || parsed.s_addr == 0xFFFFFFFFu) { reply(400, "text/plain", "Enter an IPv4 address such as 192.168.1.50"); return; }
                if (!std::isfinite(port) || port != std::floor(port) || port < 1024 || port > 65000 || (enabled != 0 && enabled != 1)) { reply(400, "text/plain", "Invalid port (1024-65000)"); return; }
                command.action = MultiCommand::UdpSet; command.udpHost = host; command.udpPort = (int)port; command.udpEnabled = enabled == 1;
            } else if (path == "/api/calibrate") {
                if (!receiver.running()) { reply(409, "text/plain", "Start receiving first: the calibration needs the beacon"); return; }
                command.action = MultiCommand::Calibrate;
            } else if (path == "/api/auto/start") command.action = MultiCommand::AutoStart;
            else if (path == "/api/auto/stop") command.action = MultiCommand::AutoStop;
            else if (path == "/api/shutdown") command.action = MultiCommand::Shutdown;
            else { reply(404, "text/plain", "Unknown command"); return; }
            { std::lock_guard<std::mutex> lock(mutex);
              if (commands.size() >= 16) { reply(409, "text/plain", "Receiver commands are busy"); return; }
              commands.push_back(command); }
            reply(202, "application/json", "{\"accepted\":true}"); return;
        }
        if (method != "GET") { reply(405, "text/plain", "GET or POST required"); return; }
        if(path=="/assets/playback.js") {reply(200,"application/javascript",playbackScript);return;}
        if (path == "/monitor") {
            std::string page = autoMonitorPage; auto offset = page.find("__CONTROL_TOKEN__"); page.replace(offset,17,token);
            reply(200,"text/html; charset=utf-8",page); return;
        }
        if (path == "/" || path == "/receiver") {
            std::string page = multiWebPage; auto offset = page.find("__CONTROL_TOKEN__"); page.replace(offset, 17, token);
            reply(200, "text/html; charset=utf-8", page); return;
        }
        if (path == "/api/state") { reply(200, "application/json", state()); return; }
        if (path == "/assets/bootstrap.css") { reply(200, "text/css; charset=utf-8", multiBootstrapCss); return; }
        if (path.rfind("/stream/", 0) == 0) {
            char* tail = nullptr; auto id = strtoul(path.c_str() + 8, &tail, 10);
            if (!tail || *tail || id < 1 || id > 100000) { reply(400, "text/plain", "Invalid channel"); return; }
            auto stream = receiver.subscribe((unsigned)id);
            if (!stream) { reply(409, "text/plain", "Channel stopped or subscriber limit reached"); return; }
            int one = 1; setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
            std::string header = "HTTP/1.0 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
            if (!sendAll(socket, header.data(), header.size())) return;
            while (running) {
                std::vector<uint8_t> packets;
                { std::unique_lock<std::mutex> lock(stream->mutex);
                  stream->cv.wait_for(lock, std::chrono::milliseconds(200), [&] { return stream->closed || !stream->queue.empty() || !running; });
                  if (stream->closed) break;
                  if (!stream->queue.empty()) { packets = std::move(stream->queue.front()); stream->queue.pop_front(); stream->bytes -= packets.size(); } }
                if (!packets.empty() && !sendAll(socket, packets.data(), packets.size())) break;
                if (packets.empty() && sockPeekClosed(socket) == 0) break;
            }
            { std::lock_guard<std::mutex> lock(stream->mutex); stream->closed = true; }
            return;
        }
        reply(404, "text/plain", "Not found");
    }
};
MultiWeb::MultiWeb(MultiReceiver& receiver) : p_(std::make_unique<Impl>(receiver)) {}
MultiWeb::~MultiWeb() { stop(); }
bool MultiWeb::start(int port, int videoBase, std::string& error) {
    stop(); netInit(); p_->port = port; p_->videoBase = videoBase;
    std::random_device random; p_->token.clear();
    for (int i = 0; i < 4; ++i) { char text[9]; snprintf(text, sizeof text, "%08x", (unsigned)random()); p_->token += text; }
    p_->listener = socket(AF_INET, SOCK_STREAM, 0); sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_addr.s_addr = htonl(INADDR_ANY); address.sin_port = htons(port);
    if (p_->listener == kBadSock || bind(p_->listener, (sockaddr*)&address, sizeof address) != 0 || listen(p_->listener, 16) != 0) {
        error = "Cannot open local web port " + std::to_string(port); if (p_->listener != kBadSock) sockClose(p_->listener); p_->listener = kBadSock; return false;
    }
    p_->running = true;
    p_->batc.start();
    const size_t workerCount = std::max<size_t>(8, p_->receiver.config().maxChannels + 4);
    for (size_t i = 0; i < workerCount; ++i) p_->workers.emplace_back([this] {
        while (p_->running) {
            sock_t socket;
            { std::unique_lock<std::mutex> lock(p_->mutex); p_->cv.wait(lock, [&] { return !p_->running || !p_->connections.empty(); });
              if (!p_->running) break;
              socket = p_->connections.front(); p_->connections.pop_front(); p_->active.push_back(socket); }
            try { p_->handle(socket); } catch (...) {} // a failed request cannot terminate DSP
            { std::lock_guard<std::mutex> lock(p_->mutex); p_->active.erase(std::remove(p_->active.begin(), p_->active.end(), socket), p_->active.end()); sockClose(socket); }
        }
    });
    p_->acceptor = std::thread([this] {
        while (p_->running) {
            auto socket = accept(p_->listener, nullptr, nullptr); if (socket == kBadSock) break;
            std::lock_guard<std::mutex> lock(p_->mutex);
            if (!p_->running || p_->connections.size() >= 24) sockClose(socket);
            else { p_->connections.push_back(socket); p_->cv.notify_one(); }
        }
    }); return true;
}
void MultiWeb::stop() {
    p_->running = false;
    if (p_->listener != kBadSock) { sockShutdown(p_->listener); sockClose(p_->listener); }
    if (p_->acceptor.joinable()) p_->acceptor.join(); p_->listener = kBadSock;
    { std::lock_guard<std::mutex> lock(p_->mutex); for (auto socket : p_->active) sockShutdown(socket);
      for (auto socket : p_->connections) sockClose(socket); p_->connections.clear(); }
    p_->cv.notify_all(); for (auto& worker : p_->workers) if (worker.joinable()) worker.join(); p_->workers.clear();
    p_->batc.stop();
}
bool MultiWeb::takeCommand(MultiCommand& command) {
    std::lock_guard<std::mutex> lock(p_->mutex); if (p_->commands.empty()) return false;
    command = p_->commands.front(); p_->commands.pop_front(); return true;
}
void MultiWeb::setRadios(const std::vector<DeviceInfo>& radios, int selected, double gain) { std::lock_guard<std::mutex> lock(p_->mutex); p_->radios = radios; p_->selected = selected; p_->gain = gain; }
void MultiWeb::setMessage(const std::string& message) { std::lock_guard<std::mutex> lock(p_->mutex); p_->message = message; }
void MultiWeb::setNotice(const std::string& text, bool centred) { std::lock_guard<std::mutex> lock(p_->mutex); p_->notice = text; p_->noticeCentred = centred; ++p_->noticeSeq; }
void MultiWeb::setPublisher(unsigned id, const std::string& status) { std::lock_guard<std::mutex> lock(p_->mutex); p_->publishers[id] = status; }
void MultiWeb::setUdpOutput(unsigned id, const std::string& address) { std::lock_guard<std::mutex> lock(p_->mutex); p_->udpOutputs[id] = address; }
void MultiWeb::setLoad(const std::string& json) { std::lock_guard<std::mutex> lock(p_->mutex); p_->load = json; }
void MultiWeb::setCalibration(const std::string& json) { std::lock_guard<std::mutex> lock(p_->mutex); p_->calibration = json; }
void MultiWeb::setUdpSettings(bool enabled, const std::string& host, int basePort) { std::lock_guard<std::mutex> lock(p_->mutex); p_->udpEnabled = enabled; p_->udpHost = host; p_->udpBase = basePort; }
std::vector<uint16_t> MultiWeb::batcFrame() const { return p_->batc.latest(); }
void MultiWeb::setAutoMonitor(const std::string& json) { std::lock_guard<std::mutex> lock(p_->mutex); p_->autoMonitor=json; }
