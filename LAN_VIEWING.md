# LAN viewing

The multi-channel web receiver accepts local and private-network IPv4 connections. Open `http://<receiver-PC-address>:47321/receiver` or `/monitor` from another PC on the same LAN.

The page uses its hostname for every WebRTC video connection. MediaMTX advertises the receiver's network interfaces; the existing per-channel encoders are reused. No additional FFmpeg encoder is started for a LAN viewer. Playback ownership is shared across local and remote browsers: opening either page takes playback, and an inactive page can take it back with **Play video here**. This is trusted-LAN access, including the receiver controls available on the pages.

If Windows asks about network access, allow the app and MediaMTX on your Private network. If needed, right-click **Enable LAN access.cmd** in the package and select **Run as administrator** once. The helper reads the package settings and adds inbound rules scoped to the package programs, Private networks, and the local subnet. It leaves other firewall rules unchanged. Explicit pre-existing block rules can still prevent access.

With the default settings (8 channels), the ports are:

- TCP 47321: web pages and control API.
- TCP 18900–18907: WebRTC signalling (one per channel).
- UDP 18190–18197: WebRTC media (one per channel).

RTSP publishing stays on loopback; FFmpeg continues reading the local MPEG-TS source. The receiver rejects arbitrary Host headers and non-private source addresses. Direct IPv4 URLs are supported; custom DNS names, IPv6, internet hosting, and port forwarding are not configured.

`python tools/test_auto_monitor.py --host <receiver-PC-address>` tests HTTP LAN-origin playback with synthetic RF signals, including localhost/LAN ownership handoff. Running it on the receiver PC does not verify another machine's routing or Windows inbound firewall access.
