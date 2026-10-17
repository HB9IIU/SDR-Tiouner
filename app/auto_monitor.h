#pragma once
#include "dect2/multi_receiver.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <sstream>

// Called only by the receiver control loop. No browser connection owns this state.
struct AutoCandidate { double mhz, rate, strength; };
inline std::vector<AutoCandidate> detectAutoCarriers(const std::vector<uint16_t>& fft) {
    if (fft.size() < 32) return {};
    std::vector<double> smooth(fft.size());
    for (size_t i=0;i<fft.size();++i) {
        double sum=0; int count=0;
        for (int k=-3;k<=3;++k) { auto j=(int)i+k; if(j>=0&&j<(int)fft.size()) {sum+=fft[j];++count;} }
        smooth[i]=sum/count;
    }
    // The noise floor is a smooth curve fitted to the quiet bins: the real band floor tilts by several thousand counts
    // across 9 MHz, so one fixed threshold would see stations in plain noise on the high side. Signals and the cliffs at
    // the band edges are rejected from the fit and iterated away.
    const size_t count=smooth.size();
    const double step=9.0/(count-1);
    std::vector<double> floorAt(count);
    {
        auto sorted=smooth; std::sort(sorted.begin(),sorted.end());
        double c[3]={sorted[count/2],0,0};   // quadratic in x=0..1; start flat at the median
        std::vector<char> keep(count,0);
        for(size_t i=0;i<count;++i)keep[i]=smooth[i]<=c[0]?1:0;
        for(int pass=0;pass<5;++pass) {
            double m[3][4]={};   // normal equations
            size_t used=0;
            for(size_t i=0;i<count;++i) {
                if(!keep[i])continue;
                ++used;
                const double x=(double)i/(count-1),p[3]={1,x,x*x};
                for(int r=0;r<3;++r){for(int k=0;k<3;++k)m[r][k]+=p[r]*p[k];m[r][3]+=p[r]*smooth[i];}
            }
            if(used<16)break;
            // Gauss elimination with partial pivoting
            bool ok=true;
            for(int col=0;col<3&&ok;++col) {
                int piv=col;for(int r=col+1;r<3;++r)if(std::abs(m[r][col])>std::abs(m[piv][col]))piv=r;
                if(std::abs(m[piv][col])<1e-12){ok=false;break;}
                for(int k=0;k<4;++k)std::swap(m[col][k],m[piv][k]);
                for(int r=0;r<3;++r){if(r==col)continue;const double f=m[r][col]/m[col][col];for(int k=col;k<4;++k)m[r][k]-=f*m[col][k];}
            }
            if(!ok)break;
            for(int r=0;r<3;++r)c[r]=m[r][3]/m[r][r];
            for(size_t i=0;i<count;++i) {
                const double x=(double)i/(count-1),fit=c[0]+c[1]*x+c[2]*x*x,residual=smooth[i]-fit;
                keep[i]=residual<1000&&residual>-1500?1:0;   // neither a signal nor an edge cliff
            }
        }
        for(size_t i=0;i<count;++i){const double x=(double)i/(count-1);floorAt[i]=c[0]+c[1]*x+c[2]*x*x;}
    }
    // Strength is measured above one common floor, so a station on the high side of the tilted band is not made to look
    // weaker than the plateau drawn on the chart. (Detection itself still uses the local floor.)
    double commonFloor;
    {
        auto levels=floorAt;auto nth=levels.begin()+levels.size()/10;
        std::nth_element(levels.begin(),nth,levels.end());commonFloor=*nth;
    }
    const double rates[]={25000,33000,50000,66000,125000,250000,333000,500000,1000000,1500000,2000000};
    std::vector<AutoCandidate> result;
    for(size_t i=0;i<smooth.size();) {
        if(smooth[i]<floorAt[i]+2000) {++i;continue;}
        size_t begin=i,peak=i;
        while(i<smooth.size()&&smooth[i]>=floorAt[i]+2000) {if(smooth[i]>smooth[peak])peak=i;++i;}
        if(begin==0||i==smooth.size()||i-begin<3) continue;
        const double noise=floorAt[peak], half=noise+(smooth[peak]-noise)*.5; size_t l=peak,r=peak;
        while(l>begin&&smooth[l-1]>half)--l;
        while(r+1<i&&smooth[r+1]>half)++r;
        double mhz=10490.5+(l+r)*.5*step, measured=(r-l+1)*step*1e6;
        if(measured<18000||measured>2800000)continue;
        double rate=rates[0]; for(double v:rates)if(std::abs(std::log(v/measured))<std::abs(std::log(rate/measured)))rate=v;
        if(std::abs(mhz-10491.5)<.3) {mhz=10491.5;rate=1500000;}
        result.push_back({mhz,rate,smooth[peak]-commonFloor});
    }
    std::sort(result.begin(),result.end(),[](auto a,auto b){return a.strength>b.strength;});
    return result;
}
// The rules for a station worth decoding, shared by the Auto Monitor and by the start of the receiver page: from 10492 MHz (above the
// beacon), the centre inside the BATC frame (its last bins are a roll-off), a plateau of at least 60% of the beacon's.
inline constexpr double kBeaconMhz = 10491.5, kBeaconRate = 1500000, kScanFromMhz = 10492.0, kScanToMhz = 10499.4, kMinBeaconRatio = .6;

