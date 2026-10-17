# Changelog

## 0.1.2 (2026-10-17)

Smarter start, clearer messages, and a way to see where picture problems come from.

- **First start:** channel 1 goes on the beacon and channels 2 to 4 on the strongest stations above it. After that, your start list is kept and is used again at every start.
- **Two channels can no longer receive the same station.**
- **No radio found:** a red card with a Refresh button. When exactly one radio shows up after Refresh, a green card announces it and receiving starts after 3 seconds (Cancel stops it). With several radios, you choose from a card with one button per radio.
- **A radio is always pre-selected**, even a slow one.
- **Radios with a narrow window** (a Pluto over a slow link): a channel outside the window says "Outside radio window" instead of showing another station's picture.
- **TS Diagnostics** on every card, collapsed by default. It shows bitrate, lost frames, CRC, continuity and TEI errors (last 10 s and total) and the time of the last error. A clean transport stream does not prove the picture is clean.
- **Auto Monitor:** cards in the order of the stations on the spectrum, a taller spectrum, the radio window shown, and a repeated line removed.
- **Record button** on the Receiver and the Auto Monitor page. It writes the untouched transport stream of every running channel to `.ts` files in a `recordings` folder next to the program (one file per channel and tuning, named with the date, the frequency and the name of the station; VLC plays them). The button glows and blinks while it records. The beacon is not recorded, because it runs all day. It stops by itself when less than 1 GB of disk is free. A folder button next to it opens the recordings folder.
- A second copy of the program now says that it is already running and opens the page of the first one.
- Auto Monitor: the spectrum is as wide as the cards when they fill two rows.
- **Update check.** A few seconds after the start, the program asks GitHub which release is the latest and shows "Update x.y.z available" in the menu when yours is older. It downloads nothing and sends nothing about you; `check_updates=0` in `multi-receiver.ini` switches it off.
- Card messages are centred.

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
