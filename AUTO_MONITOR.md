# Auto Monitor

Open **Auto Monitor** (http://127.0.0.1:47321/monitor with the default port). It works on its own and needs nothing from the Receiver page: opening it takes over the radio. If no capture is running it starts the radio selected on the Receiver page (or the best one found) with automatic gain. If the Receiver page already has a capture running, the monitor keeps that capture and releases the receiver channels.

How it works:

1. The first card is the beacon (10491.500 MHz, 1500 kS/s), shown at once.
2. The backend scans the BATC spectrum once a second, only above the beacon (from 10492.0 MHz).
3. A candidate must remain visible over three checks. Its frequency and symbol rate are estimated from the spectrum, and a new decoder and a new card are created for it (standard selection is automatic).
4. A card whose signal disappears stays on screen marked **LOST**, and its decoder is released. After 45 seconds the same card is reused if the signal returns.

There is no fixed number of cards: a decoder is created for every station found, up to `max_channels` in `multi-receiver.ini` (default 8, maximum 16). Each decoder costs CPU/GPU and a video encoder. The scanner runs in the backend, so it continues when the page is closed.

The capture window stays fixed: only carriers that fit completely inside it are assigned. Spectrum estimates can be wrong for weak, crowded or unusual carriers.

The page shows a read-only BATC spectrum with station names, and a grid of cards in two or four columns with one volume slider per card (zero mutes; raising another card mutes the previous one). Video playback belongs to one page at a time: opening Receiver or Auto Monitor takes playback from the previous page; the other shows **Play video here**.

Any manual operation on the Receiver page (tuning, capture alignment, calibration, radio stop) stops the automatic scanning and leaves the channels as they are. Gain and UDP output can be changed while monitoring. Temporary monitor channels are never written to the settings file. Reopening the page starts the monitor again.

In demo mode (`--demo`) the band scan uses a synthetic spectrum of the two demo carriers instead of the BATC internet feed.

## Validation

- `ctest --test-dir build-windows -R '^(auto_monitor|multi_receiver)$' --output-on-failure`: spectrum detection, beacon card, scanning above the beacon, cards created on demand, lost cards and cooldown, capture boundaries, stale-feed hold, and stopping without retuning; existing shared-receiver checks.
- `python tools/test_auto_monitor.py`: isolated synthetic RF demo, two decoded WebRTC videos, audio selection, layout controls, command authentication, backend survival after page close, automatic startup, passive spectrum, stopping without retuning, settings preservation, and manual override. Requires Playwright and installed Chrome. It does not open physical radio hardware.
