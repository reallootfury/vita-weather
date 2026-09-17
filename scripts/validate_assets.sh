#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

png_field() {
  local path="$1"
  local offset="$2"
  local count="$3"
  od -An -tu1 -j "$offset" -N "$count" "$path"
}

validate_png() {
  local path="$1"
  local expected_width="$2"
  local expected_height="$3"
  local expected_color_type="$4"
  local -a signature dimensions color

  read -r -a signature <<<"$(png_field "$path" 0 8)"
  if [[ "${signature[*]}" != "137 80 78 71 13 10 26 10" ]]; then
    echo "asset validation: $path is not a PNG" >&2
    return 1
  fi

  read -r -a dimensions <<<"$(png_field "$path" 16 8)"
  local width=$((dimensions[0] * 16777216 + dimensions[1] * 65536 + dimensions[2] * 256 + dimensions[3]))
  local height=$((dimensions[4] * 16777216 + dimensions[5] * 65536 + dimensions[6] * 256 + dimensions[7]))
  read -r -a color <<<"$(png_field "$path" 25 1)"

  if ((width != expected_width || height != expected_height)); then
    echo "asset validation: $path is ${width}x${height}, expected ${expected_width}x${expected_height}" >&2
    return 1
  fi
  if ((color[0] != expected_color_type)); then
    echo "asset validation: $path has PNG color type ${color[0]}, expected $expected_color_type" >&2
    return 1
  fi
}

# VitaShell expects the system icon and static LiveArea images in indexed form.
validate_png assets/icon0.png 128 128 3
validate_png assets/livearea-bg.png 840 500 3
validate_png assets/livearea-startup.png 280 158 3

# The large gate uses transparency; atmosphere photos are native truecolor.
validate_png assets/livearea-gate.png 280 158 6
validate_png assets/weather-day.png 1088 544 2
validate_png assets/weather-night.png 1088 544 2
validate_png assets/weather-clear.png 1088 544 2
validate_png assets/weather-rain.png 1088 544 2
validate_png assets/weather-snow.png 1088 544 2
validate_png assets/weather-storm.png 1088 544 2
validate_png assets/weather-fog.png 1088 544 2
validate_png assets/sun-glow.png 256 256 6

validate_rgba8() {
  local path="$1"
  local expected_size=$((1088 * 544 * 4))
  local actual_size
  actual_size=$(stat -c '%s' "$path")
  if ((actual_size != expected_size)); then
    echo "asset validation: $path is $actual_size bytes, expected $expected_size" >&2
    return 1
  fi
}

validate_rgba8 assets/weather-day.rgba8
validate_rgba8 assets/weather-night.rgba8
validate_rgba8 assets/weather-clear.rgba8
validate_rgba8 assets/weather-rain.rgba8
validate_rgba8 assets/weather-snow.rgba8
validate_rgba8 assets/weather-storm.rgba8
validate_rgba8 assets/weather-fog.rgba8

validate_ogg() {
  local path="$1"
  local -a signature
  read -r -a signature <<<"$(png_field "$path" 0 4)"
  if [[ "${signature[*]}" != "79 103 103 83" ]]; then
    echo "asset validation: $path is not an Ogg stream" >&2
    return 1
  fi
}

validate_ogg assets/weather-rain.ogg
validate_ogg assets/weather-storm.ogg

echo "Vita asset validation: PASS"
