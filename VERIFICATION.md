# Two-hour radar verification - November 10, 2026

- Firmware upload hash verified on the connected ESP32-D0WD-V3/ST7789 DeskBuddy.
- Saved NVS settings compared byte-for-byte before restart and were unchanged.
- Startup captured no crash, brownout, assert, or abort markers.
- Automatic background load completed with Weather Radar off-screen: 13 frames, 120 minutes, fixed Thomson 15-mile view.
- A subsequent off-page refresh reused cached images and downloaded zero unchanged frames.
- Lossless round-trip checks: 13; codec failures: 0.
- Actual stored cache during this test: 260 bytes. This depends on image content; uncompressed fallback is used when compression would be larger.
- Playback verified all 13 timestamps in chronological order, including loop wraparound.
- First observed playback frame was the newest cached timestamp: True.

This public build was uploaded with existing saved Wi-Fi settings preserved. Fresh-board Wi-Fi onboarding was not retested in this session. Network availability still determines when new radar timestamps can be retrieved.
