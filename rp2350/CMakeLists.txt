cmake_minimum_required(VERSION 3.13)

# RP2350A SID 6581 controller firmware, derived from RPFM
# (E:\working\vscode-projects\YM2163-Midi\RPFM).
#
# Build:
#   export PATH=/d/msys64/ucrt64/bin:/d/msys64/mingw64/bin:/d/msys64/usr/bin:/usr/bin:/bin
#   cmake -S . -B build -G "Unix Makefiles" \
#         -DCMAKE_MAKE_PROGRAM=/d/msys64/usr/bin/make.exe \
#         -DPICO_PLATFORM=rp2350-arm-s -DPICO_BOARD=pico2
#   cmake --build build -j8

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    get_filename_component(PICO_SDK_PATH "${CMAKE_CURRENT_SOURCE_DIR}/pico-sdk-2.2.0" ABSOLUTE)
    message("Using local PICO_SDK_PATH: ${PICO_SDK_PATH}")
endif()

include(pico_sdk_import.cmake)

project(rpfm_sid_player C CXX ASM)

pico_sdk_init()

# The player. 11-bit bus behind 2x 74LVC245, PIO-generated PHI2 at the PAL
# colourburst frequency (985248 Hz).
add_executable(rpfm_sid_player
    src/main.c
    src/usb_descriptors.c
    src/emu/scc.c
    src/lcd.c
)

# Bring-up target: WS2812 marquee + heartbeat LED only. Same USB descriptors,
# so CMD_BOOTSEL still works and flashing stays hands-free.
add_executable(led_test
    src/led_test.c
    src/usb_descriptors.c
)

# tusb_config.h has to win over the SDK's own copy.
include_directories(BEFORE ${CMAKE_CURRENT_SOURCE_DIR}/src)

pico_generate_pio_header(rpfm_sid_player ${CMAKE_CURRENT_SOURCE_DIR}/src/spfm_bus.pio)
pico_generate_pio_header(rpfm_sid_player ${CMAKE_CURRENT_SOURCE_DIR}/src/pio_cs.pio)
pico_generate_pio_header(rpfm_sid_player ${CMAKE_CURRENT_SOURCE_DIR}/src/phi2.pio)
pico_generate_pio_header(rpfm_sid_player ${CMAKE_CURRENT_SOURCE_DIR}/src/ws2812.pio)

pico_generate_pio_header(led_test ${CMAKE_CURRENT_SOURCE_DIR}/src/ws2812.pio)

target_link_libraries(rpfm_sid_player PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_pio
    hardware_pwm
    hardware_i2c
    pico_multicore
    tinyusb_device
)

target_link_libraries(led_test PRIVATE
    pico_stdlib
    hardware_gpio
    hardware_pio
    tinyusb_device
)

# The USB stack is the only console; UART stdio would fight the pins.
pico_enable_stdio_usb(rpfm_sid_player 0)
pico_enable_stdio_uart(rpfm_sid_player 0)

pico_enable_stdio_usb(led_test 0)
pico_enable_stdio_uart(led_test 0)

pico_add_extra_outputs(rpfm_sid_player)
pico_add_extra_outputs(led_test)