// The start of the receiver page: the strongest stations above the beacon in one BATC frame (at most `count`), in order of frequency,
// and only those that fit in the radio's window (centre `centreMhz`, `sampleRate`). Nothing without the beacon in the frame as the reference.
inline std::vector<AutoCandidate> pickStartStations(const std::vector<uint16_t>& fft, size_t count, double centreMhz, double sampleRate) {
    const auto found = detectAutoCarriers(fft);
    double beacon = 0;
    for (const auto& c : found) if (std::abs(c.mhz - kBeaconMhz) < .01 && c.rate == kBeaconRate) beacon = c.strength;
    std::vector<AutoCandidate> stations;
    if (beacon <= 0) return stations;
    for (const auto& c : found)
        if (c.mhz >= kScanFromMhz && c.mhz <= kScanToMhz && c.strength >= kMinBeaconRatio * beacon &&
            std::abs(c.mhz - centreMhz) * 1e6 + c.rate * 1.35 / 2 <= sampleRate * .49) stations.push_back(c);
    std::sort(stations.begin(), stations.end(), [](const AutoCandidate& a, const AutoCandidate& b) { return a.strength > b.strength; });
    if (stations.size() > count) stations.resize(count);
    std::sort(stations.begin(), stations.end(), [](const AutoCandidate& a, const AutoCandidate& b) { return a.mhz < b.mhz; });
    return stations;
}

