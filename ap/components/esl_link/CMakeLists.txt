# The UART link between the AP host and the 802.15.4 radio coprocessor (esl_link.h).
# Used by main/radio_uart.c and by the radio firmware in ap/radio/, and built on the
# host by test/Makefile, so nothing here may include ESP-IDF headers.
idf_component_register(SRCS esl_link.c INCLUDE_DIRS include)
