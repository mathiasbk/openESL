set(srcs app_main.c http_api.c sched.c store_nvs.c tagfw.c wifi.c)

if(CONFIG_ESL_RADIO_STUB)
    list(APPEND srcs radio_stub.c)
else()
    list(APPEND srcs radio_ieee802154.c)
endif()

if(CONFIG_ESL_SERIAL_API)
    list(APPEND srcs serial_api.c)
endif()

if(CONFIG_ESL_DISPLAY)
    list(APPEND srcs display.c)
endif()

if(CONFIG_ESL_WEBHOOK)
    list(APPEND srcs health.c)
endif()

if(CONFIG_ESL_OTA)
    list(APPEND srcs ota.c)
endif()

if(CONFIG_ESL_WIFI_CAPTIVE)
    list(APPEND srcs captive.c)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    REQUIRES esl_core esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash esp_app_format ieee802154
             esp_driver_usb_serial_jtag vfs esp_lcd esp_driver_spi esp_driver_gpio esp_http_client app_update lwip esp_partition)
# mdns comes from idf_component.yml and is added to this component automatically.
