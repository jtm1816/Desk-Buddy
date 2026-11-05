# Thomson map source

OpenStreetMap extract downloaded November 5, 2026. Data (c) OpenStreetMap contributors, ODbL 1.0: https://www.openstreetmap.org/copyright . The file is gzip-compressed JSON, not program configuration or private device data.

The generated map headers used by the firmware are already included. Regenerating maps is optional. Run `python tools/build_weather_map.py` from the repository root with Pillow installed and the font path adjusted for your system. This regenerates the matching weather and aircraft maps together.
