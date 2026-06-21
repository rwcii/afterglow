# adv-stimulus — standalone controllable BLE advertiser (ESP32-S3).
# A companion board for the on-air test rig: it advertises a known source on
# serial command so a device under test can be driven through capture, classify,
# departure, and replay deterministically.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(adv_stimulus)
