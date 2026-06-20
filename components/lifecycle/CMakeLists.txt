idf_component_register(
    SRCS "lifecycle.c" "lifecycle_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES pool config entropy afterglow_core esp_timer
)
