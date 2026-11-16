/*
 * ssd16xx-29.h - panel parameters for the SoluM ST-GM29XX2 (2.9", 296 x 128).
 *
 * Selected through "panel" in shared/models.json (MODEL_PANEL_H in model_gen.h) and
 * included by model.h; epd.c is the driver and reads only these macros. The controller
 * is in the SSD1680 family (UNCONFIRMED, see epd.c).
 *
 * One layout column per RAM line (Y = gate line). With the gates scanned from G0 the
 * text came out mirrored left to right (seen on a 2.9" tag 2026-10-17), so the gates
 * are scanned from the last line back to G0 instead (EPD_GATE_SCAN, the TB bit of
 * driver output control 0x01).
 */
#ifndef PANEL_SSD16XX_29_H
#define PANEL_SSD16XX_29_H

#define EPD_TRANSPOSE       0
#define EPD_ROTATE_180      0
#define EPD_DATA_ENTRY      0x03   /* X+ and Y+, X first */
#define EPD_GATE_SCAN       0x01   /* TB = 1: G295 to G0 */
#define EPD_UPDATE_SEQ      0xF7   /* full update with the OTP waveform */
#define EPD_WRITE_TEMP      0      /* no 0x1A before the update */

/* Sent after driver output control (0x01): command, byte count, bytes. EPD_OP_WINDOW
 * (count 0) is where epd.c sets the data entry mode and the RAM window. */
#define EPD_INIT \
    0x3C, 1, 0x05,          /* border waveform */ \
    0x18, 1, 0x80,          /* internal temperature sensor */ \
    EPD_OP_WINDOW, 0

/*
 * Some ST-GM29XX2 boards (seen on tags made in 2017) carry a UC-type controller (UC8151
 * family) instead; epd.c tells them apart by BUSY and then sends this list instead of the
 * one above. Command, byte count, bytes; count 0 waits for BUSY (power on). The values
 * are what the stock firmware sends to a UC-type panel on this tag.
 */
#define EPD_UC_INIT \
    0x01, 2, 0x03, 0x01,            /* power setting */ \
    0x06, 3, 0x17, 0x17, 0x17,      /* booster soft start */ \
    0x04, 0,                        /* power on */ \
    0x00, 2, 0x97, 0x0B,            /* panel setting: 128 x 296, OTP waveform, black/white */ \
    0x61, 3, 0x80, 0x01, 0x28,      /* resolution: 128 sources, 296 gates */ \
    0x82, 1, 0x12,                  /* VCOM DC */ \
    0x50, 1, 0x97                   /* VCOM and data interval */

#endif /* PANEL_SSD16XX_29_H */
