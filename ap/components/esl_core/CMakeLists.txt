# Platform-independent AP logic. Also built on the host by test/Makefile,
# so nothing here may include ESP-IDF headers.
idf_component_register(
    SRCS esl_api.c esl_content.c esl_crc32.c esl_json.c esl_layout.c esl_layout_compile.c esl_persist.c esl_proto.c esl_radio.c
         esl_store.c esl_text.c esl_wifi.c
    INCLUDE_DIRS include ../../../shared)
