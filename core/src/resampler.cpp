#include "dect2/resampler.h"
#include "dect2/dsp_compat.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace dect2 {

static double bessel0(double x) { double s = 1, t = 1; for (int k = 1; k < 40; k++) { t *= (x / (2 * k)) * (x / (2 * k)); s += t; } return s; }

bool RationalResampler::configure(double inRate, double outRate, double* relError, double occupiedHz) {
    double target = outRate / inRate;
    int bestL = 0, bestM = 0;
    double bestErr = 1e9;
    // up to 1024 phases: the radios' own rates need it to be exact (2.5 -> 2.048 Msps for DAB on an Airspy R2 is 512/625; with at most 128
    // the nearest fraction was 100 ppm off, which cost DAB 13 dB of SNR). One phase is 2*H taps, so 1024 phases are 64k floats.
    for (int L = 1; L <= 1024; L++) {
        int M = (int)std::lround(L / target);
        if (M < 1) continue;
        double err = std::fabs((double)L / M - target) / target;
        if (err < bestErr - 1e-12) { bestErr = err; bestL = L; bestM = M; }
        if (err < 2e-8) break;
    }
    if (relError) *relError = bestErr;
    if (bestErr > 1e-4) return false;
    int g = std::gcd(bestL, bestM);
    L_ = bestL / g; M_ = bestM / g;
    // kernel: lowpass at the lower of the two Nyquist rates, normalised to the input rate
    double fc = 0.5 * std::min(1.0, target) * 0.985;
    H_ = target < 0.99 ? 32 : 16;
    double beta = 7.0;
    if (occupiedHz > 0 && target < 0.99 && occupiedHz < outRate) {
        // The signal only fills occupiedHz of the output band, so only what folds into it has to be stopped: the transition band can run from
        // the signal edge to where the first alias reaches the signal edge, a much wider one than the generic design assumes. Kaiser's estimate
        // of the length for 65 dB: far fewer taps per output sample (the cost of the resampler is the taps), with the same alias rejection.
        const double fp = 0.5 * occupiedHz, fs = outRate - fp;
        const double dw = (fs - fp) / inRate;
        const double atten = 65.0;
        const int taps = (int)std::ceil((atten - 7.95) / (2.285 * 2 * M_PI * dw)) + 1;
        H_ = std::max(8, std::min(H_, ((taps + 1) / 2 + 3) / 4 * 4));   // a whole number of 8 tap blocks: desamp finishes a remainder with scalar code
        fc = 0.5 * (fp + fs) / inRate;
        beta = 0.1102 * (atten - 8.7);
    }
    T_ = 2 * H_;
    taps_.assign(L_, std::vector<float>(T_));
    base_.assign(L_, 0);
    nOfPhase_.assign(L_, 0);
    // n_p such that (n_p * M) mod L = p
    std::vector<int> nOf(L_);
    for (int n = 0; n < L_; n++) nOf[(int)(((long long)n * M_) % L_)] = n;
    for (int p = 0; p < L_; p++) {
        int np = nOf[p];
        long long num = (long long)np * M_;
        base_[p] = (int)(num / L_);
        nOfPhase_[p] = np;
        double frac = (double)(num % L_) / L_;
        double sum = 0;
        std::vector<double> h(T_);
        for (int t = 0; t < T_; t++) {
            double x = (t - (H_ - 1)) - frac; // tap k = t-(H-1) sits at distance k - frac from the output instant
            double a = 2 * fc * x;
            double sinc = std::fabs(a) < 1e-12 ? 1.0 : std::sin(M_PI * a) / (M_PI * a);
            double r = x / H_;
            double w = std::fabs(r) >= 1 ? 0 : bessel0(beta * std::sqrt(1 - r * r)) / bessel0(beta);
            h[t] = 2 * fc * sinc * w;
            sum += h[t];
        }
        for (int t = 0; t < T_; t++) taps_[p][t] = (float)(h[t] / sum);
    }
    reset();
    return true;
}

void RationalResampler::reset() {
    re_.assign(H_ - 1, 0.f);
    im_.assign(H_ - 1, 0.f);
}

void RationalResampler::process(const cf32* in, size_t n, std::vector<cf32>& out) {
    if (passthrough()) { out.insert(out.end(), in, in + n); return; }
    size_t old = re_.size();
    re_.resize(old + n); im_.resize(old + n);
    for (size_t i = 0; i < n; i++) { re_[old + i] = in[i].real(); im_[old + i] = in[i].imag(); }
    // buffer index 0 corresponds to input sample (-(H-1)); output n uses samples i0 + k, k = -(H-1)..H
    const long long avail = (long long)re_.size();
    // periods: each consumes M input samples and yields L outputs; the last needed index is base+kM + T - 1
    long long P = (avail - (T_ + M_ + 2)) / M_;
    if (P <= 0) return;
    std::vector<float> ore((size_t)P * L_), oim((size_t)P * L_);
    std::vector<float> tr((size_t)P), ti((size_t)P);
    for (int p = 0; p < L_; p++) {
        const float* hp = taps_[p].data();
        const float* sr = re_.data() + base_[p];
        const float* si = im_.data() + base_[p];
        desamp(sr, M_, hp, tr.data(), (int)P, T_);
        desamp(si, M_, hp, ti.data(), (int)P, T_);
        for (long long k = 0; k < P; k++) { ore[(size_t)k * L_ + nOfPhase_[p]] = tr[k]; oim[(size_t)k * L_ + nOfPhase_[p]] = ti[k]; }
    }
    size_t o0 = out.size();
    out.resize(o0 + (size_t)P * L_);
    for (size_t i = 0; i < (size_t)P * L_; i++) out[o0 + i] = cf32(ore[i], oim[i]);
    // drop consumed input, keeping H-1 history
    size_t drop = (size_t)P * M_;
    re_.erase(re_.begin(), re_.begin() + drop);
    im_.erase(im_.begin(), im_.begin() + drop);
}

} // namespace dect2
