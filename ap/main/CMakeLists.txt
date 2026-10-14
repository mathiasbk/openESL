set(srcs app_main.c http_api.c store_nvs.c wifi.c)

if(CONFIG_ESL_RADIO_STUB)
    list(APPEND srcs radio_stub.c)
else()
    list(APPEND srcs radio_ieee802154.c)
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    REQUIRES esl_core esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash esp_app_format ieee802154)
# mdns comes from idf_component.yml and is added to this component automatically.
