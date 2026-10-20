# The status screen's panel driver (esl_lcd.h), for main/display.c and the radio
# coprocessor in ap/radio/. Kconfig: ESL_DISPLAY and its pins.
idf_component_register(
    SRCS esl_lcd.c
    INCLUDE_DIRS include
    REQUIRES esl_core esp_lcd esp_driver_spi esp_driver_gpio)
