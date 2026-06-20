idf_component_register(
    SRCS "test_target_main.c"
    INCLUDE_DIRS "."
    REQUIRES unity afterglow_core config pool radio_backend radio_single nvs_flash
    WHOLE_ARCHIVE
)
