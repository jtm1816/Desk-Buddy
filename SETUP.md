# Build and setup

## Dependencies

This version was built with:

- Arduino ESP32 core **3.3.12**
- TFT_eSPI **2.5.43**
- ArduinoJson **7.4.2**
- XPT2046_Touchscreen **1.4**
- The bundled, modified PNGdec **1.1.6** under `libraries/PNGdec`

WiFi, HTTPClient, WiFiClientSecure, WebServer, Preferences, and SPI come from the ESP32 core.

Copy `config/TFT_User_Setup.h` to your TFT_eSPI library as `User_Setup.h`. Save your previous setup first. This enables the tested ST7789 panel, pins, color order, speed, and fonts. Ensure `User_Setup_Select.h` selects this file.

## Build with Arduino CLI

Run from the repository root after installing the dependencies above:

```sh
arduino-cli compile --fqbn esp32:esp32:jczn_2432s028r:PartitionScheme=huge_app --libraries libraries --library libraries/PNGdec --build-path build firmware/Deskbuddy_weather
```

Keep `build_opt.h` beside the sketch: it enables `PNG_SPLIT_DICTIONARY`. The stock PNGdec library does not provide the split-dictionary interface used here. See [the patch notes](libraries/PNGdec/PATCH_NOTES.md).

Arduino IDE users should install the bundled PNGdec copy in a separate sketchbook library folder, select the matching ESP32-2432S028R board and Huge App partition scheme, and open `firmware/Deskbuddy_weather/Deskbuddy_weather.ino`. Preserve the supplied `build_opt.h`. Avoid having multiple competing PNGdec copies selected.

## Upload

1. Connect the board with a USB data cable.
2. Select the correct serial port.
3. If required, hold BOOT, tap RESET/EN, then release BOOT.
4. Upload the sketch using the board's **Huge App** partition layout.
5. Tap RESET/EN after upload if the board stays in download mode.

The application-only binary belongs at **0x10000** and assumes that the correct bootloader and 3 MB partition layout are already installed. It is not a fresh-board image. For a new board, use the Arduino sketch upload flow, which writes the matching bootloader, partitions, and application. Do not flash an application-only binary at 0x0. Do not use a full-device dump containing someone else's settings.

## First startup

- The public firmware contains no network name or password. Allow the initial connection attempt to finish, then select your Wi-Fi network on the touchscreen and enter its password.
- Touch calibration can be requested by holding the screen during the startup calibration prompt. Follow the displayed targets if prompted.
- Once connected, use Status to find the board's local IP address. Open it in a browser on the same network to change themes, widgets, units, weather location, time zone, and other settings.
- Wi-Fi settings are saved on the board. Public downloads do not include anyone's saved NVS settings.

## Radar limitations

The radar center, map, and imagery are fixed together at Thomson, GA, latitude 33.4706, longitude -82.5009. Both maps show a 15-mile radius. Moving only the normal weather location would misalign the map and radar, so this firmware deliberately keeps their center fixed. A different radar location needs regenerated map assets and matching source constants.

The weather view requests up to thirteen past frames spanning the provider's latest two hours and caches decoded crops. It retries missing data, retains good cached frames within the current history window on network failures, and refreshes in the background even when another page is open. The forecast clock, frame time, and animation time are different concepts; the displayed radar time is the image's timestamp.
