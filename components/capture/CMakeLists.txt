idf_component_register(
    SRCS "capture.c"
    INCLUDE_DIRS "include"
    REQUIRES radio_backend pool classifier
)
