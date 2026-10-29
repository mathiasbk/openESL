# Platform-independent AP logic. Also built on the host by test/Makefile,
# so nothing here may include ESP-IDF headers.
idf_component_register(
    SRCS esl_api.c esl_barcode.c esl_clock.c esl_content.c esl_crc32.c esl_health.c esl_json.c esl_layout.c esl_layout_compile.c esl_persist.c esl_proto.c esl_radio.c esl_link.c esl_rcache.c esl_rhost.c esl_nlink.c esl_sched.c esl_source.c esl_source_met.c esl_tagfw.c esl_image.c esl_img2d.c esl_png.c
         esl_store.c esl_text.c esl_wifi.c esl_screen.c esl_render.c esl_qr.c
         ../../../tag/src/font.c ../../../tag/src/barcode.c
         ../../../tag/fonts/font_small.c ../../../tag/fonts/font_medium.c ../../../tag/fonts/font_mid.c ../../../tag/fonts/font_large.c
         ../../../tag/fonts/font_text20.c ../../../tag/fonts/font_price44.c
    INCLUDE_DIRS include ../../../shared
    # font.h, barcode.h: the tag's fonts and barcodes for the status screen and esl_render.c; private, since tag/src also has an app.h
    PRIV_INCLUDE_DIRS ../../../tag/src)
