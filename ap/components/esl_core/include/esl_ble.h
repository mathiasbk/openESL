#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_gicisky.h"
#include "esl_push.h"

/* The BLE transport for the Gicisky 2.9-inch BWRY tag: connect, pack the AP's picture
 * into the tag's framebuffer, run esl_gicisky_send, disconnect.
 *
 * The BLE central itself is the backend: connect, write, wait, disconnect and info. The
 * AP's NimBLE implementation in ap/main/ble.c fills it in; the host tests use a fake.
 *
 * esl_ble_send matches esl_push_out_t.send: give an esl_ble_t as its ctx. */

typedef struct {
    void *ctx;          /* the backend's own state */
    uint8_t *fb;        /* ESL_GICISKY_FB_SIZE bytes of scratch */
    size_t fb_cap;
    uint8_t model;      /* the ESL_MODEL_* it speaks; 0 = any */
    /* Connects to mac and discovers FEF1/FEF2. False when it cannot. */
    bool (*connect)(void *ctx, uint64_t mac);
    /* Ends a connection that connect() made. Never called after a failed connect. */
    void (*disconnect)(void *ctx);
    /* The esl_gicisky_io_t operations, on the connected tag. */
    bool (*write)(void *ctx, esl_gicisky_chan_t chan, const uint8_t *data, size_t len);
    bool (*wait)(void *ctx, const uint8_t *prefix, size_t len);
    /* Optional (may be NULL): what the tag reported, for esl_push_info_t. */
    bool (*info)(void *ctx, esl_push_info_t *info);
} esl_ble_t;

/* esl_push_out_t.send for a Gicisky tag: connect, pack img (the AP's raw black/red
 * planes, ESL_GICISKY_PLANES_SIZE bytes) into the framebuffer, send it, disconnect.
 * info is zeroed by the caller; the backend's info fills it when it can. */
esl_push_res_t esl_ble_send(void *ctx, uint64_t mac, uint8_t model, const uint8_t *img, size_t len,
                            esl_push_info_t *info);
