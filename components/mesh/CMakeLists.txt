idf_component_register(
    SRCS "mesh.c" "mesh_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend pool config entropy afterglow_core esp_timer
)
