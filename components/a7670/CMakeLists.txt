idf_component_register(
    SRCS "A7670.cpp"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_driver_gpio esp_driver_uart esp_timer freertos
)
