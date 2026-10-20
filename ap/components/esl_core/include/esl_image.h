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
 * layout file for its model), so the image is drawn when a tag needs it and dropped
 * again. A few buffers hold the images being sent: a tag polls with another image id
 * than the one its content gives, the AP draws the image into a free buffer and sends
 * it block by block (IMG / IMG_REQ) in that wake-up; the buffer is free again when the
 * tag reports the id, or ESL_IMAGE_HOLD_MS after its last block. When every buffer is
 * busy with another tag, the tag is told to come back in a couple of seconds.
 *
 * The id of an image is the CRC-32 of its bytes (esl_render draws the same bytes for
 * the same content), so a tag that shows an image is never sent it again, also after
 * an AP restart; the store keeps the id with the tag once it has been drawn.
 *
 * Every public function takes the lock; drawing (under it) takes the layouts lock too,
 * so the two must not be the same lock as the store's. */

#define ESL_IMAGE_BUFS    2
#define ESL_IMAGE_HOLD_MS 120000u   /* a buffer stays with its tag this long after the last block */
#define ESL_IMAGE_BUSY_S  2         /* next_poll_s for a tag that must wait for a buffer */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
} esl_image_ops_t;

typedef struct {
    uint64_t mac;         /* 0 = free */
    uint32_t content_id;  /* the content the image was drawn from */
    uint32_t id;          /* image id (CRC-32 of data) */
    uint16_t size;
    uint8_t model;
    uint64_t used_ms;     /* last block sent, or drawn */
    uint8_t *data;        /* cap bytes */
} esl_image_buf_t;

typedef struct {
    esl_image_buf_t buf[ESL_IMAGE_BUFS];
    size_t cap;           /* bytes per buffer: the largest esl_render_size() */
    esl_image_ops_t ops;
    uint32_t drawn;       /* images drawn (for the log and the tests) */
} esl_image_t;

/* Bytes the buffers need: ESL_IMAGE_BUFS times the largest image of any model. */
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

/* Frees the tag's buffer, if any (the tag was deleted, or shows its image). */
void esl_image_release(esl_image_t *im, uint64_t mac);

/* Draws the tag's image into out (cap bytes) outside the buffers, for the API and the
 * dashboard: returns its size (0: no such tag, no content, or cap too small) and its
 * id in *id (may be NULL). */
size_t esl_image_draw(esl_image_t *im, esl_store_t *st, esl_layouts_t *ls, uint64_t mac, uint8_t *out, size_t cap,
                      uint32_t *id);
