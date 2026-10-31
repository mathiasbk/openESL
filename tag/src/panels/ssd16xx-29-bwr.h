/*
 * ssd16xx-29-bwr.h - panel parameters for the SoluM ST-GR29000 (2.9", 296 x 128,
 * black/white/red), issue #255.
 *
 * Selected through "panel" in shared/models.json (MODEL_PANEL_H in model_gen.h) and
 * included by model.h; epd.c is the driver and reads only these macros.
 *
 * UNCONFIRMED, not yet tried on a label: these are the values of the black/white
 * ST-GM29XX2 (ssd16xx-29.h), on the assumption that the red label has a controller in
 * the same SSD1680 family on the same kind of board. With MODEL_HAS_RED = 1, epd.c also
 * writes the red RAM (0x26, bit = 1 is red, not inverted) after the black one (0x24).
 * Check on the board before trusting them, since the same product code can come with
 * different panels:
 *   - the controller and the panel's FPC marking (if it is not an SSD16xx, epd.c needs
 *     another driver, not just this header);
 *   - that the image is not mirrored or turned (EPD_GATE_SCAN, EPD_DATA_ENTRY), as on
 *     the black/white 2.9" (2026-10-17);
 *   - that red shows as red and not as black or white (polarity of 0x26, and whether
 *     display update control 1, 0x21, must be sent to use the red RAM);
 *   - how long the full update with red takes (the OTP waveform; epd.c waits up to
 *     EPD_BUSY_TIMEOUT_MS, 30 s) and what it costs in battery (tools/power-budget).
 */
#ifndef PANEL_SSD16XX_29_BWR_H
#define PANEL_SSD16XX_29_BWR_H

#define EPD_TRANSPOSE       0
#define EPD_ROTATE_180      0
#define EPD_DATA_ENTRY      0x03   /* X+ and Y+, X first */
#define EPD_GATE_SCAN       0x01   /* TB = 1: G295 to G0, as on the black/white 2.9" */
#define EPD_UPDATE_SEQ      0xF7   /* full update with the OTP waveform (black, white and red) */
#define EPD_WRITE_TEMP      0      /* no 0x1A before the update */

/* Sent after driver output control (0x01): command, byte count, bytes. EPD_OP_WINDOW
 * (count 0) is where epd.c sets the data entry mode and the RAM window. */
#define EPD_INIT \
    0x3C, 1, 0x05,          /* border waveform */ \
    0x18, 1, 0x80,          /* internal temperature sensor */ \
    EPD_OP_WINDOW, 0

#endif /* PANEL_SSD16XX_29_BWR_H */
