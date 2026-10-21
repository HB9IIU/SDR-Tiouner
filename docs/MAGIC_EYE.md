# Magic Eye indicator

One eye per channel uses the existing /api/state MER, enabled, lock, error, modulation and FEC values. No RF fallback or extra receiver polling is used. A stale connection dims the eye after roughly two seconds. Movement is capped near 30 fps while settling and stops when settled; hidden browser tabs use normal browser animation throttling.

More green area means better MER; the dark gap shrinks. The numeric label remains the actual MER. Common normal-frame DVB-S2 modes use approximate Es/N0 references from the receiver table (core/src/dvbs_s2.cpp), with full illumination five dB above the reference. This is a visual quality scale, not a measured decoder margin or a guarantee of error-free reception. Unknown modes use a 0–15 dB fallback, identified in the tooltip. Lock loss always dims the eye, regardless of RF power.

Optional per-browser tuning: localStorage key `magicEyeScale` accepts a JSON object with `goodMarginDb` (positive number, default 5) and `thresholds` (keys `standard:modulation:fec`, values reference dB). For example: {"goodMarginDb":5,"thresholds":{"2:QPSK:4/5":4.68}}. Delete the key to restore defaults. These visual preferences never change decoding.

Rendering is an original browser canvas implementation inspired by the circular phosphor/shadow concept in G8YTZ Lynx; it uses a single eye per channel rather than a diversity split eye.
