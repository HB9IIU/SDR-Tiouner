#pragma once
#include "dect2/player.h"
extern "C" {
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cmath>

namespace dect2 {
// Fixed canvas keeps the encoder and WebRTC tracks alive across resolution/codec changes.
// Only the newest presented picture is retained; no extra decode or frame queue.
class BrowserPicture {
public:
    static constexpr int width = 960, height = 540;
    std::vector<uint8_t> bytes = std::vector<uint8_t>(width * height * 3 / 2);
    ~BrowserPicture() { sws_freeContext(scaler_); }
    void black() {
        std::fill(bytes.begin(), bytes.begin() + width * height, 16);
        std::fill(bytes.begin() + width * height, bytes.end(), 128);
    }
    bool update(const VideoFrame& frame) {
        if (frame.w < 2 || frame.h < 2 || frame.w > 8192 || frame.h > 8192) return false;
        const bool rgba = !frame.rgba.empty();
        if (!rgba && ((frame.w & 1) || (frame.h & 1))) return false;
        if (rgba ? frame.rgba.size() < size_t(frame.w) * frame.h * 4 :
            (frame.y.size() < size_t(frame.w) * frame.h || frame.uv.size() < size_t(frame.w) * (frame.h / 2))) return false;
        const double aspect = frame.dar > 0.05 ? frame.dar : double(frame.w) / frame.h;
        int w = width, h = height;
        if (aspect > double(width) / height) h = std::max(2, int(width / aspect) & ~1);
        else w = std::max(2, int(height * aspect) & ~1);
        scaler_ = sws_getCachedContext(scaler_, frame.w, frame.h, rgba ? AV_PIX_FMT_RGBA : AV_PIX_FMT_NV12,
                                      w, h, AV_PIX_FMT_NV12, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler_) return false;
        const int* sourceSpace = sws_getCoefficients(frame.bt709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
        sws_setColorspaceDetails(scaler_, sourceSpace, rgba || frame.fullRange, sws_getCoefficients(SWS_CS_ITU709),
                                0, 0, 1 << 16, 1 << 16);
        black();
        int x = ((width - w) / 2) & ~1, y = ((height - h) / 2) & ~1;
        const uint8_t* src[] = {rgba ? frame.rgba.data() : frame.y.data(), rgba ? nullptr : frame.uv.data(), nullptr, nullptr};
        int strides[] = {frame.w * (rgba ? 4 : 1), rgba ? 0 : frame.w, 0, 0};
        uint8_t* dst[] = {bytes.data() + y * width + x, bytes.data() + width * height + (y / 2) * width + x, nullptr, nullptr};
        int dstStrides[] = {width, width, 0, 0};
        return sws_scale(scaler_, src, strides, 0, frame.h, dst, dstStrides) == h;
    }
private:
    SwsContext* scaler_ = nullptr;
};
}