// The Auto Monitor operator: the first card is the beacon; the band above it is scanned continuously and every
// new station gets its own card (and decoder). The decoder pool is only limited by the receiver's max_channels.
struct MonitorCard { unsigned uid=0, channel=0; AutoCandidate carrier{}; bool beacon=false, lost=false; double since=0, lastSeen=0, lastLock=0, lostAt=0; };
class AutoMonitor {
    static constexpr double kLostShownSeconds=5, kRetryPauseSeconds=10;   // a lost card stays briefly, then goes; its carrier waits before a retry
    std::vector<MonitorCard> cards;
    std::vector<std::pair<double,int>> seen;
    std::vector<std::pair<double,double>> cooldown;   // (MHz, until) for carriers whose card was removed
    unsigned nextUid=1;
    double nextTick=0;
    std::string message="Stopped";
    bool beaconMissing=false;   // the radio has not locked the beacon for a while: LNB, cable or bias
    static bool near(double a,double b,double width) { return std::abs(a-b)<width; }
    static bool locked(const dect2::ChannelStatus& ch) { return ch.telemetry.tsLock&&ch.packetAgeSeconds>=0&&ch.packetAgeSeconds<3; }
    // A decoder that no card is using: an idle pool channel, or a new one while the pool has room.
    unsigned freeChannel(dect2::MultiReceiver& receiver) {
        for(const auto& ch:receiver.channels()) {
            bool used=false;for(const auto& c:cards)if(!c.lost&&c.channel==ch.id)used=true;
            if(!used)return ch.id;
        }
        return receiver.create({kBeaconMhz,kBeaconRate,0,false});
    }
public:
    bool enabled=false;
    std::vector<dect2::ChannelStatus> saved;   // the channels as they were before the monitor took over (what settings keep)
    std::vector<AutoCandidate> candidates;
    void stop(dect2::MultiReceiver&) {
        if(!enabled)return;
        enabled=false;beaconMissing=false;cards.clear();seen.clear();cooldown.clear();saved.clear();message="Automatic scanning stopped; channels left in place";
    }
    bool start(dect2::MultiReceiver& receiver) {
        if(enabled)return true;
        if(!receiver.running()) {message="No radio capture: connect a radio";return false;}
        saved=receiver.channels();
        // The monitor owns the radio: whatever the receiver page was decoding is released.
        for(auto ch:saved) if(ch.config.enabled) {ch.config.enabled=false;std::string error;receiver.configure(ch.id,ch.config,error);}
        const unsigned id=freeChannel(receiver);
        if(!id) {message="No decoder available";return false;}
        std::string error;
        // The beacon may be outside a narrow capture; its card is still shown.
        receiver.configure(id,{kBeaconMhz,kBeaconRate,0,true},error);
        beaconMissing=false;cards.clear();seen.clear();cooldown.clear();candidates.clear();
        const double now=std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        MonitorCard beacon;beacon.uid=nextUid++;beacon.channel=id;beacon.carrier={kBeaconMhz,kBeaconRate,0};beacon.beacon=true;beacon.since=beacon.lastSeen=beacon.lastLock=now;
        cards.push_back(beacon);
        enabled=true;nextTick=0;message="Scanning for stations above the beacon";
        return true;
    }
    void tick(dect2::MultiReceiver& receiver,const std::vector<uint16_t>& fft,double now) {
        if(!enabled||now<nextTick)return;
        nextTick=now+1;
        if(!receiver.running()) {stop(receiver);return;}
        candidates.clear();
        // A station is only worth decoding if its plateau reaches 60% of the beacon's (real stations measure 75-100%, noise bumps
        // about 20%): the beacon is the reference.
        const auto found=detectAutoCarriers(fft);
        double beaconStrength=0;
        for(auto c:found)if(near(c.mhz,kBeaconMhz,.01)&&c.rate==kBeaconRate)beaconStrength=c.strength;
        if(beaconStrength>0)
            for(auto c:found)
                if(c.mhz>=kScanFromMhz&&c.mhz<=kScanToMhz&&c.strength>=kMinBeaconRatio*beaconStrength)candidates.push_back(c);
        // A missing BATC feed never causes a working decoder to be retuned.
        if(fft.empty()) {message="BATC unavailable; holding current cards";return;}
        std::vector<std::pair<double,int>> confirmed;
        for(auto c:candidates) {
            int count=1;
            for(auto previous:seen)if(near(c.mhz,previous.first,.025))count=std::min(3,previous.second+1);
            confirmed.push_back({c.mhz,count});
        }
        seen=confirmed;
        const auto channels=receiver.channels();
        auto statusOf=[&](unsigned id)->const dect2::ChannelStatus* {for(const auto& ch:channels)if(ch.id==id)return &ch;return nullptr;};
        // No beacon received AND nothing else locked for a while: the setup (LNB connection, bias) is wrong. If another
        // station decodes, the radio chain works and a slow beacon is not a setup problem.
        {
            bool anyLocked=false;
            for(auto& card:cards) {
                const auto* ch=statusOf(card.channel);
                if(!ch||!locked(*ch))continue;
                anyLocked=true;
                if(card.beacon)card.lastLock=now;
            }
            beaconMissing=false;
            for(const auto& card:cards)
                if(card.beacon&&!anyLocked&&now-std::max(card.since,card.lastLock)>20)beaconMissing=true;
        }
        // Cards whose signal is gone: the card stays, its decoder is released.
        for(auto& card:cards) {
            if(card.beacon||card.lost)continue;
            const auto* ch=statusOf(card.channel);
            if(!ch) {card.lost=true;card.lostAt=now;card.channel=0;continue;}
            for(auto c:candidates)if(near(c.mhz,card.carrier.mhz,std::max(.025,card.carrier.rate/2e6)))card.lastSeen=now;
            if(locked(*ch))card.lastLock=now;
            if(!locked(*ch)&&(now-card.lastSeen>12||now-std::max(card.since,card.lastLock)>30)) {
                auto config=ch->config;config.enabled=false;std::string error;receiver.configure(ch->id,config,error);
                card.lost=true;card.lostAt=now;card.channel=0;
            }
        }
        // A lost card is shown for a few seconds and then removed; its carrier is not retried straight away.
        for(auto it=cards.begin();it!=cards.end();) {
            if(it->lost&&now-it->lostAt>=kLostShownSeconds) {cooldown.push_back({it->carrier.mhz,it->lostAt+kRetryPauseSeconds});it=cards.erase(it);}
            else ++it;
        }
        cooldown.erase(std::remove_if(cooldown.begin(),cooldown.end(),[&](auto v){return v.second<=now;}),cooldown.end());
        const auto cfg=receiver.config(); const double centre=(cfg.centerIfHz+cfg.lnbHz-cfg.correctionHz)/1e6;
        for(auto c:candidates) {
            bool stable=false;for(auto v:seen)if(near(c.mhz,v.first,.025)&&v.second>=3)stable=true;
            if(!stable)continue;
            if(std::abs(c.mhz-centre)*1e6+c.rate*1.35/2>receiver.sampleRate()*.49)continue;
            bool occupied=false;
            for(const auto& card:cards)
                if(near(c.mhz,card.carrier.mhz,std::max(.04,(c.rate+card.carrier.rate)*.35/1e6)))occupied=true;
            for(const auto& v:cooldown)if(near(c.mhz,v.first,.04))occupied=true;
            if(occupied)continue;
            const unsigned id=freeChannel(receiver);
            if(!id) {message="No decoder available for another station (raise max_channels)";continue;}
            std::string error;
            if(!receiver.configure(id,{c.mhz,c.rate,0,true},error))continue;
            MonitorCard card;card.uid=nextUid++;card.channel=id;card.carrier=c;card.since=card.lastSeen=now;cards.push_back(card);
        }
        size_t active=0;for(const auto& card:cards)if(!card.lost)++active;
        if(beaconMissing) message="No beacon received. Is the radio connected to the LNB, and is the LNB powered (about 18 V bias)?";
        else if(beaconStrength<=0) message="The BATC spectrum shows no beacon, so stations cannot be judged yet";
        else message="Scanning above the beacon; "+std::to_string(active)+" card(s) active";
    }
    std::string json() const {
        std::ostringstream out;out.precision(12);
        out<<"{\"enabled\":"<<(enabled?"true":"false")<<",\"beaconMissing\":"<<(beaconMissing?"true":"false")<<",\"message\":\""<<message<<"\",\"cards\":[";
        for(size_t i=0;i<cards.size();++i) {
            const auto& c=cards[i];if(i)out<<',';
            out<<"{\"uid\":"<<c.uid<<",\"channel\":"<<c.channel<<",\"beacon\":"<<(c.beacon?"true":"false")<<",\"lost\":"<<(c.lost?"true":"false")
               <<",\"frequency\":"<<c.carrier.mhz<<",\"symbolRate\":"<<c.carrier.rate/1000<<"}";
        }
        out<<"],\"candidates\":[";
        for(size_t i=0;i<candidates.size();++i) {if(i)out<<',';out<<"{\"frequency\":"<<candidates[i].mhz<<",\"symbolRate\":"<<candidates[i].rate/1000<<"}";}
        out<<"]}";return out.str();
    }
};
