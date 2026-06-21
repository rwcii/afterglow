# adv-observer — standalone BLE advertisement observer (ESP32-S3).
# Build/flash with ESP-IDF, independent of the Afterglow firmware build.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(adv_observer)
