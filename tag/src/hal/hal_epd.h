/*
 * hal_epd.h - the pins between ZBS243 and the display's FPC connector.
 *
 * Which ZBS243 pins these are has not been measured yet (phase 0, multimeter from
 * chip to FPC). Until then the implementation in hal/zbs243/hal_epd.c is empty.
 * SPI is bit-banged in epd.c (tag-firmware.md: "SPI to the display can be bit-banged").
 */
#ifndef HAL_EPD_H
#define HAL_EPD_H

#include "../compat.h"

void hal_epd_power(bool on);   /* power to the display, if it can be switched off */
void hal_epd_cs(bool high);
void hal_epd_dc(bool high);    /* low = command, high = data */
void hal_epd_rst(bool high);
void hal_epd_sck(bool high);
void hal_epd_mosi(bool high);
bool hal_epd_busy(void);       /* true = the display is busy */

#endif /* HAL_EPD_H */
