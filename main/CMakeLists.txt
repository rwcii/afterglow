idf_component_register(
    SRCS "afterglow_main.c"
    INCLUDE_DIRS "."
    REQUIRES
        config
        radio_backend
        radio_single
        capture
        pool
        classifier
        replay
        txentropy
        lifecycle
        mesh
        entropy
)
