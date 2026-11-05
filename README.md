# DeskBuddy Radar

A touchscreen desk display with weather, stock charts, flight tracking, and animated weather radar. Built for a 2.8-inch ESP32 CYD display.

## Pages

- **Weather:** clock, date, configurable widgets, current temperature, wind, precipitation, and pressure. Pressure uses a three-hour trend; small changes show no arrow.
- **Stocks:** Dow Jones and Nasdaq intraday charts, change from the market opening value, percentage change, and the current total index points below the change. The page shows Market Closed outside regular sessions.
- **Flight Radar:** nearby aircraft over a dark road map, with a sweep, smaller aircraft icons, and an enlarged 220-pixel radar circle.
- **Weather Radar:** up to seven past precipitation frames, with frame time and animation. Downloaded frames stay cached when changing pages.

Both radars are fixed to **Thomson, Georgia**, north up, with a **15-mile radius (30 miles across)**. Their shared map uses brighter gray roads, blue waterways, and white labels. Changing the normal weather location does **not** move the radar map.

Touch the **S** shortcut for Status. A weekday brightness schedule keeps the screen fully bright Monday-Friday, 8 AM-5 PM in the configured local time zone. Normal dimming applies outside that window.

## Hardware verified for this build

| Item | Configuration |
| --- | --- |
| Processor | ESP32-D0WD-V3, 4 MB flash |
| Display | ST7789, 240 x 320, BGR, SPI 40 MHz |
| Touch | XPT2046 |
| Arduino board | ESP32-2432S028R / `jczn_2432s028r` |
| Partition scheme | Huge App, 3 MB, no OTA |

CYD boards can have different display controllers. This build is for the **ST7789** variant. An ILI9341 configuration produced a white screen on the tested board. Do not assume every board sold as ESP32-2432S028R has the same panel.

## Install

See [SETUP.md](SETUP.md) for dependencies, display configuration, build instructions, and first-time Wi-Fi setup.

The public source has **empty Wi-Fi defaults**. Configure your network on the touchscreen after startup; credentials are stored on your own board. The supplied public firmware, when present, is built from those empty defaults. Do not upload an NVS backup or a firmware image built with personal credentials.

## Project status

The working build was uploaded with firmware hash verification and saved-settings checks. The user confirmed the screen, touch, 15-mile radars, Stocks change, and brighter roads. Radar downloads can still report connection failures; retaining cached frames does not guarantee that every request succeeds. A clean-board public build has not been physically tested separately.

The market calendar currently covers 2026-2028; review it before using later years. Weather imagery and aircraft data can be delayed or unavailable. This is a hobby display, not a warning or navigation instrument.

## Firmware credits and licensing

Based on [LextZip/Deskbuddy](https://github.com/LextZip/Deskbuddy), with flight-radar portions adapted from [MatixYo/ESP32-Plane-Radar](https://github.com/MatixYo/ESP32-Plane-Radar). Their MIT notices are retained. The application code is MIT; third-party libraries and map data retain their own licenses. See [THIRD_PARTY.md](THIRD_PARTY.md).

## Enclosure

The enclosure used with this build is based on [Aura, Smart Weather Forecast Display by Surrey Homeware](https://makerworld.com/en/models/1382304-aura-smart-weather-forecast-display). This repository contains **firmware only**. The case files are not included, and the software license does not grant rights to the original enclosure.
