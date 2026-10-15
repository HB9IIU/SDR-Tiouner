#include "../core/src/browser_picture.h"
#include <cstdio>
using namespace dect2;
int main() {
    BrowserPicture picture;
    picture.black();
    if (picture.bytes.size() != 960 * 540 * 3 / 2 || picture.bytes[0] != 16 || picture.bytes.back() != 128) return 1;
    VideoFrame frame; frame.w = 320; frame.h = 240; frame.dar = 4.0 / 3;
    frame.y.assign(320 * 240, 180); frame.uv.assign(320 * 120, 128);
    if (!picture.update(frame)) return 1;
    // A 4:3 picture is pillarboxed, never stretched to 16:9.
    if (picture.bytes[270 * 960] != 16 || picture.bytes[270 * 960 + 480] < 170) return 1;
    frame.w = 640; frame.h = 360; frame.dar = 16.0 / 9;
    frame.y.assign(640 * 360, 100); frame.uv.assign(640 * 180, 128);
    if (!picture.update(frame) || picture.bytes[270 * 960] < 90) return 1;
    frame.rgba.assign(640 * 360 * 4, 255);
    if (!picture.update(frame) || picture.bytes[270 * 960 + 480] < 220) return 1;
    frame.rgba.resize(1);
    if (picture.update(frame)) return 1;
    puts("PASS: fixed-size NV12, aspect ratio, resolution changes, RGBA and invalid frames");
}
