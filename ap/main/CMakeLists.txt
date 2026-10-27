set(srcs app_main.c http_api.c sched.c store_nvs.c tagfw.c wifi.c)

# The radio task (radio.c) and the driver of the link it talks through (esl_link.h): 802.15.4
# on this chip, or a radio coprocessor on UART (boards/s3-n16r8-c6). A radio over WiFi
# (boards/s3-n16r8-netradio) answers the tags itself and has radio_net.c instead. REQUIRES
# below lists the components of every driver: it is read before sdkconfig is.
if(CONFIG_ESL_RADIO_STUB)
    list(APPEND srcs radio_stub.c)
elseif(CONFIG_ESL_RADIO_DRIVER_NET)
    list(APPEND srcs radio_net.c radio_tx.c)
else()
    list(APPEND srcs radio.c radio_tx.c)
    if(CONFIG_ESL_RADIO_DRIVER_IEEE802154)
        list(APPEND srcs radio_ieee802154.c)
    elseif(CONFIG_ESL_RADIO_DRIVER_UART)
        list(APPEND srcs radio_uart.c)
    elseif(NOT CMAKE_BUILD_EARLY_EXPANSION)  # no sdkconfig yet in that pass
        message(FATAL_ERROR "No radio driver for ${IDF_TARGET}: choose one under \"Radio driver\" in menuconfig, "
                            "or the stub radio mode (ESL_RADIO_STUB)")
    endif()
endif()

if(CONFIG_ESL_SERIAL_API)
    list(APPEND srcs serial_api.c)
endif()

if(CONFIG_ESL_DISPLAY OR CONFIG_ESL_RADIO_DRIVER_UART)
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

if(CONFIG_ESL_WEB_DASHBOARD)
    # The web page's tabs this board has room for, packed from web/ (main/webui.py)
    set(webui_c ${CMAKE_CURRENT_BINARY_DIR}/webui_files.c)
    set(web_dir ${CMAKE_CURRENT_LIST_DIR}/../../web)
    set(webui_modules dashboard)
    set(webui_wasm)
    if(CONFIG_ESL_WEB_DESIGNER)
        # The Design tab packs the renderer too: python web/build.py --wasm (Emscripten) builds it
        # into web/dist/wasm/, or OPENESL_WASM names another directory
        set(wasm_dir ${web_dir}/dist/wasm)
        if(DEFINED ENV{OPENESL_WASM})
            set(wasm_dir $ENV{OPENESL_WASM})
        endif()
        set(webui_wasm --wasm ${wasm_dir})
        list(APPEND webui_modules design)
    endif()
    idf_build_get_property(python PYTHON)
    file(GLOB webui_deps ${web_dir}/index.html ${web_dir}/css/*.css ${web_dir}/js/*.js
         ${CMAKE_CURRENT_LIST_DIR}/../../tag/designs/layouts/*.json ${CMAKE_CURRENT_LIST_DIR}/../../shared/models.json)
    if(CONFIG_ESL_WEB_DESIGNER)
        # the renderer files and what they are built from: a change to either runs webui.py again,
        # which stops the build when the renderer is older than its sources
        execute_process(COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/webui.py --renderer-deps ${wasm_dir}
                        OUTPUT_VARIABLE wasm_deps OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        string(REPLACE "\n" ";" wasm_deps "${wasm_deps}")
        list(APPEND webui_deps ${wasm_deps})
    endif()
    add_custom_command(
        OUTPUT ${webui_c}
        COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/webui.py ${web_dir} ${webui_c} ${webui_wasm} ${webui_modules}
        DEPENDS ${CMAKE_CURRENT_LIST_DIR}/webui.py ${webui_deps}
        VERBATIM)
    list(APPEND srcs ${webui_c})
endif()

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS .
    REQUIRES esl_core esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash esp_app_format ieee802154 esl_coproc esl_lcd esp_driver_uart
             esp_driver_usb_serial_jtag vfs esp_lcd esp_driver_spi esp_driver_gpio esp_http_client app_update lwip esp_partition)
# mdns comes from idf_component.yml and is added to this component automatically.
