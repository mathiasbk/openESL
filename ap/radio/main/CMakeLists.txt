idf_component_register(
    SRCS radio_main.c
    REQUIRES esl_coproc esl_core esl_lcd ieee802154 esp_driver_uart esp_timer)
