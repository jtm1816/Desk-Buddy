# Credits and third-party material

## Application origins

- **LextZip / Deskbuddy**, MIT: https://github.com/LextZip/Deskbuddy . The original V.8 application is the base for this firmware. Notice: `LICENSES/Deskbuddy-MIT.txt`.
- **MatixYo / ESP32-Plane-Radar**, MIT: https://github.com/MatixYo/ESP32-Plane-Radar . Flight-radar portions are adapted for this rectangular display. Notice: `LICENSES/Plane-Radar-MIT.txt`. The existing source labels the adapted version 1.1.5; that version label was not independently verified against a release tag.

## Bundled decoder

**PNGdec 1.1.6**, Larry Bank / bitbank2, Apache-2.0: https://github.com/bitbank2/PNGdec . Its license and source notices are retained under `libraries/PNGdec`. The bundled zlib source retains its own notices. Our conditional changes separate the inflate dictionary so it can share reserved sprite memory; see `PATCH_NOTES.md`. No global installation is modified by this repository.

## External libraries

TFT_eSPI (Bodmer), ArduinoJson (Benoit Blanchon), XPT2046_Touchscreen (Paul Stoffregen and contributors), and the Espressif Arduino-ESP32 core remain under their respective upstream licenses. Install them through their original distributions with the upstream notices. Their full implementations are not bundled here.

## Maps

Map data **(c) OpenStreetMap contributors**, licensed under the [Open Data Commons Open Database License 1.0](https://www.openstreetmap.org/copyright). This credit is also drawn on the weather radar screen. The generated map arrays and source map database are excluded from the application's MIT license; ODbL obligations continue to apply to the database and derived database material.

The compressed source extract and generator are under `assets/map-source` and `tools` for reproducibility. The source map extract was downloaded November 5, 2026. Town-name text was rasterized with the locally installed Arial font; no font file is redistributed. The map generator requires a suitable local font.

## Data services

- Weather: [Open-Meteo](https://open-meteo.com/), displayed weather values and pressure trend.
- Radar: [RainViewer](https://www.rainviewer.com/api/weather-maps-api.html), past precipitation tiles and timestamps.
- Aircraft: [adsb.fi](https://opendata.adsb.fi/), internet ADS-B data. The ESP32 does not receive ADS-B radio signals directly.
- Stocks: Yahoo Finance chart endpoints, Dow Jones / Nasdaq chart data. This project is not affiliated with Yahoo, DJI, Nasdaq, or any data provider.
- Sunrise/sunset: https://sunrise-sunset.org/api
- KP index: NOAA SWPC.
- Network time: configured public NTP servers.

Source acknowledgments do not grant additional rights to provider data or promise continued API availability.

## Enclosure inspiration

[Aura, Smart Weather Forecast Display](https://makerworld.com/en/models/1382304-aura-smart-weather-forecast-display), Surrey Homeware. Case licensing is separate from both this MIT firmware and Aura's GPL-licensed software. No Aura case files or Aura firmware are included here.
