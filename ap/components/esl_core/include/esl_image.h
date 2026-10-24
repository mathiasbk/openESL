#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_layout.h"
#include "esl_store.h"

/* Screen images for tags that show what the AP draws (POLL bit5, ESL_POLL_F_IMAGE;
 * docs/protokoll.md part 1, "Image transfer").
 *
 * Nothing is stored: the AP has the source of every image (the tag's content and the
 * layout file for its model, or the picture a client sent, below), so the image is
 * drawn (or read back) when a tag needs it and dropped again. A few buffers hold the images being sent: a tag polls with another image id
 * than the one its content gives, the AP draws the image into a free buffer and sends
 * it block by block (IMG / IMG_REQ) in that wake-up; the buffer is free again when the
 * tag reports the id, once its last block has gone out, or ESL_IMAGE_HOLD_MS after the
 * last block it asked for (a tag fetches the blocks back to back, so a pause that long
 * means it gave up). When every buffer is busy with another tag, the tag is told to
 * come back in a couple of seconds.
 *
 * The id of an image is the CRC-32 of its bytes (esl_render draws the same bytes for
 * the same content), so a tag that shows an image is never sent it again, also after
 * an AP restart; the store keeps the id with the tag once it has been drawn, with the
 * version of the layout file it was drawn with: a new file for the model draws it again.
 *
 * Every public function takes the lock; drawing (under it) takes the layouts lock too,
 * so the two must not be the same lock as the store's. */

#define ESL_IMAGE_BUFS    2
#define ESL_IMAGE_HOLD_MS 5000u     /* a buffer stays with its tag this long after a block */
#define ESL_IMAGE_BUSY_S  2         /* next_poll_s for a tag that must wait for a buffer */

/* ESL_IMG_FMT_RLE: n bytes coded into out, at most max bytes; 0 when it does not fit.
 * out may be in - ESL_IMG_RLE_MARGIN(n) (coding in place: the output never overtakes
 * the input by more than that, and gives 0 rather than write over input it has not read).
 * out NULL only counts: the length the code would have. */
#define ESL_IMG_RLE_MARGIN(n) ((n) / 128 + 2)
size_t esl_img_rle_encode(const uint8_t *in, size_t n, uint8_t *out, size_t max);

/* ESL_IMG_FMT_RLE back to raw: in (n bytes) into out, which takes raw bytes; the
 * length, which must be raw, else 0. out may be before in in the same buffer as long as
 * in starts at least ESL_IMG_RLE_MARGIN(raw) + raw - n bytes after out (decoding in
 * place: the output never overtakes input it has not read; 0 rather than doing so). */
size_t esl_img_rle_decode(const uint8_t *in, size_t n, uint8_t *out, size_t raw);

/* ---- pictures from clients (PUT /api/tags/{mac}/image) -------------------------
 *
 * A PNG of the screen's size, or the image bytes as esl_render draws them, becomes the
 * tag's image: each pixel goes to the nearest colour the model has (white, black, and
 * red on a model with red), or with dither, Floyd-Steinberg error diffusion between
 * them, for photos. It is kept in NVS in a stored form: a 10-byte header (the MAC,
 * LE, the model and ESL_IMG_FMT_*) and the image, run-length coded when that is
 * shorter, so what a tag is sent needs no coding. */

#define ESL_IMAGE_STORED_HDR 10
/* The largest stored form for a model */
#define ESL_IMAGE_STORED_MAX(raw) (ESL_IMAGE_STORED_HDR + (raw))

typedef enum {
    ESL_PIC_OK = 0,
    ESL_PIC_ERR_READ,      /* the body ended early */
    ESL_PIC_ERR_FORMAT,    /* not a PNG, and not the raw image's size */
    ESL_PIC_ERR_BAD,       /* a broken PNG file */
    ESL_PIC_ERR_UNSUPPORTED,
    ESL_PIC_ERR_SIZE,      /* not the screen's size */
    ESL_PIC_ERR_NOMEM,
    ESL_PIC_ERR_BOTH,      /* a raw image with a pixel set in both the black and the red plane */
} esl_pic_err_t;

/* Reads a picture of len bytes for a tag of model through read (exactly n bytes, or
 * false) into out, esl_render_size(model) bytes in esl_render's format. */
esl_pic_err_t esl_image_picture(uint8_t model, bool dither, size_t len, bool (*read)(void *ctx, uint8_t *buf, size_t n),
                                void *ctx, uint8_t *out);

