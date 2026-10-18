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

#endif /* PANEL_SSD16XX_154_H */
