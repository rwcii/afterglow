# afterglow_core — portable, privacy-critical math.
# Pure C11, ZERO ESP-IDF / FreeRTOS dependencies, so the exact code that ships
# on-device is also compiled and tested host-native (see test/host). Do not add
# esp_* REQUIRES here — that coupling is what the extraction exists to avoid.
idf_component_register(
    SRCS
        "ag_prng.c"
        "ag_txwalk.c"
        "ag_eviction.c"
        "ag_subset.c"
        "ag_meshguard.c"
        "ag_eligible.c"
        "ag_txad.c"
    INCLUDE_DIRS "include"
)

# math (libm) for expf/powf/logf/sqrt used in the eviction/txwalk/subset models.
target_link_libraries(${COMPONENT_LIB} PRIVATE m)
