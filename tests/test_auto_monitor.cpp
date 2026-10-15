#include "../app/auto_monitor.h"
#include <cstdio>
#define CHECK(c) do { if(!(c)){fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#c);return 1;} }while(0)
static std::vector<uint16_t> frame() {
    std::vector<uint16_t> bins(9001,10000);
    for(int i=850;i<=1150;++i)bins[i]=23000; // beacon is recognised as 1500 kS/s
    for(int i=5750;i<=6250;++i)bins[i]=22000; // 10496.5 MHz, about 500 kS/s, 92% of the beacon's plateau
    return bins;
}
static size_t count(const std::string& text,const std::string& part) {
    size_t n=0;for(size_t at=text.find(part);at!=std::string::npos;at=text.find(part,at+1))++n;return n;
}
static size_t enabledChannels(dect2::MultiReceiver& rx) {
    size_t n=0;for(const auto& ch:rx.channels())if(ch.config.enabled)++n;return n;
}
int main() {
    CHECK(detectAutoCarriers({}).empty());
    CHECK(detectAutoCarriers(std::vector<uint16_t>(9001,10000)).empty());
    auto carriers=detectAutoCarriers(frame());CHECK(carriers.size()==2);
    CHECK(carriers[0].mhz==10491.5&&carriers[0].rate==1500000);
    CHECK(std::abs(carriers[1].mhz-10496.5)<.003&&carriers[1].rate==500000);

    // The real BATC frame (922 bins) has a floor that tilts upward by about 4000 counts across the band: plain noise must
    // not become stations, and a real narrow carrier on the tilted floor must still be found.
    {
        std::vector<uint16_t> tilted(922);
        for(size_t i=0;i<tilted.size();++i)tilted[i]=(uint16_t)(9700+4000.0*i/921+((i*37)%11)*60-300);
        CHECK(detectAutoCarriers(tilted).empty());
        auto withSignal=tilted;for(int i=714;i<=760;++i)withSignal[i]=(uint16_t)(withSignal[i]+15000);   // about 10497.7 MHz, ~330 kS/s
        auto found=detectAutoCarriers(withSignal);
        CHECK(found.size()==1&&std::abs(found[0].mhz-10497.7)<.05&&found[0].rate>=250000&&found[0].rate<=500000);
    }
    // A station on the high side of the tilted floor that looks about 84% as tall as the beacon on the chart must
    // reach 80% of the beacon: strength is judged on the same footing as the chart, not above the local floor.
    {
        std::vector<uint16_t> band(922);
        for(size_t i=0;i<band.size();++i)band[i]=(uint16_t)(9700+4000.0*i/921);
        for(int i=25;i<=179;++i)band[i]=39000;      // the beacon, 10491.5 MHz, 1.5 MS/s
        for(int i=826;i<=872;++i)band[i]=34000;     // about 10498.8 MHz, ~330 kS/s
        double beacon=0,station=0;
        for(auto c:detectAutoCarriers(band)){if(c.mhz==10491.5)beacon=c.strength;else if(std::abs(c.mhz-10498.8)<.1)station=c.strength;}
        CHECK(beacon>0&&station>0&&station>=.8*beacon&&station<beacon);
    }
    dect2::MultiConfig cfg;cfg.centerIfHz=745e6;cfg.sampleRate=10e6;cfg.maxChannels=6;
    dect2::MultiReceiver rx(cfg);CHECK(rx.create({10493,333000,0,false}));
    AutoMonitor monitor;CHECK(!monitor.start(rx));   // no radio capture, nothing to monitor
    std::string error;CHECK(rx.startDemo(error));
    CHECK(rx.configure(1,{10493,333000,0,true},error)); // a channel the receiver page had running
    CHECK(monitor.start(rx));
    // Take-over: one card, the beacon, on the first decoder; the receiver page's channel is released.
    CHECK(count(monitor.json(),"\"uid\"")==1&&count(monitor.json(),"\"beacon\":true")==1);
    {auto channels=rx.channels();CHECK(channels[0].config.enabled&&channels[0].config.satelliteMhz==10491.5&&channels[0].config.symbolRate==1500000);}
    CHECK(enabledChannels(rx)==1);

    // A new station above the beacon gets a card only once it has been seen three times.
    auto strong=frame();for(int i=5750;i<=6250;++i)strong[i]=40000;
    monitor.tick(rx,strong,98);CHECK(count(monitor.json(),"\"uid\"")==1);
    monitor.tick(rx,strong,99);CHECK(count(monitor.json(),"\"uid\"")==1);
    monitor.tick(rx,strong,100);
    CHECK(count(monitor.json(),"\"uid\"")==2&&enabledChannels(rx)==2);
    {bool found=false;for(const auto& ch:rx.channels())if(ch.config.enabled&&std::abs(ch.config.satelliteMhz-10496.5)<.003&&ch.config.symbolRate==500000)found=true;CHECK(found);}
    auto revisions=rx.channels();
    monitor.tick(rx,{},110);                       // stale source: cards held, nothing retuned
    monitor.tick(rx,frame(),111);                  // the same station again: no retune, no duplicate card
    CHECK(count(monitor.json(),"\"uid\"")==2);
    for(size_t i=0;i<revisions.size();++i)CHECK(rx.channels()[i].revision==revisions[i].revision);

    // A second new station adds a third card, on a decoder created on demand beyond the initial pool.
    auto two=frame();for(int i=7050;i<=7250;++i)two[i]=21000;   // about 10498.3 MHz
    monitor.tick(rx,two,112);monitor.tick(rx,two,113);monitor.tick(rx,two,114);
    CHECK(count(monitor.json(),"\"uid\"")==3&&enabledChannels(rx)==3);

    // A carrier below 60% of the beacon's plateau is not even tried (the 46% one here), one at 78% is.
    auto weak=frame();for(int i=4000;i<=4200;++i)weak[i]=16000;   // about 10494.5 MHz, 46% of the beacon
    for(int t=115;t<118;++t)monitor.tick(rx,weak,t);
    CHECK(count(monitor.json(),"\"uid\"")==3);
    {bool listed=false;for(const auto& c:monitor.candidates)if(std::abs(c.mhz-10494.5)<.05)listed=true;CHECK(!listed);}
    // Anything left of or at the beacon, or too weak (a roll-off hump at the frame edge), is not scanned.
    auto below=std::vector<uint16_t>(9001,10000);for(int i=850;i<=1150;++i)below[i]=23000;for(int i=100;i<=300;++i)below[i]=21000;for(int i=8750;i<=8850;++i)below[i]=13000;   // 10499.3 MHz: a weak hump from the roll-off at the edge of the BATC frame
    for(int t=118;t<121;++t)monitor.tick(rx,below,t);
    CHECK(count(monitor.json(),"\"uid\"")==3);
    for(int i=0;i<(int)monitor.candidates.size();++i)CHECK(monitor.candidates[i].mhz>=10492.0);

    // A signal that disappears: the card stays, marked lost, and its decoder is released.
    const auto before=enabledChannels(rx);
    monitor.tick(rx,std::vector<uint16_t>(9001,10000),140);
    CHECK(count(monitor.json(),"\"lost\":true")==2&&enabledChannels(rx)==before-2);
    CHECK(count(monitor.json(),"\"uid\"")==3);     // beacon never lost; cards remain
    // The lost cards are shown for a few seconds, then removed; the carrier is not retried straight away.
    monitor.tick(rx,frame(),141);monitor.tick(rx,frame(),142);monitor.tick(rx,frame(),143);
    CHECK(count(monitor.json(),"\"lost\":true")==2&&count(monitor.json(),"\"uid\"")==3);
    monitor.tick(rx,frame(),146);
    CHECK(count(monitor.json(),"\"lost\":true")==0&&count(monitor.json(),"\"uid\"")==1);   // only the beacon remains
    monitor.tick(rx,frame(),148);
    CHECK(count(monitor.json(),"\"uid\"")==1);   // still in the retry pause
    monitor.tick(rx,frame(),160);
    CHECK(count(monitor.json(),"\"uid\"")==2&&count(monitor.json(),"\"lost\":true")==0);   // back after the pause

    // Stopping leaves the channels where they are.
    auto last=rx.channels();monitor.stop(rx);
    CHECK(!monitor.enabled);
    for(size_t i=0;i<last.size();++i)CHECK(rx.channels()[i].revision==last[i].revision);
    rx.stop();

    // A real station reaching the high end of the band (10499.0-10499.4 MHz) is found like any other.
    {
        dect2::MultiReceiver wide(cfg);CHECK(wide.create({10493,333000,0,false}));
        CHECK(wide.startDemo(error));AutoMonitor edgeMonitor;CHECK(edgeMonitor.start(wide));
        auto edge=frame();for(int i=5750;i<=6250;++i)edge[i]=10000;for(int i=8500;i<=8920;++i)edge[i]=22000;   // 10499.0-10499.42 MHz: reaches the end of the frame, measures ~500 kS/s
        for(int t=1;t<4;++t)edgeMonitor.tick(wide,edge,t);
        CHECK(count(edgeMonitor.json(),"\"uid\"")==2);
        bool atEdge=false;for(const auto& c:wide.channels())if(c.config.enabled&&std::abs(c.config.satelliteMhz-10499.15)<.1)atEdge=true;
        CHECK(atEdge);
        edgeMonitor.stop(wide);wide.stop();
    }
    // A narrow capture must never chase candidates outside its window.
    dect2::MultiConfig narrowCfg; narrowCfg.sampleRate=2e6;narrowCfg.centerIfHz=741.5e6;
    dect2::MultiReceiver narrow(narrowCfg);CHECK(narrow.create({10491.5,500000,0,false}));
    CHECK(narrow.startDemo(error));AutoMonitor limited;CHECK(limited.start(narrow));
    limited.tick(narrow,frame(),1);limited.tick(narrow,frame(),2);limited.tick(narrow,frame(),3);
    CHECK(count(limited.json(),"\"uid\"")==1);
    CHECK(narrow.config().centerIfHz==narrowCfg.centerIfHz);
    // The radio does not receive the beacon (nothing locks): after a while the setup is blamed, not the stations.
    CHECK(count(limited.json(),"\"beaconMissing\":false")==1);
    {
        const double real=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        limited.tick(narrow,frame(),real+30);
        CHECK(count(limited.json(),"\"beaconMissing\":true")==1);
    }
    limited.stop(narrow);narrow.stop();
    puts("PASS: carrier detection, beacon card, scan above beacon, on-demand cards, lost cards, cooldown, stopping without retuning");
}
