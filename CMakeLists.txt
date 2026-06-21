# Afterglow — ESP-IDF top-level project file
# Target: Seeed Studio XIAO ESP32-S3 (esp32s3)
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(afterglow)

# On-air test rig (tools/onair-test): build with -DAG_ONAIR_TEST=1 to compile in
# the serial test hooks (emission ground-truth log, eligibility census, fast
# departure sweep). Off by default — a normal build carries none of it.
if(AG_ONAIR_TEST)
    idf_build_set_property(COMPILE_DEFINITIONS "AG_ONAIR_TEST=1" APPEND)
endif()
