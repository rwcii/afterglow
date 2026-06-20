idf_component_register(
    SRCS "mesh.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend pool config entropy afterglow_core
)
