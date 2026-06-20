idf_component_register(
    SRCS "pool.c" "pool_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend config entropy afterglow_core esp_timer
)
