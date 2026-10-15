/*
 * boot_core.h - the bootloader's decisions, without hardware code (docs/tag-ota.md).
 *
 * boot_core_run() looks at the update state in the SPI flash (src/otaslot.h) and, when
 * asked to, copies an image from a slot into the chip. It returns when the application
 * at ESL_FW_APP_BASE should run. The hardware functions below are in boot_zbs243.c on
 * the tag and in test/fake_xflash.c on the PC.
 *
 * Images are whole 1 KB pages (build.py pads them), and SPI flash addresses are split
 * into the 64 KB block (hi) and the offset in it (lo): every slot and the state fit in
 * one block. Both keep the 8051 code small enough for page 0.
 */
#ifndef BOOT_CORE_H
#define BOOT_CORE_H

#include "compat.h"

#define BOOT_BUF_LEN 1024     /* one chip flash page */

extern uint8_t __xdata boot_buf[BOOT_BUF_LEN];
extern uint32_t boot_crc;     /* CRC-32 (IEEE) register, updated by bl_spi_read */

/* Reads len bytes of the SPI flash at (hi << 16) + lo into boot_buf and runs them
   through boot_crc (boot_crc_byte). */
void bl_spi_read(uint8_t hi, uint16_t lo, uint16_t len);
/* Programs the byte at (hi << 16) + lo to 0x00 (sets an OTA_ST_* flag). */
void bl_spi_mark(uint8_t hi, uint16_t lo);
/* Erases chip flash page `page` and writes boot_buf (1024 bytes) to it. */
void bl_page_write(uint8_t page);

/* boot_crc = one CRC-32 step over v (reflected, polynomial 0xEDB88320, no final xor). */
void boot_crc_byte(uint8_t v);

void boot_core_run(void);

#endif /* BOOT_CORE_H */
