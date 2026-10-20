/*
 * image.h - screen images drawn by the AP (docs/protokoll.md part 1, "Image transfer").
 *
 * The AP sends the picture in IMG blocks; image_block() writes them to the SPI flash
 * (one slot after the firmware slots in otaslot.h) and checks the whole image against
 * its id, a CRC-32. image_show() then streams the columns from the flash to the panel,
 * through epd_draw_image() and image_column(); the ZBS243 has no RAM for a whole
 * picture. The slot keeps the image over a restart, so the tag shows it again and
 * reports its id without a new download.
 */
#ifndef IMAGE_H
#define IMAGE_H

#include "compat.h"
#include "proto_tag.h"
#include "model.h"
#include "draw.h"

#define IMAGE_MORE   0     /* ask for the block at image.dl_got */
#define IMAGE_DONE   1     /* the image is whole and checked (or already in the slot): image_show() */
#define IMAGE_STOP   2     /* refused, or the download failed */

/* The slot: a 16-byte header (written last, so a half-downloaded image has none) and
   the image from IMG_DATA_OFF. 64 KB, like the firmware slots. */
#define IMG_SLOT            0xB0000ul
#define IMG_DATA_OFF        0x100u
#define IMG_HDR_LEN         16
#define IMG_HDR_MAGIC       0x4D49534Fu   /* "OSIM" */
#define IMG_HDR_FORMAT      1
#define IMG_HDR_OFF_MAGIC   0             /* u32 */
#define IMG_HDR_OFF_FORMAT  4             /* u8 = IMG_HDR_FORMAT */
#define IMG_HDR_OFF_MODEL   5             /* u8 MODEL_CODE */
#define IMG_HDR_OFF_FMT     6             /* u8 ESL_IMG_FMT_* */
#define IMG_HDR_OFF_SIZE    8             /* u16 bytes from IMG_DATA_OFF */
#define IMG_HDR_OFF_ID      12            /* u32 image_id = CRC-32 of those bytes */

/* A raw image (ESL_IMG_FMT_RAW): one column after the other, DRAW_COL_BYTES each, the
   black plane and then the red plane on a model with red. A coded one
   (ESL_IMG_FMT_RLE) is shorter; it is decoded once it is whole and checked, to
   IMG_RAW_OFF, before the header is written, and shown from there. */
#define IMG_RAW_SIZE        ((uint16_t)DRAW_W * DRAW_COL_BYTES * (1 + MODEL_HAS_RED))
#define IMG_RAW_OFF         0x2000u       /* a 4 KB sector past the largest download */

typedef struct {
    uint8_t present;        /* the SPI flash is there (ota.present) */
    uint32_t slot_id;       /* id of the whole, checked image in the slot, 0 = none */
    uint32_t rejected;      /* an id this tag refused or could not show, 0 = none */
    uint32_t dl_id;         /* download in progress, 0 = none */
    uint16_t dl_size;
    uint16_t dl_got;
    uint8_t dl_fmt;
    uint8_t slot_fmt;       /* ESL_IMG_FMT_* of the image in the slot */
} image_state_t;

extern image_state_t __xdata image;

/* Reads the slot header and checks the image. Called once at start-up, after ota_init(). */
void image_init(uint8_t present);
/* ESL_POLL_F_IMAGE | ESL_POLL_F_IMG_RLE if the tag takes images. */
uint8_t image_poll_flags(void);

/* A burst of IMG blocks: image_open() before the first, image_close() after the last. */
void image_open(void);
void image_close(void);
/* One IMG block, parsed into r, with its data at d. Returns IMAGE_MORE, IMAGE_DONE or IMAGE_STOP. */
uint8_t image_block(const proto_reply_t *r, const uint8_t *d);

/* Radio off: streams the image in the slot to the panel. False if there is none, or
   the panel did not answer. */
bool image_show(void);
/* Something else was drawn over the image: the slot no longer says what the screen
   shows, so it is not shown again at start-up. Radio off. */
void image_forget(void);

/* Column x of the image in the slot, for epd.c (the flash must be open). On a model
   without red, red is cleared. */
void image_column(uint16_t x, uint8_t __xdata *black, uint8_t __xdata *red);

#endif /* IMAGE_H */
