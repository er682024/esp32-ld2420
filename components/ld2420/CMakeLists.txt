idf_component_register(
    SRCS "ld2420.c"
    INCLUDE_DIRS "."
    REQUIRES driver
    PRIV_REQUIRES esp_timer
)