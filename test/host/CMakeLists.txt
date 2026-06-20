# Host-native test suite for afterglow_core — plain CMake + CTest, NO ESP-IDF.
# Builds the exact ag_core sources that ship on-device and links them against
# fast host unit tests + the statistical separability harness.
#
#   cmake -S test/host -B build/host && cmake --build build/host
#   ctest --test-dir build/host --output-on-failure
cmake_minimum_required(VERSION 3.16)
project(afterglow_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wshadow -O2)

# The portable core, compiled from the same sources that ship on-device.
set(CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/afterglow_core)
add_library(afterglow_core STATIC
    ${CORE_DIR}/ag_prng.c
    ${CORE_DIR}/ag_txwalk.c
    ${CORE_DIR}/ag_eviction.c
    ${CORE_DIR}/ag_subset.c
    ${CORE_DIR}/ag_meshguard.c
    ${CORE_DIR}/ag_eligible.c
)
target_include_directories(afterglow_core PUBLIC ${CORE_DIR}/include)
target_link_libraries(afterglow_core PUBLIC m)

# The statistical-metrics library (shared by the separability tests).
add_library(separability STATIC separability/detector.c)
target_include_directories(separability PUBLIC separability)
target_link_libraries(separability PUBLIC afterglow_core m)

enable_testing()

# --- unit tests -----------------------------------------------------------
foreach(t test_eviction test_txwalk test_subset test_meshguard)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE afterglow_core)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# --- separability tests ---------------------------------------------------
foreach(t test_separability test_eligibility)
    add_executable(${t} separability/${t}.c)
    target_link_libraries(${t} PRIVATE separability afterglow_core)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
