# Changes

## 2026-11-10 - two-hour weather radar and background updates

- Expanded weather history from seven frames (one hour) to up to thirteen frames (two hours).
- Keep refreshing metadata and missing precipitation frames while other tabs are open or the display is asleep, when Wi-Fi remains enabled and connected.
- Opening Weather Radar immediately selects the newest cached image and resumes animation.
- Expire only frames outside the current provider history; keep reusable downloaded frames.
- Compress cached crops losslessly with on-device round-trip checks; retain raw crops when packing would be larger.
- Allow foreground drawing and other network tasks time between downloaded tiles.
- Brighter roads, Thomson center, fixed 15-mile radius, pressure trend, and stock index totals remain.

Uploaded and verified on the physical device: full 13-frame off-page load, cached refresh, lossless checks, and chronological animation loop.
