#pragma once
#include "dect2/multi_receiver.h"
#include <memory>
#include <string>

struct MultiCommand {
    enum Action { RadioStart, RadioStop, RadioRefresh, ChannelCreate, ChannelSet, GainSet, UdpSet, Calibrate, AlignCapture, Shutdown, AutoStart, AutoStop, RecordSet, RecordOpen } action;
    int device = -1;
    unsigned id = 0;
    double gain = 30;
    bool automaticGain = true;
    dect2::ChannelConfig channel;
    int align = 1;                   // AlignCapture: 0 left, 1 centre, 2 right
    bool udpEnabled = true;          // UdpSet: where the untouched TS of every running channel goes
    std::string udpHost = "127.0.0.1";
    int udpPort = 19000;
    bool record = false;             // RecordSet: record the transport stream of every running channel to files
};
class MultiWeb {
public:
    explicit MultiWeb(dect2::MultiReceiver& receiver);
    ~MultiWeb();
    bool start(int port, int videoBase, std::string& error);
    void stop();
    bool takeCommand(MultiCommand& command);
    void setRadios(const std::vector<dect2::DeviceInfo>& radios, int selected, double gain = 30);
    void setMessage(const std::string& message);
    // A notice the page shows as a popup (a toast) once: something the user should know and that the status line is too small for
    // centred: a popup in the middle of the page that closes by itself when the capture is running (for "please wait"), else a toast
    void setNotice(const std::string& text, bool centred = false);
    void setPublisher(unsigned id, const std::string& status);
    void setUdpOutput(unsigned id, const std::string& address);
    void setUdpSettings(bool enabled, const std::string& host, int basePort);
    // The calibration status of the frequency correction as a JSON object, passed through to the page ("calibration" in the state)
    void setCalibration(const std::string& json);
    // The load of this program on the PC (see multi_load.h) as a JSON object, passed through to the page ("load" in the state)
    void setLoad(const std::string& json);   // shown on the page, which edits them with /api/udp   // "" = no UDP stream for the channel right now
    std::vector<uint16_t> batcFrame() const;
    void setAutoMonitor(const std::string& json);
    // The recording as a JSON object, passed through to the page ("recording" in the state)
    void setRecording(const std::string& json);
    // Whether a newer release is out, as a JSON object, passed through to the page (the update entry of the state)
    void setUpdate(const std::string& json);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