/* The stored form of raw (esl_render_size(model) bytes) for mac into out
 * (ESL_IMAGE_STORED_MAX bytes). Returns its length. */
size_t esl_image_pack(uint64_t mac, uint8_t model, const uint8_t *raw, uint8_t *out);

/* A stored form (len bytes at the start of buf, cap bytes) back to an image at the
 * start of buf: coded (*fmt ESL_IMG_FMT_RLE) when it is stored so and rle is set, else
 * raw. cap must be at least ESL_IMAGE_STORED_MAX + ESL_IMG_RLE_MARGIN of the raw size.
 * Returns the image's length, 0 when the stored form is broken or not for mac and model. */
size_t esl_image_unpack(uint64_t mac, uint8_t model, bool rle, uint8_t *buf, size_t len, size_t cap, uint8_t *fmt);

/* The screen of a tag with content c (raw, as esl_render draws it): drawn, or its stored
 * picture. out holds cap bytes: esl_render_size(model) for drawn content, more for a
 * picture (esl_image_unpack). Returns the length, 0 when it cannot be had. */
size_t esl_image_screen(esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint8_t model, const esl_content_t *c,
                        uint8_t *out, size_t cap);

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} esl_image_ops_t;

typedef struct {
    uint64_t mac;         /* 0 = free */
    uint32_t content_id;  /* the content the image was drawn from */
    uint32_t layout_ver;  /* with this version of the model's layout file (0: none) */
    uint32_t id;          /* image id (CRC-32 of data) */
    uint16_t size;        /* bytes as sent */
    uint8_t model;
    uint8_t fmt;          /* ESL_IMG_FMT_*: 2D or RLE for a tag that takes it, when that is shorter */
    uint64_t used_ms;     /* last block sent, or drawn */
    bool sent;            /* the last block went out: free for another tag */
    uint8_t *data;        /* cap bytes */
} esl_image_buf_t;

typedef struct {
    esl_image_buf_t buf[ESL_IMAGE_BUFS];
    size_t cap;           /* bytes per buffer: the largest esl_render_size() and its margin */
    esl_image_ops_t ops;
    uint32_t drawn;       /* images drawn (for the log and the tests) */
} esl_image_t;

/* Bytes the buffers need: ESL_IMAGE_BUFS times the largest image of any model, plus the
   room to code it in place, or to read its stored picture and decode it in place. */
size_t esl_image_mem_size(void);
/* mem holds esl_image_mem_size() bytes. ops may be NULL (no locking). */
void esl_image_init(esl_image_t *im, uint8_t *mem, const esl_image_ops_t *ops);

/* The image a tag should show, drawn into a buffer unless it holds it already:
 * returns its id, 0 when the tag has no content it can be drawn for (no content, or
 * content held back: esl_tag_t content_err). The id is recorded with the tag
 * (esl_store_image_known). shown is the id the tag reports: when it is the tag's
 * image, nothing is drawn and the tag's buffer, if any, is released. *busy is set
 * when no buffer is free: the tag should poll again in ESL_IMAGE_BUSY_S. */
uint32_t esl_image_prepare(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint32_t shown,
                           uint64_t now_ms, bool *busy);

/* The IMG reply for a tag that asks for the block at pos of image id (IMG_REQ), or
 * pos 0 after a POLL. When the tag's buffer holds another image (its content changed
 * meanwhile), block 0 of that one goes out, so the tag starts over; when the tag has
 * no buffer, the image is drawn first (as esl_image_prepare), and the reply is IDLE
 * when it has no content or every buffer is busy. out holds ESL_MAX_PAYLOAD bytes. */
size_t esl_image_block(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint32_t id,
                       uint16_t pos, uint8_t max, uint16_t next_poll_s, uint64_t now_ms, uint8_t *out);

/* The tag's image drawn into out (cap bytes, at least esl_image_mem_size() /
 * ESL_IMAGE_BUFS) instead of a buffer, for a radio that keeps its own copy (esl_rhost):
 * returns its length (0 when the tag has no content it can be drawn for) with its id,
 * model and format, and records the id with the tag as esl_image_prepare does. */
size_t esl_image_draw(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint8_t *out, size_t cap,
                      uint32_t *id, uint8_t *model, uint8_t *fmt);

/* Frees the tag's buffer, if any (the tag was deleted, or shows its image). */
void esl_image_release(esl_image_t *im, uint64_t mac);
