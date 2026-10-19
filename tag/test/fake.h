/* fake.h - fake hardware for the host tests (radio, display, MAC, battery, flash store). */
#ifndef FAKE_H
#define FAKE_H

#include "compat.h"
#include "draw.h"

#define FAKE_MAX_FRAMES 16

/* Radio */
extern uint8_t fake_tx[FAKE_MAX_FRAMES][128];
extern uint8_t fake_tx_len[FAKE_MAX_FRAMES];
extern int fake_tx_count;
extern int fake_rx_calls;
void fake_rx_push(const uint8_t *frame, uint8_t len);   /* the next reply radio_rx returns */
void fake_rx_lose(void);                                /* the next radio_rx gets nothing */
void fake_radio_reset(void);
extern int fake_radio_on;

/* Identity and battery */
extern uint8_t fake_mac[8];
extern uint16_t fake_batt_mv;
extern unsigned fake_delay_ms_total;

/* Persistent store (hal_store_*): one record in RAM, kept over app_init() */
extern uint8_t fake_store[200];
extern uint8_t fake_store_len;        /* 0 = no record */
extern int fake_store_writes;
extern bool fake_store_radio_on_write; /* a write happened with the radio on (not allowed) */
/* Layout file store (hal_layout_store_*): one page, 0xFF when erased */
extern uint8_t fake_lstore[1024];
extern int fake_lstore_writes;
void fake_store_clear(void);          /* clears both stores */

/* SPI flash, the chip's flash and restarts (test/fake_xflash.c). Absent unless
   fake_xflash_clear(true): tags without the chip take no firmware updates. */
#define FAKE_XFLASH_SIZE 0x100000
extern uint8_t fake_xflash[FAKE_XFLASH_SIZE];
extern uint8_t fake_code[0x10000];
extern bool fake_xflash_present;
extern bool fake_xflash_is_open;
extern int fake_xflash_misuse;
extern int fake_resets;                    /* hal_reset() calls */
void fake_xflash_clear(bool present);      /* all 0xFF */

/* Display (test/fake_epd.c): epd_draw draws into a whole image. fake_draw_count counts
   the calls, fake_refresh_count the ones that changed the image (epd.c refreshes the
   panel only then, see epd_shown_crc). */
extern int fake_draw_count;
extern int fake_refresh_count;
extern bool fake_epd_ok;
extern uint8_t fake_fb[DRAW_H][DRAW_W];   /* 0 = white, 1 = black */
void fake_render(draw_scene_t *s);         /* draws the scene into fake_fb */
int fake_ink_count(int color);
bool fake_fb_save_ppm(const char *path);

#endif
