/*
 * ssd16xx-154.h - panel parameters for the SoluM ST-GM16XX2 (1.54", 200 x 200).
 *
 * Selected through "panel" in shared/models.json (MODEL_PANEL_H in model_gen.h) and
 * included by model.h; epd.c is the driver and reads only these macros. The values
 * follow the stock firmware (docs/st-gm16xx2.md); UNCONFIRMED that it is an SSD16xx at all.
 *
 * On this panel a RAM line (Y) is a screen row, so one layout column cannot be one RAM
 * line: that gave a transposed image (rotated and mirrored, seen on the tag 2026-10-14).
 * Instead 8 layout columns are transposed into one RAM byte column (X), and the counter
 * runs down Y first. RAM (0, 0) is the bottom right corner when the tag is held the
 * right way up, so the image is also turned 180 degrees (it was upside down on the tag
 * 2026-10-16).
 */
#ifndef PANEL_SSD16XX_154_H
#define PANEL_SSD16XX_154_H

#define EPD_TRANSPOSE       1
#define EPD_ROTATE_180      1
#define EPD_DATA_ENTRY      0x07   /* X+ and Y+, Y first */
#define EPD_GATE_SCAN       0x00   /* TB = 0: G0 first */
#define EPD_UPDATE_SEQ      0xD7   /* stock firmware, with the temperature from 0x1A */
#define EPD_WRITE_TEMP      1      /* 0x1A with EPD_TEMP_C before the update */
#define EPD_TEMP_C          25     /* TODO phase 6: the ZBS243 temperature sensor */
#define EPD_SLEEP_CTRL1     0x03   /* 0x21 before deep sleep, as the stock firmware does */

/* Sent after driver output control (0x01): command, byte count, bytes. EPD_OP_WINDOW
 * (count 0) is where epd.c sets the data entry mode and the RAM window. */
#define EPD_INIT \
    0x3A, 1, 0x1B,          /* dummy line period */ \
    0x3B, 1, 0x0B,          /* gate line width */ \
    EPD_OP_WINDOW, 0, \
    0x3C, 1, 0x33,          /* border waveform */ \
    0x0C, 3, 0xC7, 0x8F, 0x8F, /* booster soft start */ \
    0x21, 1, 0x83           /* display update control 1 */

/*
 * Some ST-GM16XX2 boards (seen on a tag made in 2017) carry a UC-type controller instead,
 * which takes two bits per pixel (epd.c); epd.c tells it apart by BUSY and then sends this
 * list instead of the one above. Command, byte count, bytes; count 0 waits for BUSY (power
 * on). See docs/st-gm16xx2.md.
 */
#define EPD_UC_INIT \
    0x01, 2, 0x03, 0x00,            /* power setting */ \
    0x06, 3, 0x07, 0x06, 0x05,      /* booster soft start */ \
    0x04, 0,                        /* power on */ \
    0x50, 1, 0x07,                  /* VCOM and data interval */ \
    0x30, 1, 0x39,                  /* PLL */ \
    0x61, 3, 0xC8, 0x00, 0xC8,      /* resolution: 200 sources, 200 gates */ \
    0x82, 1, 0x0A,                  /* VCOM DC */ \
    0x00, 1, 0x17,                  /* panel setting */ \
    /* This controller has no waveform of its own: the LUTs for 20-30 degrees C */ \
    0x20, 15, 0x0F, 0x0F, 0x01, 0x19, 0x19, 0x05, 0x03, 0x03, 0x0A, 0, 0, 0, 0, 0, 0, /* VCOM */ \
    0x21, 15, 0x8F, 0x8F, 0x01, 0x99, 0x59, 0x05, 0x03, 0x43, 0x0A, 0, 0, 0, 0, 0, 0, /* white */ \
    0x22, 15, 0x4F, 0x4F, 0x01, 0x99, 0x59, 0x05, 0x83, 0x03, 0x0A, 0, 0, 0, 0, 0, 0  /* black */

#endif /* PANEL_SSD16XX_154_H */
