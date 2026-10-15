# SDR-Tiouner by HB9IIU

SDR-Tiouner is a multi-channel QO-100 DATV receiver for SDR hardware (SDRplay, Airspy R2, HackRF): one wideband radio, several DVB-S/S2 channels decoded and shown at once in the browser. It is meant as a software alternative to the MiniTiouner. The channel pool is dynamic: up to `max_channels` decoders (default 8, set in `multi-receiver.ini`).


## Free of charge, not for sale

SDR-Tiouner is free software, made by an amateur for the amateur radio community. It is given away free of charge and it is **not for sale**.

- You never have to pay to receive it, use it, copy it or pass it on.
- If anyone asks you to pay for this program, please do not pay, and let HB9IIU know.
- You are welcome to share it with other radio amateurs, free of charge, together with `SOURCE-OFFER.txt` and the licence files.

The GNU General Public License (`LICENSE.txt`) lets anyone charge a fee for a copy, so this is the wish of the author and not an added condition. The terms of the licence are unchanged.
## Launch

Run `Start SDR-Tiouner.cmd` in the package folder (or `SDR-Tiouner.exe`).

The browser opens at `http://127.0.0.1:47321`. Close the other receiver before starting the same SDRplay in SDR-Tiouner. Choose the appropriate SDRplay antenna entry, set gain, and press **Start capture**. Enter the frequency and symbol rate for each carrier, then press **Start** on each channel. **Retune** and **Stop** affect only their channel. The BATC spectrum is at the top: select a channel below it and click a transmission to tune that channel, with an estimated symbol rate. The received SDR spectrum and waterfall remain below the radio controls. Press **Sound** on the video you want to hear. **Close** exits the backend and its helper processes.

To exercise two generated RF carriers without touching a radio:

```
SDR-Tiouner.exe --demo
```

The demo sends DVB-S2 at 10491.500 MHz and DVB-S at 10496.500 MHz, both QPSK at 1500 kS/s. They are generated in memory; no transmission occurs. Use `SDR-Tiouner.exe --demo-s2` to test two DVB-S2 carriers instead.

## UDP transport stream (original quality)

The **Auto Monitor** page at `/monitor` scans the band above the beacon on the BATC spectrum and creates a card and decoder for every station it finds. It takes the radio itself when the page opens. See [AUTO_MONITOR.md](AUTO_MONITOR.md).

The browser tiles are re-encoded to at most 960x540 H.264 for WebRTC. Next to that, every running channel also sends its **original MPEG transport stream** (exactly as it came out of the demodulator: its own resolution, codec and audio, no re-encoding) over UDP, as MiniTiouner does:

- Channel *n* goes to `udp://127.0.0.1:19000 + n - 1` (7 TS packets per datagram). The address is shown on the channel card.
- Open it in VLC (Media > Open Network Stream > `udp://@127.0.0.1:19000`), `ffplay udp://127.0.0.1:19000`, OBS, or record it with `ffmpeg -i udp://127.0.0.1:19000 -c copy out.ts`.
- `multi-receiver.ini`: `udp_output=1` (0 turns it off), `udp_host=127.0.0.1` (another IPv4 address, such as a PC on the LAN or a multicast group, sends it there), `udp_port_base=19000`.
- The stream starts when the channel is started and restarts by itself after a retune.
## Airspy R2 / Mini

The Airspy R2 and Mini are driven natively (`core/src/source_airspy.cpp`); the package carries `libairspy.dll` (libairspy 1.0.10 with the close-order patch, `tools/package/patches/`) next to its libusb. Install the WinUSB driver once with Zadig (choose the Airspy, driver WinUSB). Close SDR# or any other program that has the radio open first.

- The R2 samples 10 Msps (the full QO-100 section; about 9 MHz is usable) or 2.5 Msps; the Mini 6 or 3 Msps, which is too slow for SDR-Tiouner (it needs 9 Msps). The sample rate is chosen from what the radio reports.
- At 10 Msps the driver switches the radio to packed 12-bit USB transfers (30 MB/s instead of 40 MB/s). Without that, USB 2 loses about 4% of the samples and nothing decodes although the spectrum looks normal. If the log (stderr) says samples were lost on the USB link, use a USB 3 port without a hub.
- Tuning range 24-1800 MHz, so the 745 MHz IF of a 9750 MHz LNB is in range. The antenna-power switch is the Airspy's 4.5 V bias-tee; it cannot power an LNB (12-18 V), so use an external bias-tee/power inserter and leave it off.
- Gain is one overall step 0-21 (linearity table by default, sensitivity table in the radio settings); the shared Auto gain works on that range. The Airspy has no overload report, so Auto reacts to the spectrum level only.
- Frequency correction (`correction_hz`, or the ppm setting) is applied by tuning off-centre; the Airspy library has no correction of its own.

