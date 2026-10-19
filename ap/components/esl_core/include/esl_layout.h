#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_json.h"
#include "esl_proto.h"

/* Layout files per tag model (krav G7, A12-A14; docs/protokoll.md part 3).
 *
 * A layout file is JSON ("format": "esl-layouts") with up to 16 named
 * layouts for one model. esl_layout_compile() checks it against the model
 * (screen size, fonts, what the tag can draw) and turns it into the binary
 * file the tags download (ESL_LBIN_* in shared/proto.h). The version of a
 * model's layouts is the CRC32 of that binary, so it changes exactly when
 * what the tags draw changes.
 *
 * Layout n of the file is sent as CONTENT layout ESL_LAYOUT_FILE + n; clients
 * only ever see the names.
 *
 * Storage follows esl_store: every public function takes the lock, the
 * persist hooks are called after it is released. Only the JSON is persisted;
 * the binary is compiled again at boot. */

#define ESL_LAYOUT_MAX_SIZE 8192 /* JSON file */
#define ESL_LAYOUT_FIELD_NAME_MAX 16
#define ESL_LAYOUT_ERR_MAX 160

/* ---- compiler ------------------------------------------------------------ */

typedef struct {
    uint32_t version; /* CRC32 of bin, never 0 */
    uint16_t size;    /* bytes used in bin */
    uint8_t bin[ESL_LBIN_MAX];
    uint8_t count;    /* layouts in the file */
    uint8_t nfields[ESL_LAYOUT_FILE_MAX];
    char names[ESL_LAYOUT_FILE_MAX][ESL_LAYOUT_NAME_MAX + 1];
    /* The barcode element of each layout, if any: its field (-1 = none), symbology
     * and the widest code in modules, quiet zones included, that fits its box. */
    int8_t bc_field[ESL_LAYOUT_FILE_MAX];
    uint8_t bc_sym[ESL_LAYOUT_FILE_MAX];
    uint16_t bc_max[ESL_LAYOUT_FILE_MAX];
} esl_layout_bin_t;

/* Compiles a layout file for model. Returns false with a message in err (at
 * least ESL_LAYOUT_ERR_MAX bytes) that names the place in the file, e.g.
 * "layouts[0].elements[2]: x + w = 300 is past the right edge (296)". */
bool esl_layout_compile(const char *json, size_t len, uint8_t model, esl_layout_bin_t *out, char *err,
                        size_t errlen);

/* Index of name in a compiled file, or -1. */
int esl_layout_bin_find(const esl_layout_bin_t *b, const char *name, size_t len);

/* ---- storage --------------------------------------------------------------- */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*persist_save)(void *ctx, uint8_t model, const char *data, size_t len);
    void (*persist_erase)(void *ctx, uint8_t model);
    void *(*alloc)(size_t n);   /* memory for a file, freed with free(); NULL = malloc (tests fail it) */
    /* Reads the saved JSON of a model into buf (at most cap bytes); bytes read, 0 = none.
     * When set, the JSON is not kept in RAM: only its length and CRC-32 are, and
     * esl_layouts_write reads it back through this. NULL keeps the JSON in RAM. */
    size_t (*persist_load)(void *ctx, uint8_t model, char *buf, size_t cap);
} esl_layout_ops_t;

typedef struct {
    uint16_t len;            /* bytes of JSON, never 0 */
    uint32_t crc;            /* CRC-32 of the JSON: tells whether an upload is the same file */
    char *data;              /* the JSON, len bytes (not terminated); NULL when persist_load holds it */
    esl_layout_bin_t bin;
} esl_layout_t;

/* One esl_layout_t on the heap for each model that has a file, NULL for the rest:
 * the AP has about 60 KB free and a file costs 1.4 KB (plus its JSON without
 * persist_load), so holding ESL_LAYOUT_MAX_SIZE for every model in the registry
 * would not fit. */
typedef struct {
    esl_layout_t *models[ESL_MODEL_COUNT - 1]; /* index model - 1 */
    esl_layout_ops_t ops;
} esl_layouts_t;

/* What the radio path needs to know about one model's file when it answers a
 * POLL (copied out, so the store can use it under its own lock). */
typedef struct {
    uint32_t version; /* 0 = no file for this model */
    uint8_t count;
    uint8_t nfields[ESL_LAYOUT_FILE_MAX];
    char names[ESL_LAYOUT_FILE_MAX][ESL_LAYOUT_NAME_MAX + 1];
} esl_layout_env_t;

/* ops may be NULL (no locking, no persistence). ls must be zeroed (a static, or
 * calloc) or one that was initialised before: its files are freed first. */
void esl_layouts_init(esl_layouts_t *ls, const esl_layout_ops_t *ops);

/* Frees every file (nothing is persisted or erased). */
void esl_layouts_free(esl_layouts_t *ls);

/* Loads a persisted file at boot (compiling it again). Does not call
 * persist_save. False for an unknown model or a file that no longer compiles. */
bool esl_layouts_restore(esl_layouts_t *ls, uint8_t model, const char *data, size_t len);

/* Stores a file that esl_layout_compile() accepted as bin for this model.
 * Returns the version (0 for an unknown model, a bad size, or no memory; the model then
 * keeps the file it had). *changed is false (and nothing is persisted) when the model
 * already has exactly this file, or when there was no memory. */
uint32_t esl_layouts_set(esl_layouts_t *ls, uint8_t model, const char *data, size_t len,
                         const esl_layout_bin_t *bin, bool *changed);

bool esl_layouts_delete(esl_layouts_t *ls, uint8_t model);

/* Version, JSON size and binary size of a model's file; false if it has none.
 * Any pointer may be NULL. */
bool esl_layouts_info(esl_layouts_t *ls, uint8_t model, uint32_t *version, size_t *len, size_t *bin_size);

/* Appends a model's file (the JSON) to out; false if it has none, or if persist_load
 * holds it and gives nothing back. */
bool esl_layouts_write(esl_layouts_t *ls, uint8_t model, esl_sb_t *out);

/* Writes the layout names of a model's file as a JSON array to out ("[]" if none). */
void esl_layouts_write_names(esl_layouts_t *ls, uint8_t model, esl_sb_t *out);

/* Index of layout name in a model's file, -1 if the model has no such layout.
 * *nfields (may be NULL) gets the number of fields it takes. */
int esl_layouts_find(esl_layouts_t *ls, uint8_t model, const char *name, size_t len, uint8_t *nfields);

/* Checks the barcode field of content c (a named layout) against the layout of that name
 * in model's file: it must be one the tag can encode and fit in the element's box.
 * Returns true if it is fine, or if the file has no such layout; else false with a
 * message in msg. */
bool esl_layouts_check_barcode(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, char *msg, size_t msglen);

/* Fills env for a model; version 0 and no names if it has no file. ls may be NULL. */
void esl_layouts_env(esl_layouts_t *ls, uint8_t model, esl_layout_env_t *env);

/* Builds the LAYOUT reply for a tag of this model that asks for the block at
 * pos of version ver (from LAYOUT_REQ; ver 0 / pos 0 for the first block after
 * a POLL). If the model's file is no longer ver (it changed during the
 * download), the block at 0 of the current file is sent, so the tag starts
 * over; version 0 means the AP has no file for the model. The block has at
 * most max bytes (and at most ESL_LAYOUT_CHUNK_MAX). out holds ESL_MAX_PAYLOAD
 * bytes. Returns the length. ls may be NULL. */
size_t esl_layouts_block(esl_layouts_t *ls, uint8_t model, uint32_t ver, uint16_t pos, uint8_t max,
                         uint16_t next_poll_s, uint8_t *out);
