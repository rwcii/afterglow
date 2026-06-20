idf_component_register(
    SRCS "txentropy.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend config entropy classifier pool afterglow_core
)
