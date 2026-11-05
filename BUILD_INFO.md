# Public firmware build

Built November 5, 2026 with Arduino-ESP32 3.3.12.

- Target: `esp32:esp32:jczn_2432s028r:PartitionScheme=huge_app`
- Program storage: 1,364,971 / 3,145,728 bytes (43%)
- Static RAM: 57,764 / 327,680 bytes (17%)
- Display: ST7789 240x320 BGR, 40 MHz SPI
- `PNG_SPLIT_DICTIONARY` enabled; bundled patched PNGdec 1.1.6
- Public build has empty Wi-Fi defaults. Source and binary were checked against private network strings without recording those values.
- Map generator verified to reproduce both included map headers byte-for-byte from the supplied cropped OSM extract.

## Download

[Application firmware](firmware/deskbuddy-radar-st7789-app.bin) ([SHA-256](SHA256SUMS)).

This is an **application-only** image for **0x10000**, for a board that already has the matching Huge App partition layout. For a fresh board, use the source/Arduino upload instructions in [SETUP.md](SETUP.md). This image is not a full-flash dump and contains no saved NVS data.

The public build compiles successfully. It has not been flashed separately to a clean board; the user's installed build, with the same application behavior and private Wi-Fi defaults, was verified on the physical device. No device was reflashed during GitHub preparation.
