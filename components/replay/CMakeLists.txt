idf_component_register(
    SRCS "replay.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend pool classifier txentropy config entropy
)
