#include "ota.h"
#include "otaslot.h"
#include "config.h"
#include "model.h"
#include "hal/hal.h"

ota_state_t __xdata ota;

/* One SPI flash page; kept between wake-ups (XRAM above 0xE300 survives deep sleep),
   so a download goes on where it stopped. Bytes dl_got & 0xFF of it are filled. */
static uint8_t __xdata page[OTA_PAGE];
static const uint8_t __xdata zero = 0;

static void mark(uint8_t flag)
{
    hal_xflash_write(OTA_STATE + flag, &zero, 1);
}

void ota_init(void)
{
    uint8_t i;

    for (i = 0; i < sizeof(ota); i++)
        ((uint8_t __xdata *)&ota)[i] = 0;
    if (!hal_xflash_open())
        return;
    ota.present = 1;
    hal_xflash_read(OTA_STATE, page, OTA_ST_LEN);
    if (page[OTA_ST_INSTALL] != 0xFF) {
        i = page[OTA_ST_ROLLEDBACK] != 0xFF;
        ota.trial = !i && page[OTA_ST_COPIED] != 0xFF && page[OTA_ST_CONFIRMED] == 0xFF;
        hal_xflash_read(OTA_SLOT_NEW, page, ESL_FWF_HDR_LEN);
        ota.failed_ver = ESL_GET_U16(page + ESL_FWF_OFF_VER);
        /* Rolled back: that version failed. Just installed: it must be the version this
           code reports, or the AP would offer it again and again; if not, it counts as
           failed too (and the image stays, as it runs). */
        if (ota.failed_ver == FW_VERSION || (!i && !ota.trial))
            ota.failed_ver = 0;
    }
    hal_xflash_close();
}

uint8_t ota_poll_flags(void)
{
    uint8_t f = 0;
    if (ota.present)
        f |= ESL_POLL_F_OTA;
    if (ota.failed_ver)
        f |= ESL_POLL_F_FWFAIL;
    return f;
}

void ota_confirm(void)
{
    if (!ota.trial)
        return;
    ota.trial = 0;
    hal_xflash_open();
    mark(OTA_ST_CONFIRMED);
    hal_xflash_close();
}

void ota_open(void)
{
    hal_xflash_open();
}

void ota_close(void)
{
    hal_xflash_close();
}

/* CRC-32 of len bytes of the SPI flash from addr, read through page[]. */
static uint32_t xflash_crc(uint32_t addr, uint16_t len)
{
    uint32_t crc = 0;
    uint16_t n;

    while (len) {
        n = len < OTA_PAGE ? len : OTA_PAGE;
        hal_xflash_read(addr, page, n);
        crc = proto_crc32(crc, page, n);
        addr += n;
        len -= n;
    }
    return crc;
}

/* Writes the header of the OTA file (ESL_FWF_*) for an image to the slot at base. */
static bool write_header(uint32_t base, uint16_t ver, uint16_t size, uint32_t crc)
{
    uint8_t i;

    for (i = 0; i < ESL_FWF_HDR_LEN; i++)
        page[i] = 0;
    ESL_PUT_U32(page + ESL_FWF_OFF_MAGIC, ESL_FWF_MAGIC);
    page[ESL_FWF_OFF_FORMAT] = ESL_FWF_FORMAT;
    page[ESL_FWF_OFF_MODEL] = MODEL_CODE;
    ESL_PUT_U16(page + ESL_FWF_OFF_VER, ver);
    ESL_PUT_U32(page + ESL_FWF_OFF_SIZE, (uint32_t)size);
    ESL_PUT_U32(page + ESL_FWF_OFF_CRC, crc);
    return hal_xflash_write(base, page, ESL_FWF_HDR_LEN);
}

