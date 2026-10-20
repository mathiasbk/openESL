#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_proto.h"

/* Firmware images for the tags, one per model (docs/tag-ota.md, krav T15).
 *
 * An image is the OTA file build.py makes (openesl-ota.bin, in the releases
 * as openesl-tag-<model>-<version>-ota.bin): the ESL_FWF_* header, then the application. It is
 * uploaded in parts (esl_tagfw_part, PUT /api/tag-firmware) into a slot of
 * ESL_TAGFW_SLOT bytes per model in the store (a flash partition on the
 * device, RAM on the PC), checked, and then offered to every tag of that
 * model that polls with ESL_POLL_F_OTA and another firmware version. The
 * tags fetch it block by block (FW_REQ / FW).
 *
 * Every public function takes the lock for the table. Reads for the radio
 * run under it; erasing and writing the store (slow on flash) run outside
 * it, on a slot that is not offered meanwhile. One upload at a time. */

#define ESL_TAGFW_SLOT 0x10000u   /* bytes per model in the store */
#define ESL_TAGFW_CHOSEN 16       /* tags that can be chosen for an image; more: all of them */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    /* The store: slot model - 1, ESL_TAGFW_SLOT bytes each. erase clears a
       whole slot; write only follows an erase, in order. */
    bool (*erase)(void *ctx, uint8_t model);
    bool (*write)(void *ctx, uint8_t model, uint32_t off, const void *data, size_t len);
    bool (*read)(void *ctx, uint8_t model, uint32_t off, void *data, size_t len);
    /* Which tags an image is for (esl_tagfw_to_t), kept across restarts.
       save with to NULL erases it; load returns false when nothing is kept.
       Both may be NULL (RAM only). */
    void (*save_to)(void *ctx, uint8_t model, const void *to, size_t len);
    bool (*load_to)(void *ctx, uint8_t model, void *to, size_t len);
} esl_tagfw_ops_t;

typedef struct {
    uint16_t ver;    /* POLL format (major * 10000 + minor * 100 + patch); 0 = no image */
    uint16_t size;   /* bytes of the application */
    uint32_t crc;    /* CRC-32 of the application */
} esl_tagfw_img_t;

/* The tags an image is offered to: all of the model, or the n in mac. ver is
   the image's, so a kept list for another image counts for nothing. */
typedef struct {
    uint16_t ver;
    uint8_t all;
    uint8_t n;
    uint64_t mac[ESL_TAGFW_CHOSEN];
} esl_tagfw_to_t;

typedef struct {
    esl_tagfw_img_t img[ESL_MODEL_COUNT - 1];   /* index model - 1 */
    esl_tagfw_to_t to[ESL_MODEL_COUNT - 1];     /* who img is for */
    esl_tagfw_ops_t ops;
    uint8_t up_model;                           /* upload in progress, 0 = none */
    uint32_t up_got, up_total;
    uint64_t up_only;                           /* the upload is for this tag only, 0 = all */
} esl_tagfw_t;

/* Reads what the store holds and keeps each image whose header and CRC are
   right. ops may not be NULL. */
void esl_tagfw_init(esl_tagfw_t *fw, const esl_tagfw_ops_t *ops);

/* One part of an upload: len bytes at offset of an OTA file of total bytes.
   Offset 0 starts an upload (the model comes from the header in this part),
   every other part must follow the last, or overlap it (a part sent again: the
   bytes the store has are skipped). only (read with the part at offset 0)
   offers the new image to that tag alone, 0 to every tag of the model. The model's old image is no longer
   offered from the first part on. Returns the HTTP status, with a JSON reply
   in out: {"received","total"} on the way, with the image's fields
   (esl_tagfw_write_fields) when the last part made it whole, or {"error"}. */
int esl_tagfw_part(esl_tagfw_t *fw, size_t offset, size_t total, const uint8_t *data, size_t len, uint64_t only,
                   esl_sb_t *out);

/* The image for a model; false (and *img zeroed) if none. img may be NULL. */
bool esl_tagfw_get(esl_tagfw_t *fw, uint8_t model, esl_tagfw_img_t *img);
bool esl_tagfw_delete(esl_tagfw_t *fw, uint8_t model);
/* Whether the model's image is offered to this tag. False when there is none. */
bool esl_tagfw_for(esl_tagfw_t *fw, uint8_t model, uint64_t mac);
/* Who the model's image is for; false (and *to zeroed) if there is no image. */
bool esl_tagfw_get_to(esl_tagfw_t *fw, uint8_t model, esl_tagfw_to_t *to);
/* Offers the model's image to one more tag (mac), or to every tag (mac 0).
   0 when done, 404 when the model has no image, 409 when ESL_TAGFW_CHOSEN
   tags are chosen already. */
int esl_tagfw_choose(esl_tagfw_t *fw, uint8_t model, uint64_t mac);
/* "model","version","size","crc" of one image (version as "1.2.3"), without braces */
void esl_tagfw_write_fields(esl_sb_t *out, uint8_t model, const esl_tagfw_img_t *img);

/* The FW reply for a tag of this model that asks for the block at pos of
   image ver/crc (FW_REQ), or pos 0 of the current image after a POLL (ver 0).
   An image that changed in the meantime is sent from 0 again. Returns 0
   when the model has no image. out holds ESL_MAX_PAYLOAD bytes. */
size_t esl_tagfw_block(esl_tagfw_t *fw, uint8_t model, uint16_t ver, uint32_t crc, uint16_t pos, uint8_t max,
                       uint16_t next_poll_s, uint8_t *out);
