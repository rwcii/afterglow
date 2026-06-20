idf_component_register(
    SRCS "classifier.c" "classifier_logic.c"
    INCLUDE_DIRS "include"
    REQUIRES pool config afterglow_core
)
