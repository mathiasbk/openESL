#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Gicisky / PICKSMART 2.9-inch BWRY tag (docs/gicisky-296-bwry.md).
 *
 * The tag keeps the maker's firmware; the AP packs a picture into its framebuffer and
 * sends it over BLE. The model is gicisky-29-bwry in shared/models.json;
 * the AP's NimBLE central in ap/main/ble.c uses this module, with the transport supplied
 * by the caller.
 *
 * The AP's renderer draws 1 bit per pixel in screen coordinates: the black plane, then
 * the red plane on a model with red, then the yellow plane on a model with yellow
 * (esl_render.h). The tag's framebuffer is 2 bits per pixel, rotated 90 degrees
 * counter-clockwise, with black=0, white=1, yellow=2 and red=3.
 */

#define ESL_GICISKY_WIDTH       296u
#define ESL_GICISKY_HEIGHT      128u
#define ESL_GICISKY_COL_BYTES   16u                          /* ESL_RENDER_COL_BYTES(128) */
#define ESL_GICISKY_PLANES_SIZE (ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES * 2u) /* 9472 */
#define ESL_GICISKY_PLANES3_SIZE (ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES * 3u) /* 14208 */
#define ESL_GICISKY_FB_SIZE     (ESL_GICISKY_WIDTH * ESL_GICISKY_HEIGHT / 4u)    /* 9472 */
#define ESL_GICISKY_CHUNK       240u
#define ESL_GICISKY_CHUNKS      ((ESL_GICISKY_FB_SIZE + ESL_GICISKY_CHUNK - 1u) / ESL_GICISKY_CHUNK) /* 40 */
#define ESL_GICISKY_LAST_CHUNK  (ESL_GICISKY_FB_SIZE - (ESL_GICISKY_CHUNKS - 1u) * ESL_GICISKY_CHUNK) /* 112 */

/* Values in the tag's 2-bit framebuffer */
#define ESL_GICISKY_BLACK       0u
#define ESL_GICISKY_WHITE       1u
#define ESL_GICISKY_YELLOW      2u
#define ESL_GICISKY_RED         3u

/* Packs the AP's raw image (planes, ESL_GICISKY_PLANES_SIZE bytes: the black plane, then
 * the red plane) into the tag's framebuffer (fb, ESL_GICISKY_FB_SIZE bytes). A red pixel
 * is red; a black pixel is black; every other pixel is white. Returns false on a bad size
 * or a NULL pointer. */
bool esl_gicisky_pack_bw_red(const uint8_t *planes, size_t len, uint8_t *fb);

/* Packs 1, 2 or 3 AP image planes (black, then red, then yellow; each
 * ESL_GICISKY_WIDTH * ESL_GICISKY_COL_BYTES bytes) into the tag's framebuffer. A red
 * pixel is red, else a yellow one yellow, else black, else white. */
bool esl_gicisky_pack_planes(const uint8_t *planes, size_t len, uint8_t *fb);

/* Packs a logical 2-bit image (296 x 128, row-major, four pixels per byte, MSB first,
 * values above) into the tag's framebuffer. For a renderer that draws yellow too.
 * Returns false on a bad size or a NULL pointer. */
bool esl_gicisky_pack_logical_2bpp(const uint8_t *logical, size_t len, uint8_t *fb);

/* The characteristic a payload goes to. */
typedef enum {
    ESL_GICISKY_CMD = 0,  /* FEF1: commands and status */
    ESL_GICISKY_DATA = 1, /* FEF2: image chunks */
} esl_gicisky_chan_t;

/* What the transfer needs from the BLE central. write sends one payload to a
 * characteristic; wait waits for a notification whose first len bytes are prefix (other
 * notifications are ignored). Both block and return false on a timeout or a broken link. */
typedef struct {
    void *ctx;
    bool (*write)(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len);
    bool (*wait)(void *ctx, const uint8_t *prefix, size_t len);
} esl_gicisky_io_t;

typedef enum {
    ESL_GICISKY_SENT = 0, /* the tag took the image */
    ESL_GICISKY_AWAY,     /* a write or a notification failed */
    ESL_GICISKY_REFUSED,  /* reserved: a refusal the tag may send (not seen yet) */
} esl_gicisky_res_t;

/* Sends one framebuffer (fb, len = ESL_GICISKY_FB_SIZE) through io: the commands on
 * FEF1, the chunks on FEF2, waiting for each notification. The display refreshes when
 * the central disconnects; this function does not disconnect. */
esl_gicisky_res_t esl_gicisky_send(const esl_gicisky_io_t *io, const uint8_t *fb, size_t len);
