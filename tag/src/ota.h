/*
 * ota.h - firmware updates over the radio (docs/tag-ota.md, requirement T15).
 *
 * The AP sends the new application image in FW blocks; ota_block() writes them to the
 * SPI flash (src/otaslot.h) and checks the whole image. ota_install() then copies the
 * running application to the rollback slot, asks the bootloader to install the new one
 * and restarts. The first answered POLL after that confirms the new image (ota_confirm);
 * without that, the bootloader goes back to the old one after a few restarts.
 */
#ifndef OTA_H
#define OTA_H

#include "compat.h"
#include "proto_tag.h"

#define OTA_MORE   0     /* ask for the block at ota.dl_got */
#define OTA_DONE   1     /* the image is complete and checked: ota_install() */
#define OTA_STOP   2     /* not for this tag, or the download failed */

typedef struct {
    uint8_t present;        /* the SPI flash answered at start-up */
    uint8_t trial;          /* this image was just installed and has not talked to the AP yet */
    uint16_t failed_ver;    /* a version that was rejected or rolled back (ESL_POLL_F_FWFAIL), 0 = none */
    uint16_t dl_ver;        /* download in progress, 0 = none */
    uint16_t dl_size;
    uint16_t dl_got;
    uint32_t dl_crc;
} ota_state_t;

extern ota_state_t __xdata ota;

/* Reads the update state from the SPI flash. Called once at start-up. */
void ota_init(void);
/* ESL_POLL_F_OTA and ESL_POLL_F_FWFAIL as they apply now. */
uint8_t ota_poll_flags(void);
/* The AP answered: if this is a new image, keep it. Radio off. */
void ota_confirm(void);

/* A burst of FW blocks: ota_open() before the first, ota_close() after the last. */
void ota_open(void);
void ota_close(void);
/* One FW block, parsed into r, with its data at d. Returns OTA_MORE, OTA_DONE or OTA_STOP. */
uint8_t ota_block(const proto_reply_t *r, const uint8_t *d);
/* After OTA_DONE, radio off: keeps the old image for rollback, asks the bootloader to
   install the new one and restarts. Returns only if that failed (or on the PC). */
void ota_install(void);

#endif /* OTA_H */
