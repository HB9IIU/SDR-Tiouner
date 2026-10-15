#include "dect2/quality.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace dect2 {

double requiredSnrDb(const PlpFec& f) {
    // rows: QPSK, 16-QAM, 64-QAM, 256-QAM; columns: code rate 1/2, 3/5, 2/3, 3/4, 4/5, 5/6
    static const double t[4][6] = {{1.0, 2.2, 3.1, 4.1, 4.7, 5.2},
                                   {6.0, 7.7, 8.9, 10.2, 11.0, 11.6},
                                   {9.2, 11.4, 13.1, 14.9, 15.9, 16.7},
                                   {12.1, 14.6, 17.0, 18.9, 20.3, 21.4}};
    const int m = std::max(0, std::min(3, f.mod));
    const int r = f.rate >= 0 && f.rate < 6 ? f.rate : 0;
    return t[m][r] + (f.rotation ? 0.0 : 0.0);
}

void QualityMeter::reset() { *this = QualityMeter(); }

// DVB-T: required C/N for quasi error free reception in a Gaussian channel (EN 300 744 table), by modulation and code rate


void QualityMeter::update(const RxTelemetry& rx) {   // the modes added after FM (DVB-S/S2, DTMB, ...): there is no table of required SNR, so the score is how much of the recent data decoded
        if (rx.state != 2 && !rx.dataValid) { rep_ = QualityReport(); rep_.label = "no lock"; return; }
        const uint64_t good = rx.blocksOk, bad = rx.blocksBad;
        if (lastOk_ != ~0ull && (good >= lastOk_ && bad >= lastBad_) && (good - lastOk_ + bad - lastBad_) > 0) {
            hist_.push_back({good - lastOk_, bad - lastBad_});
            if (hist_.size() > 40) hist_.pop_front();
        }
        lastOk_ = good; lastBad_ = bad; lastFrames_ = 0;
        uint64_t ok = 0, ng = 0;
        for (auto& h : hist_) { ok += h.first; ng += h.second; }
        QualityReport r;
        r.valid = true;
        r.snrDb = rx.dataSnrDb;
        r.fecOk = (ok + ng) ? (double)ok / (double)(ok + ng) : (rx.dataValid ? 1.0 : 0.0);
        const double q = 100.0 * r.fecOk * r.fecOk * (rx.dataValid ? 1.0 : 0.5);
        smooth_ = rep_.valid ? smooth_ + 0.25 * (q - smooth_) : q;
        r.percent = smooth_;
        r.label = r.percent >= 85 ? "excellent" : r.percent >= 65 ? "good" : r.percent >= 40 ? "marginal" : "poor";
        rep_ = r;
        return;

}

} // namespace dect2
