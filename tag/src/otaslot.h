/*
 * otaslot.h - where firmware updates live in the tag's SPI flash (docs/tag-ota.md).
 * Shared by the application (ota.c) and the bootloader (boot/).
 *
 * Two slots of 64 KB, clear of the data the stock firmware left in the first sectors:
 *   OTA_SLOT_NEW  the downloaded image, waiting to be installed
 *   OTA_SLOT_OLD  a copy of the application that ran before it, for rollback
 * Each slot starts with the 16-byte header of the OTA file (ESL_FWF_* in shared/proto.h),
 * written last, and the image from OTA_DATA_OFF.
 *
 * The state is one 4 KB sector. Each byte is a flag: 0xFF (erased) = not set, anything
 * else = set; flags are only ever programmed from 0xFF to 0x00, so no flag needs an
 * erase, and the sector is erased when a new download starts.
 */
#ifndef OTASLOT_H
#define OTASLOT_H

#define OTA_SLOT_NEW        0x80000ul
#define OTA_SLOT_OLD        0x90000ul
#define OTA_STATE           0xA0000ul
#define OTA_DATA_OFF        0x100u      /* image data, one SPI page after the header */
#define OTA_SECTOR          0x1000u
#define OTA_PAGE            0x100u

#define OTA_ST_INSTALL      0   /* the application asks for the image in OTA_SLOT_NEW */
#define OTA_ST_COPIED       1   /* the bootloader has copied it into the chip */
#define OTA_ST_TRIES        2   /* 2..9: one byte per start of the new image before it was confirmed */
#define OTA_ST_TRIES_N      8
#define OTA_ST_CONFIRMED    16  /* the new image has talked to the AP */
#define OTA_ST_ROLLEDBACK   17  /* the new image was bad, or never confirmed: the old one runs */
#define OTA_ST_LEN          18

#endif /* OTASLOT_H */
