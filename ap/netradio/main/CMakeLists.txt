idf_component_register(
    SRCS netradio_main.c ../../main/radio_ieee802154.c
    INCLUDE_DIRS . ../../main
    REQUIRES esl_core ieee802154 esp_wifi esp_netif esp_event nvs_flash esp_timer lwip esp_coex esp_partition
             esp_driver_usb_serial_jtag vfs esp_app_format)
# mdns comes from idf_component.yml and is added to this component automatically.
