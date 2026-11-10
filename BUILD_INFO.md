# Two-hour/background radar build

Built November 10, 2026 with Arduino-ESP32 3.3.12.

- Target: `esp32:esp32:jczn_2432s028r:PartitionScheme=huge_app`
- Program storage: 1,367,027 / 3,145,728 bytes (43%)
- Static RAM: 60,228 / 327,680 bytes (18%)
- ST7789 240x320 BGR, XPT2046 touch, 3 MB application layout
- Up to 13 ten-minute frames spanning the latest available two-hour window
- Background metadata refresh every 30 seconds after a completed attempt, while Wi-Fi and clock are ready
- Cached frames are reused; obsolete history is pruned and only missing timestamps are downloaded
- Opening Weather Radar selects the newest cached frame without forcing a download
- RainViewer's live feed returned 13 frames spanning 120 minutes during the check
- Cached crops use lossless run-length compression when smaller; raw fallback keeps every pixel unchanged. One 2,304-byte display buffer is reused. Each compressed insertion is verified against the original decoded crop.
- Maximum uncompressed history is 29,952 bytes; actual stored bytes are reported by the diagnostic endpoint.

[Application firmware](firmware/deskbuddy-radar-st7789-app.bin) belongs at **0x10000** on the existing matching 3 MB layout. It is not a fresh-board image; see [SETUP.md](SETUP.md) for source upload instructions. Empty Wi-Fi defaults; no NVS/device dump is included.

Compilation, source/binary privacy checks, firmware upload hash verification, and live 13-frame background/playback checks passed. See [VERIFICATION.md](VERIFICATION.md).