## Configuration and architecture

`multi-receiver.ini` beside the executable sets the pool cap (`max_channels`, 8 by default), shared sample rate, IF centre, LNB LO, frequency correction, gain, ports, and a list of channels. Channel parameters changed through the web page are saved there. Hardware always requires **Start capture**; it does not auto-start on launch. Advanced antenna power is off. Select the antenna entry matching the cable connection.

- One `IqSource` opens the radio once, with a fixed 745 MHz IF centre and requested 10 Msps. With a 9750 MHz LNB, the nominal sampled span is satellite 10490–10500 MHz. The hardware's actual sample rate is checked. Usable RF bandwidth and attenuation at the edges must be checked with the RSP and LNB.
- `MultiReceiver` allocates channel objects from a configurable pool. IDs, status, publisher ports and cards are data driven. The two generated test carriers are test fixtures, not the production channel architecture.
- Immutable shared IQ blocks feed bounded per-channel queues. Each worker reuses the existing DVB-S DSP mixer/half-band filters for digital downconversion and decimation, then runs an independent `DvbsReceiver` and TS demux. The existing receiver requires at least 2 Msps at its input, so decimation stops above that limit.
- The backend's per-channel FFmpeg processes decode MPEG-TS and encode browser-compatible H.264/Opus (up to 960×540, two decoder/encoder threads). Each has an isolated MediaMTX WebRTC publisher. The browser renders and plays; it performs no DATV demodulation.
- A channel's stop/retune closes only its TS subscribers and restarts only its publisher. Capture tuning and other workers are untouched. Bounded IQ/TS queues prevent an overloaded channel or disconnected browser from accumulating unlimited memory. Software exceptions in a channel are contained; an OS/process crash or shared-radio failure can still affect the backend.
- The page displays the actual received-band spectrum and a bounded canvas waterfall. It does not substitute the internet BATC spectrum.

Audio starts muted because of browser autoplay policy. Each card has a Sound/Mute toggle. Modulation/FEC are automatic; standard can be automatic, DVB-S, or DVB-S2.

## Validation and limits

This is a beta (0.1.0-beta.1), not a claim that the requirements are fully accepted. Two synthetic RF carriers, browser pictures/audio tracks, independent channel stop/retune, API protections, pool limits, and resource samples can be tested locally. Live QO-100 acceptance requires two actual carriers on the connected SDRplay. CPU <25% of the whole PC, receiver/helper RAM <500 MB, end-to-end latency <3 s, and a one-hour run without growth/dropouts must be measured with representative live transmissions; video resolution, symbol rate, RF conditions and FEC workload affect these results.

For repeatable resource acceptance, the proposed 500 MB budget covers the receiver and its managed FFmpeg/MediaMTX helpers. Browser CPU/RAM and the combined total are reported separately. Working-set totals can count shared DLL pages more than once, so the report states the metric rather than treating it as exact whole-system memory use.

Build with `bash tools/package/make_multi_windows.sh`. Automated integration checks use `tools/test_multi_receiver.py --duration 600`; use `--duration 3600` for a one-hour synthetic soak. Tests use a separate port set, temporary settings and synthetic input, and leave physical radios alone. The test reports backend/helper CPU and memory separately from Chromium, checks advancing pictures and records IQ drops. Synthetic generator CPU is included in the backend measurement; this is not a live RF measurement.

Useful references: [SDRplay instantaneous bandwidth illustration](https://www.sdrplay.com/docs/Hamcation2024slides.pdf), [MediaMTX browser playback](https://mediamtx.org/docs/usage/read).

Shared gain defaults to Auto. It monitors the whole received band and SDRplay RF-overload reports, reduces gain quickly during overload, and raises it slowly with headroom. Select Manual, enter a gain and press Apply gain to change it while capture runs. Gain updates keep the capture centre and channel parameters unchanged. The receiver spectrum/waterfall are display-only; only the BATC spectrum supports click-to-tune.

Spectrum alignment: both plots use the actual RX centre and sample rate, including frequency calibration. BATC data occupies its true 10490.5–10499.5 MHz interval on that shared scale; blank margins contain no BATC data and cannot be clicked to tune.

## LAN viewing

Open `http://<receiver-PC-IP>:47321/` from another PC on the same LAN. Existing encoded feeds are reused, with playback on one page at a time across local and remote browsers. See [LAN_VIEWING.md](LAN_VIEWING.md) for addresses and the Windows firewall helper.
