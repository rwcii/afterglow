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
    ${CORE_DIR}/ag_txad.c
)
target_include_directories(afterglow_core PUBLIC ${CORE_DIR}/include)
target_link_libraries(afterglow_core PUBLIC m)

# The statistical-metrics library (shared by the separability tests).
add_library(separability STATIC separability/detector.c)
target_include_directories(separability PUBLIC separability)
target_link_libraries(separability PUBLIC afterglow_core m)

# Portable component logic compiled host-side against thin ESP-IDF shims (see
# shim/). Only the pure-logic parts of the firmware components are listed here;
# hardware-touching code (radio, NVS I/O) is exercised on-target, not here.
set(COMP_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../components)
add_library(components_host STATIC
    ${COMP_DIR}/config/config.c
    ${COMP_DIR}/pool/pool_logic.c
    ${COMP_DIR}/classifier/classifier_logic.c
    ${COMP_DIR}/lifecycle/lifecycle_logic.c
    ${COMP_DIR}/mesh/mesh_logic.c
)
target_include_directories(components_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/shim
    ${COMP_DIR}/config/include
    ${COMP_DIR}/pool/include
    ${COMP_DIR}/classifier/include
    ${COMP_DIR}/lifecycle/include
    ${COMP_DIR}/mesh/include
    ${COMP_DIR}/radio_backend/include
)
target_link_libraries(components_host PUBLIC afterglow_core m)

enable_testing()

# --- unit tests -----------------------------------------------------------
foreach(t test_eviction test_txwalk test_subset test_meshguard test_txad)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE afterglow_core)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# --- component-logic tests (host shims) -----------------------------------
foreach(t test_config test_pool test_classifier test_adv_kind test_lifecycle test_mesh)
    add_executable(${t} ${t}.c)
    target_link_libraries(${t} PRIVATE components_host afterglow_core)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# --- separability tests ---------------------------------------------------
foreach(t test_separability test_eligibility)
    add_executable(${t} separability/${t}.c)
    target_link_libraries(${t} PRIVATE separability afterglow_core)
    add_test(NAME ${t} COMMAND ${t})
endforeach()
