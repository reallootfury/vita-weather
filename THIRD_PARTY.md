# Third-party notices

Vita Weather is an unofficial, substantially modified native PS Vita adaptation inspired by [Breezy Weather](https://github.com/breezy-weather/breezy-weather). Breezy Weather is licensed under LGPL-3.0. The Android application, name, launcher art, and trademarks are not packaged here. The upstream architecture and Open-Meteo field mapping were reviewed at commit `5e36cd77bfa34e94643ee6e7d0903462d7ff9ee4`.

Weather forecasts, geocoding, and US AQI/particulate measurements are provided by [Open-Meteo](https://open-meteo.com/). Spatial air-quality fields use Open-Meteo's CAMS-backed Air Quality API and therefore include attribution to both Open-Meteo and the Copernicus Atmosphere Monitoring Service (CAMS). Users should review their API terms and attribution requirements before redistribution.

[VitaMaps](https://github.com/spyro-98/VitaMaps) architecture, controls, and
cache implementation were reviewed at commit
`e6e70eae28f991cfacfbfac999097d75703102d2`. Vita Weather's C/vita2d map now
adapts VitaMaps' visible-window priority, overlap reuse, continuous Web
Mercator camera, and previous-level fallback concepts to its weather-overlay
renderer. VitaMaps and this adaptation are distributed under GPL-3.0-only;
the VitaMaps copyright and license are preserved by this notice and `LICENSE`.

The day atmosphere background is Raysonho's [BlueSkyWhiteClouds21.jpg](https://commons.wikimedia.org/wiki/File:BlueSkyWhiteClouds21.jpg), dedicated to the public domain under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The downloaded source SHA-256 is `12288760d60c799553ff06ad60e84eedf9cd1b2f29f39f7a97ca2a19189611c4`; the cropped 1088×544 asset SHA-256 is `b2c9863c253ea11c6c81fc365490edccbc5bdb318a8a3a07024585ffc5973eb2`.

The night atmosphere background is Free Nature Stock's [Blue Night Sky](https://commons.wikimedia.org/wiki/File:Blue_Night_Sky.jpg), distributed by ISO Republic and dedicated to the public domain under [CC0 1.0](https://creativecommons.org/publicdomain/zero/1.0/). The downloaded source SHA-256 is `68348f281f28a06033b480efd9e14c555297490939ed9d14c6d083db58c4d527`; the cropped 1088×544 asset SHA-256 is `6a77310705f25fcc14dc9120c14256c862deafb884fe1bd78ed1d68fef88ab20`.

Both photographs were center-cropped and downsampled to the Vita presentation
size. No generated image or restricted third-party artwork is packaged.

`assets/sun-glow.png` is an original, deterministic radial alpha gradient
generated for Vita Weather. It contains no third-party imagery.
SHA-256: `0c42b02494ccefb92f363a0f7b7808bff0117750573b76fab6e3116fc710e52d`.

Additional condition backgrounds are also center-cropped to 1088×544:

- Clear: SpongeLover08's [Clear blue sky](https://commons.wikimedia.org/wiki/File:Clear_blue_sky_(4-12-2026).jpg), CC0 1.0. Source SHA-256 `5679e2d9b34f44610033a9ae4b19825cd40f5f8427b3df6f4f839cb57695ab6d`; processed SHA-256 `fd786ac75f9452a4f3e79320823550a6b86a6c234e33d84a200676ab22252ea1`. A GPU-drawn sun disc and glow are layered over the photograph at runtime.
- Rain: Paolo Neo's [Rain clouds](https://commons.wikimedia.org/wiki/File:Rain_clouds.jpg), released into the public domain by its author. Source SHA-256 `6ad36130077b6b94339e840666c638a2c42a98d7269ad79d69a84fb92a73ea9b`; processed SHA-256 `a1ac7e711af5ecefb2eb87d81f21ebc27edc3172c2622d25825fa80aff4faf2a`.
- Snow: Kalle K's [Snowflakes](https://commons.wikimedia.org/wiki/File:Snowflakes_(Unsplash).jpg), published under CC0 1.0 before Unsplash's 2017 license change. Source SHA-256 `282fad0a5e6d274eb926de1deedd9052b2661ead9edd46dd4ea2c0e6c56f3136`; processed SHA-256 `7aca71527287f6cd733efc48233b0891d4e4d88e2989181158ac5cd18a0dc585`.
- Storm: the U.S. Navy's [Lightning storm over NBVC Point Mugu](https://commons.wikimedia.org/wiki/File:Lightning_storm_over_NBVC_Point_Mugu.jpg), a U.S. federal-government public-domain work. Source SHA-256 `3dfa926bfde111417f8cc95699d4463e6363a9233c37603e4025977c7f23e000`; processed SHA-256 `345e153db08a536ad4f5ae26744c193b2b323c331b0126217ad1eee8a3a26fe8`.
- Fog: Nathan Anderson's [Fog on a slope in Silverthorne](https://commons.wikimedia.org/wiki/File:Fog_on_a_slope_in_Silverthorne_(Unsplash).jpg), published under CC0 1.0 before Unsplash's 2017 license change. Source SHA-256 `04399bfa1af26c6060fd6c39bb7715d802ca861f62823c6594f0d306d4c772fa`; processed SHA-256 `e9bfdcd53d3bbe88e6a4bccebb3ec12c14d22eaf2a512454f586bb3e5bef886c`.

The VPK also contains uncompressed byte-ordered RGBA renditions of those seven
processed images so the Vita does not decode large PNG files on its render
thread during startup or condition changes. Each file is exactly 2,367,488
bytes. SHA-256: clear
`e43df62907ae0bb7cc8e85de5abadc62a137944e4d043d6d5b2fe2f175523860`,
day `0c535828dc464e88d44aeb70bb6864b8a4e3921067f20b434096b18c7e38016c`,
fog `b705630220b7ed3dc2ed969c7e52c94827aaf0229c023ad22257f208f7153156`,
night `e9d3cecd1d9226f851daaaa4fada28cf8c63fdbdddc16d2cb75bb9d813f142da`,
rain `b8b3485f2a57091246d9de1509524374b541b9fc0a1c1e6858f657cb755042ed`,
snow `53c402553b3568df1646b1639beb36809da15c671259a2f84e4ed05f8ce80578`,
and storm `9249ab392b59edd7f553dfab515078926b82506494af694e4e18bab147ecd14b`.

Optional weather ambience uses two public-domain Wikimedia Commons recordings,
resampled to 48 kHz stereo Ogg Vorbis for Vita playback:

- Rain: Jidane's [Rain.ogg](https://commons.wikimedia.org/wiki/File:Rain.ogg), released into the public domain by its author. Source SHA-256 `c34d12243125c28f09426c076019f8321b602347a8564c38be771cb997c1a152`; processed SHA-256 `8969c38809b4e46dbd0db12b40e681858aeaa92928b6c8f6e5ebcd40a46f1b5b`.
- Storm: Caesar's [Rain and thunder.ogg](https://commons.wikimedia.org/wiki/File:Rain_and_thunder.ogg), released into the public domain by its author. Source SHA-256 `cbfd7b7504bc4e53d6e56ac8d933ba56f97cc28f15a46800c74c2d8eccb3fa89`; processed SHA-256 `1f7ba998e9fe6ab2da7f77e42410dc6e642219d9fb92c81ab87d3ff9b35530f4`.

The Ogg streams are decoded by Tremor from the external VitaSDK toolchain; no
audio codec library is copied into the VPK.

Location-map tiles and map data are provided by [OpenStreetMap](https://www.openstreetmap.org/copyright) and are © OpenStreetMap contributors. The application uses `https://tile.openstreetmap.org/{z}/{x}/{y}.png`, identifies itself with a stable user agent, displays attribution on the Map page, requests only the visible 5-by-3 tile window, and persistently caches successful tiles. It does not offer offline-area download or bulk prefetch.

The optional Topographic map style uses
[OpenTopoMap](https://opentopomap.org/) raster tiles at
`https://tile.opentopomap.org/{z}/{x}/{y}.png`. The Map page displays both
OpenTopoMap and OpenStreetMap contributor attribution when that style is active.
Its cache is isolated from the Standard style and is limited to OpenTopoMap's
published maximum zoom level.

The optional CyclOSM, OSM France, and Humanitarian base styles are served by
the OpenStreetMap France tile infrastructure. Each uses an isolated cache key,
provider-aware maximum zoom, a stable application user agent, and visible
OpenStreetMap contributor attribution. As with the other styles, Vita Weather
requests only the active viewport and does not bulk-download regions.

`src/jsmn.h` is jsmn by Serge Zaitsev, licensed under the MIT License; its license text is retained in that file.

The bundled static Inter Medium font is copyright the Inter Project Authors and
licensed under the SIL Open Font License 1.1. The file was obtained from the
VitaMaps asset set at commit `e6e70eae28f991cfacfbfac999097d75703102d2`
with SHA-256
`97ad806f526e41546d46365bb3a393145f75b7b1568913db74549ad8b8dba872`.
The complete font license is packaged as `OFL-Inter.txt`.

`assets/isrg-root-x1.pem` is the ISRG Root X1 trust anchor distributed by the Internet Security Research Group for TLS certificate validation. `assets/globalsign-root-r3.pem` is the GlobalSign Root CA R3 trust anchor used to validate the standard OpenStreetMap tile-server chain. Certificate verification and hostname verification remain enabled for every provider.

The executable is statically linked against libraries supplied by the VitaSDK
toolchain. The audited build uses libvita2d, Tremor/libvorbisidec 1.2.1,
libcurl 8.17.0, OpenSSL 1.0.2i, Zstandard 1.5.7, FreeType 2.14.3, libpng
1.6.58, libjpeg-turbo 3.1.4, and zlib 1.3.2. Their required notices and license
texts are retained in `DEPENDENCY_LICENSES.md` and packaged in the VPK. The
project's additional GPLv3 linking permission for the VitaSDK OpenSSL build is
in `LICENSE-OPENSSL`.
