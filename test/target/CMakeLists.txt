# On-target test app — ESP-IDF + Unity, hardware-coupled paths only.
# Pure logic is covered far faster host-native (test/host); this app exercises
# what only the real chip / QEMU can: NVS round-trip, PSRAM pool allocation, and
# the radio_backend vtable wiring.
#
#   idf.py -C test/target set-target esp32s3 build
#   pytest test/target --embedded-services idf,qemu
cmake_minimum_required(VERSION 3.16)

# Pull in the firmware's components so the test app can exercise them.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(afterglow_target_tests)