/* Programs page[] (filled up to n bytes) as the image page that starts at pos. */
static bool flush(uint16_t pos, uint16_t n)
{
    uint32_t a = OTA_SLOT_NEW + OTA_DATA_OFF + pos;

    if ((a & (OTA_SECTOR - 1)) == 0 && !hal_xflash_erase(a))
        return false;
    return hal_xflash_write(a, page, n);
}

static uint8_t fail(void)
{
    ota.failed_ver = ota.dl_ver;    /* reported, so the AP stops offering it */
    ota.dl_ver = 0;
    return OTA_STOP;
}

uint8_t ota_block(const proto_reply_t *r, const uint8_t *d)
{
    uint8_t i, n;
    uint16_t pos;

    if (!ota.present || r->fw_model != MODEL_CODE || r->fw_ver == FW_VERSION || r->fw_ver == 0 ||
        r->fw_ver == ota.failed_ver)
        return OTA_STOP;
    if (r->fw_ver != ota.dl_ver || r->lay_size != ota.dl_size || r->fw_crc != ota.dl_crc) {
        /* A new image (or it changed on the AP): start from the beginning. The bootloader
           copies whole pages, so the size is a whole number of them. */
        if (r->lay_size == 0 || r->lay_size > ESL_FW_MAX || (r->lay_size & 0x3FF))
            return OTA_STOP;
        ota.dl_ver = r->fw_ver;
        ota.dl_size = r->lay_size;
        ota.dl_crc = r->fw_crc;
        ota.dl_got = 0;
        ota.failed_ver = 0;
        /* The state goes first: an install or rollback recorded there is over */
        if (!hal_xflash_erase(OTA_STATE) || !hal_xflash_erase(OTA_SLOT_NEW))
            return fail();
    }
    pos = r->lay_pos;
    n = r->field_len[0];
    if (pos > ota.dl_got || pos + n <= ota.dl_got)
        return OTA_MORE;                       /* not the block we asked for */
    for (i = (uint8_t)(ota.dl_got - pos); i < n; i++) {
        page[ota.dl_got & (OTA_PAGE - 1)] = d[i];
        ota.dl_got++;
        if ((ota.dl_got & (OTA_PAGE - 1)) == 0 && !flush(ota.dl_got - OTA_PAGE, OTA_PAGE))
            return fail();
    }
    if (ota.dl_got < ota.dl_size)
        return OTA_MORE;
    /* Complete (the size is whole pages, so the last one is written). Check what the
       flash holds, then write the header, which marks the slot as whole. */
    if (xflash_crc(OTA_SLOT_NEW + OTA_DATA_OFF, ota.dl_size) != ota.dl_crc ||
        !write_header(OTA_SLOT_NEW, ota.dl_ver, ota.dl_size, ota.dl_crc))
        return fail();
    return OTA_DONE;
}

void ota_install(void)
{
    uint16_t pos;
    uint32_t crc = 0;

    hal_xflash_open();
    /* The running application, for the bootloader's rollback */
    for (pos = 0; pos < ESL_FW_MAX; pos += OTA_PAGE) {
        hal_wdt_kick();
        /* the first sector holds the header too */
        if ((pos == 0 || ((OTA_DATA_OFF + pos) & (OTA_SECTOR - 1)) == 0) &&
            !hal_xflash_erase(OTA_SLOT_OLD + ((OTA_DATA_OFF + pos) & ~(OTA_SECTOR - 1))))
            goto out;
        hal_code_read(ESL_FW_APP_BASE + pos, page, OTA_PAGE);
        crc = proto_crc32(crc, page, OTA_PAGE);
        if (!hal_xflash_write(OTA_SLOT_OLD + OTA_DATA_OFF + pos, page, OTA_PAGE))
            goto out;
    }
    if (!write_header(OTA_SLOT_OLD, FW_VERSION, ESL_FW_MAX, crc))
        goto out;
    mark(OTA_ST_INSTALL);
    hal_xflash_close();
    hal_reset();
    return;
out:
    hal_xflash_close();
    fail();
}
