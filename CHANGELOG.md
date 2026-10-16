# Changelog

## 0.1.1 (2026-10-16)

Improved ergonomics and ease of use: a cleaner layout and clearer messages. The decoding is unchanged.

## 0.1.0 (2026-10-16)

First stable release.

- **Auto Monitor** rebuilt as an operator: it takes the radio itself, shows the beacon first, scans the band above it continuously on the BATC spectrum and creates a card and decoder for every new station. Lost stations leave again after a short time and are retried after a pause. A carrier is only tried if its plateau reaches 60% of the beacon's, and the tilted band floor is fitted so that it is not mistaken for stations.
- **LAN viewing.** The receiver and monitor pages can be opened from other PCs on the same network. A firewall helper for Windows is included.
- The web port is now **47321** by default, and up to 8 decoders can run (`max_channels`).
- The radio used last is remembered and selected again at the next start.
- A clear message when the radio receives no beacon and nothing else locks (LNB connection or bias voltage).
- Playback no longer needs a secure browser context, so it works from other PCs over plain HTTP.

## 0.1.0-beta.1 (2026-10-14)

First public beta: multi-channel QO-100 DATV receiver in the browser, with the BATC spectrum, per-channel decoders, UDP transport stream output and radio support for SDRplay, Airspy, HackRF, PlutoSDR, LimeSDR Mini and RTL-SDR.
