/* fake_xflash.c - the SPI flash, the chip's own flash and the restart for the PC (hal.h),
 * and the bootloader's hardware functions (boot/boot_core.h) over the same memory, so the
 * tests and the simulator can run a whole update: download, install, boot, rollback. */
#include <string.h>
#include "hal/hal.h"
#include "boot_core.h"
#include "fake.h"

uint8_t fake_xflash[FAKE_XFLASH_SIZE];
uint8_t fake_code[0x10000];
bool fake_xflash_present;
bool fake_xflash_is_open;
int fake_xflash_misuse;      /* access while closed, or a write across a page */
int fake_resets;

void fake_xflash_clear(bool present)
{
    memset(fake_xflash, 0xFF, sizeof(fake_xflash));
    fake_xflash_present = present;
    fake_xflash_is_open = false;
    fake_xflash_misuse = 0;
    fake_resets = 0;
}

bool hal_xflash_open(void)
{
    fake_xflash_is_open = fake_xflash_present;
    return fake_xflash_present;
}

void hal_xflash_close(void)
{
    fake_xflash_is_open = false;
}

void hal_xflash_read(uint32_t addr, uint8_t *buf, uint16_t len)
{
    if (!fake_xflash_is_open || addr + len > FAKE_XFLASH_SIZE) {
        fake_xflash_misuse++;
        memset(buf, 0xFF, len);
        return;
    }
    memcpy(buf, fake_xflash + addr, len);
}

bool hal_xflash_erase(uint32_t addr)
{
    if (!fake_xflash_is_open || addr >= FAKE_XFLASH_SIZE) {
        fake_xflash_misuse++;
        return false;
    }
    memset(fake_xflash + (addr & ~0xFFFul), 0xFF, 0x1000);
    return true;
}

bool hal_xflash_write(uint32_t addr, const uint8_t *buf, uint16_t len)
{
    if (!fake_xflash_is_open || len == 0 || (addr & 0xFF) + len > 0x100 || addr + len > FAKE_XFLASH_SIZE) {
        fake_xflash_misuse++;
        return false;
    }
    for (uint16_t i = 0; i < len; i++)
        fake_xflash[addr + i] &= buf[i];     /* programming only clears bits */
    return true;
}

void hal_code_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    memcpy(buf, fake_code + addr, len);
}

void hal_reset(void)
{
    fake_resets++;
}

/* ---- boot_core.h ---- */

void bl_spi_read(uint8_t hi, uint16_t lo, uint16_t len)
{
    uint32_t a = ((uint32_t)hi << 16) | lo;
    for (uint16_t i = 0; i < len; i++) {
        boot_buf[i] = fake_xflash[a + i];
        boot_crc_byte(boot_buf[i]);
    }
}

void bl_spi_mark(uint8_t hi, uint16_t lo)
{
    fake_xflash[((uint32_t)hi << 16) | lo] = 0;
}

void bl_page_write(uint8_t page)
{
    memcpy(fake_code + (uint32_t)page * 1024, boot_buf, 1024);
}
