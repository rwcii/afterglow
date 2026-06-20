# Concrete single-radio backend: implements the radio_backend vtable and owns
# radio_backend_get(). Depends on the interface only (acyclic).
idf_component_register(
    SRCS "radio_single.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend esp_wifi bt esp_timer esp_hw_support
)
