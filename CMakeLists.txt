cmake_minimum_required(VERSION 3.10)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
  else()
    message(FATAL_ERROR "Set VITASDK=/usr/local/vitasdk before configuring")
  endif()
endif()

project(vita-weather C)
include("${VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "Vita Weather")
set(VITA_TITLEID "VWEA00001")
set(VITA_VERSION "01.27")

add_custom_target(vita-assets-check ALL
  COMMAND ${CMAKE_SOURCE_DIR}/scripts/validate_assets.sh
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
  COMMENT "Validating Vita-compatible image and audio assets"
  VERBATIM
)

add_executable(${PROJECT_NAME}
  src/audio.c
  src/i18n.c
  src/main.c
  src/map_math.c
  src/net.c
  src/ui.c
  src/weather.c
)
add_dependencies(${PROJECT_NAME} vita-assets-check)

target_compile_features(${PROJECT_NAME} PRIVATE c_std_11)
target_compile_options(${PROJECT_NAME} PRIVATE
  -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -O2
  -ffunction-sections -fdata-sections
)
target_link_options(${PROJECT_NAME} PRIVATE
  -Wl,-q
  -Wl,-z,nocopyreloc
  -Wl,--gc-sections
)

target_link_libraries(${PROJECT_NAME}
  vita2d
  vorbisidec
  curl
  ssl
  crypto
  zstd
  freetype
  png
  jpeg
  z
  m
  SceDisplay_stub
  SceAudio_stub
  SceGxm_stub
  SceSysmodule_stub
  SceCtrl_stub
  SceTouch_stub
  SceCommonDialog_stub
  SceIme_stub
  SceAppMgr_stub
  SceNet_stub
  SceNetCtl_stub
  SceIofilemgr_stub
  SceKernelModulemgr_stub
  SceLocation_stub
)

vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE)
# VitaShell/LiveArea must see the location capability before liblocation will
# let the process open a provider.  ATTRIBUTE bit 1 (value 2) declares that
# this title uses liblocation and allows the normal system permission flow.
if(NOT VITA_MKSFOEX_FLAGS MATCHES "ATTRIBUTE=2")
  set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE=2"
      CACHE STRING "vita-mksfoex flags" FORCE)
endif()
vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
  VERSION ${VITA_VERSION}
  NAME ${VITA_APP_NAME}
  FILE ${CMAKE_SOURCE_DIR}/assets/icon0.png sce_sys/icon0.png
  FILE ${CMAKE_SOURCE_DIR}/assets/livearea-bg.png sce_sys/livearea/contents/bg.png
  FILE ${CMAKE_SOURCE_DIR}/assets/livearea-startup.png sce_sys/livearea/contents/startup.png
  FILE ${CMAKE_SOURCE_DIR}/assets/livearea-gate.png sce_sys/livearea/contents/default_gate.png
  FILE ${CMAKE_SOURCE_DIR}/assets/livearea-template.xml sce_sys/livearea/contents/template.xml
  FILE ${CMAKE_SOURCE_DIR}/assets/Inter-Medium.ttf font.ttf
  FILE ${CMAKE_SOURCE_DIR}/assets/OFL-Inter.txt OFL-Inter.txt
  FILE ${CMAKE_SOURCE_DIR}/assets/sun-glow.png sun-glow.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-day.png weather-day.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-night.png weather-night.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-clear.png weather-clear.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-rain.png weather-rain.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-snow.png weather-snow.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-storm.png weather-storm.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-fog.png weather-fog.png
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-day.rgba8 weather-day.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-night.rgba8 weather-night.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-clear.rgba8 weather-clear.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-rain.rgba8 weather-rain.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-snow.rgba8 weather-snow.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-storm.rgba8 weather-storm.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-fog.rgba8 weather-fog.rgba8
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-rain.ogg weather-rain.ogg
  FILE ${CMAKE_SOURCE_DIR}/assets/weather-storm.ogg weather-storm.ogg
  FILE ${CMAKE_SOURCE_DIR}/assets/isrg-root-x1.pem isrg-root-x1.pem
  FILE ${CMAKE_SOURCE_DIR}/assets/globalsign-root-r3.pem globalsign-root-r3.pem
  FILE ${CMAKE_SOURCE_DIR}/LICENSE LICENSE.txt
  FILE ${CMAKE_SOURCE_DIR}/LICENSE-OPENSSL LICENSE-OPENSSL.txt
  FILE ${CMAKE_SOURCE_DIR}/THIRD_PARTY.md THIRD_PARTY.txt
  FILE ${CMAKE_SOURCE_DIR}/DEPENDENCY_LICENSES.md DEPENDENCY_LICENSES.txt
)
