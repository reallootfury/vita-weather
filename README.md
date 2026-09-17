## AI disclosure

Vita Weather was fully built with the assistance of generative AI tools under
human direction. AI tools were used for implementation, interface iteration,
documentation, debugging, build automation, and test development. Human review
and physical-hardware testing remain necessary. This disclosure does not alter
third-party licenses or authorship notices.

# Vita Weather

Vita Weather is a native PS Vita forecast client built with VitaSDK and
vita2d. It uses the libre Open-Meteo APIs and presents current, hourly, daily,
map, and air-quality information in a controller-first interface. No external
runtime-data pack is required.

## Features

- Current conditions, feels-like temperature, humidity, UV, pressure,
  visibility, wind, precipitation, AQI, particulates, sun, and moon data.
- Smooth 24-hour and selectable 10-day forecasts with metric charts and
  explanatory summaries.
- Saved locations, live geocoding suggestions, touch input, and an experimental
  native location-service path for supported Vita hardware.
- Full-screen and embedded maps with five OpenStreetMap-based styles, a
  persistent on-demand tile cache, a 24-hour forecast dial, and temperature,
  precipitation, wind, and US AQI layers.
- Realistic and pixel weather scenes with Animated, Gentle, and Static motion.
- English, Spanish, French, and German; 12/24-hour time; complete Imperial and
  Metric unit systems.
- Last-successful forecast and AQI caches for offline viewing.

Radar, satellite imagery, severe-alert feeds, and background notifications are
not included. Map precipitation is an hourly forecast field, not radar.

## Controls

- `L` / `R`: change the main page.
- D-pad: move within lists, forecasts, charts, and the map timeline.
- `X`: confirm or open the selected item.
- `Circle`: go back; in search, it hides the keyboard first.
- `Square`: open saved locations, then predictive search.
- `Triangle`: switch units, day-chart metrics, or map layers as appropriate.
- `Start`: open Settings.
- Map: left stick pans, right-stick up/down zooms, and `X` enters full-screen.
- Touch: tabs, buttons, rows, forecast strips, detail scrolling, map dragging,
  and the map timeline support front-touch input.

## Install

Install the release VPK with VitaShell. The title ID is `VWEA00001`. A network
connection is required for the first live forecast; later launches can display
the last successful sync offline.

Native GPS remains experimental. On the currently tested 3.74 hardware,
`sceLocationOpen` can fail with `0x8010124f` before a fix is requested. The app
falls back to the selected saved location instead of blocking the UI.

## Build

From the repository root in WSL Ubuntu:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"

cmake -S . -B build \
  -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The package is written to `build/vita-weather.vpk`. LiveArea images are
validated before compilation because VitaShell requires indexed-color icon,
background, and startup PNGs.

The build runs `scripts/validate_assets.sh` automatically before packaging.
Emulator results are not physical-Vita proof.

## Data, storage, and diagnostics

Forecast, geocoding, and air-quality requests go to Open-Meteo over verified
HTTPS. Map tiles are requested only for the visible view, are credited on-map,
and are cached under `ux0:data/vita-weather/map`; the app does not bulk-download
regions. Forecast data is stored under `ux0:data/vita-weather/` for offline use.

The bounded runtime log is:

```text
ux0:data/vita-weather/loader.log
```

## License and credits

Vita Weather is distributed under GPL-3.0-only with the additional linking
permission in `LICENSE-OPENSSL`. Source and binary redistribution must retain
`LICENSE`, `LICENSE-OPENSSL`, `THIRD_PARTY.md`, and
`DEPENDENCY_LICENSES.md`.

The project is an independently implemented native client informed by
[Breezy Weather](https://github.com/breezy-weather/breezy-weather) and the map
architecture of [VitaMaps](https://github.com/spyro-98/VitaMaps). Weather data
comes from [Open-Meteo](https://open-meteo.com/); map data is © OpenStreetMap
contributors. Complete code, asset, font, audio, provider, and linked-library
attribution is in [THIRD_PARTY.md](THIRD_PARTY.md).
