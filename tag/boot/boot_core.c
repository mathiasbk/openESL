/*
 * boot_core.c - see boot_core.h. Must fit the 1 KB page 0 together with boot_zbs243.c,
 * so it is plain: one page buffer, the CRC done while reading, and the little-endian
 * header fields read in place (the 8051 with SDCC and the PC are both little-endian).
 */
#include "boot_core.h"
#include "otaslot.h"
#include "proto.h"

#define HI(a) ((uint8_t)((a) >> 16))

uint8_t __xdata boot_buf[BOOT_BUF_LEN];
uint32_t boot_crc;
static uint8_t __xdata st[OTA_ST_LEN];
static uint8_t pages;          /* image size in 1 KB pages */
static uint32_t want;          /* its CRC-32 from the header */

void boot_crc_byte(uint8_t v)
{
#if defined(__SDCC)
    (void)v;
    __asm
        mov  a, dpl
        xrl  _boot_crc, a
        mov  r7, #8
    00001$:
        clr  c
        mov  a, (_boot_crc + 3)
        rrc  a
        mov  (_boot_crc + 3), a
        mov  a, (_boot_crc + 2)
        rrc  a
        mov  (_boot_crc + 2), a
        mov  a, (_boot_crc + 1)
        rrc  a
        mov  (_boot_crc + 1), a
        mov  a, _boot_crc
        rrc  a
        mov  _boot_crc, a
        jnc  00002$
        xrl  (_boot_crc + 3), #0xED
        xrl  (_boot_crc + 2), #0xB8
        xrl  (_boot_crc + 1), #0x83
        xrl  _boot_crc, #0x20
    00002$:
        djnz r7, 00001$
    __endasm;
#else
    uint8_t b;
    boot_crc ^= v;
    for (b = 0; b < 8; b++)
        boot_crc = (boot_crc & 1) ? (boot_crc >> 1) ^ 0xEDB88320ul : boot_crc >> 1;
#endif
}

/* Reads the image in the slot in block hi page by page; with write, also writes each
   page into the chip from ESL_FW_APP_BASE. True if its CRC matches the header. */
static bool pass(uint8_t hi, bool write)
{
    uint8_t p;

    boot_crc = 0xFFFFFFFFul;
    for (p = 0; p < pages; p++) {
        bl_spi_read(hi, OTA_DATA_OFF + ((uint16_t)p << 10), BOOT_BUF_LEN);
        if (write)
            bl_page_write((uint8_t)((ESL_FW_APP_BASE >> 10) + p));
    }
    return ~boot_crc == want;
}

/* The slot in block hi holds a whole image: header and CRC. */
static bool slot_ok(uint8_t hi)
{
    uint16_t size;

    bl_spi_read(hi, 0, ESL_FWF_HDR_LEN);
    size = *(uint16_t __xdata *)(boot_buf + ESL_FWF_OFF_SIZE);
    want = *(uint32_t __xdata *)(boot_buf + ESL_FWF_OFF_CRC);
    pages = (uint8_t)(size >> 10);
    if (*(uint32_t __xdata *)(boot_buf + ESL_FWF_OFF_MAGIC) != ESL_FWF_MAGIC ||
        *(uint16_t __xdata *)(boot_buf + ESL_FWF_OFF_SIZE + 2) != 0 ||
        (size & 0x3FF) || size == 0 || size > ESL_FW_MAX)
        return false;
    return pass(hi, false);
}

static void mark(uint8_t flag)
{
    bl_spi_mark(HI(OTA_STATE), (uint16_t)OTA_STATE + flag);
}

void boot_core_run(void)
{
    uint8_t i;

    bl_spi_read(HI(OTA_STATE), (uint16_t)OTA_STATE, OTA_ST_LEN);
    for (i = 0; i < OTA_ST_LEN; i++)
        st[i] = boot_buf[i];
    if (st[OTA_ST_INSTALL] == 0xFF || st[OTA_ST_ROLLEDBACK] != 0xFF)
        return;                                 /* nothing asked, or already rolled back */
    if (st[OTA_ST_COPIED] == 0xFF) {
        /* A power cut while copying lands here again and copies again */
        if (slot_ok(HI(OTA_SLOT_NEW))) {
            pass(HI(OTA_SLOT_NEW), true);
            mark(OTA_ST_COPIED);
        } else {
            mark(OTA_ST_ROLLEDBACK);            /* nothing was copied: the old image is still there */
        }
        return;
    }
    if (st[OTA_ST_CONFIRMED] != 0xFF)
        return;
    for (i = OTA_ST_TRIES; i < OTA_ST_TRIES + OTA_ST_TRIES_N; i++) {
        if (st[i] == 0xFF) {
            mark(i);                            /* one more start without a word from the AP */
            return;
        }
    }
    /* Started OTA_ST_TRIES_N times and never confirmed: back to the old image */
    if (slot_ok(HI(OTA_SLOT_OLD)))
        pass(HI(OTA_SLOT_OLD), true);
    mark(OTA_ST_ROLLEDBACK);
}
