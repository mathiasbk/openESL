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
 * (screen size, fonts, what esl_render can draw) and turns it into the binary
 * file the AP draws with (ESL_LBIN_* in shared/proto.h). The version of a
 * model's layouts is the CRC32 of that binary, so it changes exactly when
 * what the tags show changes.
 *
 * Content names its layout (ESL_LAYOUT_NAMED); esl_render looks the name up
 * in the model's file.
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
    /* The QR element of each layout, if any: its field (-1 = none), level and the most bytes
     * it holds in its box. */
    int8_t qr_field[ESL_LAYOUT_FILE_MAX];
    uint8_t qr_ecc[ESL_LAYOUT_FILE_MAX];
    uint16_t qr_cap[ESL_LAYOUT_FILE_MAX];
} esl_layout_bin_t;

/* Compiles a layout file for model. Returns false with a message in err (at
 * least ESL_LAYOUT_ERR_MAX bytes) that names the place in the file, e.g.
 * "layouts[0].elements[2]: x + w = 300 is past the right edge (296)". */
bool esl_layout_compile(const char *json, size_t len, uint8_t model, esl_layout_bin_t *out, char *err,
                        size_t errlen);

/* Index of name in a compiled file, or -1. */
int esl_layout_bin_find(const esl_layout_bin_t *b, const char *name, size_t len);

/* Checks the barcode and QR fields of content c against layout i of a compiled file for
 * model: they must be ones the AP can encode and fit in the element's box. Returns true if
 * they are fine, else false with a message in msg. */
bool esl_layout_check_fields(const esl_layout_bin_t *b, int i, uint8_t model, const esl_content_t *c, char *msg,
                             size_t msglen);

/* ---- storage --------------------------------------------------------------- */

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*persist_save)(void *ctx, uint8_t model, const char *data, size_t len);
    void (*persist_erase)(void *ctx, uint8_t model);
    void *(*alloc)(size_t n);   /* memory for a file, freed with free(); NULL = malloc (tests fail it) */
} esl_layout_ops_t;

typedef struct {
    uint16_t len;            /* bytes in data, never 0 */
    char *data;              /* the JSON, len bytes (not terminated) */
    esl_layout_bin_t bin;
} esl_layout_t;

/* One esl_layout_t on the heap for each model that has a file, NULL for the rest:
 * the AP has about 60 KB free and a file costs 4.6 KB plus its JSON, so holding
 * ESL_LAYOUT_MAX_SIZE for every model in the registry would not fit. */
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

/* Appends a model's file to out; false if it has none. */
bool esl_layouts_write(esl_layouts_t *ls, uint8_t model, esl_sb_t *out);

/* Writes the layout names of a model's file as a JSON array to out ("[]" if none). */
void esl_layouts_write_names(esl_layouts_t *ls, uint8_t model, esl_sb_t *out);

/* Index of layout name in a model's file, -1 if the model has no such layout.
 * *nfields (may be NULL) gets the number of fields it takes. */
int esl_layouts_find(esl_layouts_t *ls, uint8_t model, const char *name, size_t len, uint8_t *nfields);

/* esl_layout_check_fields for content c (a named layout) and the layout of that name in
 * model's file, under the lock. True if the file has no such layout. */
bool esl_layouts_check_content(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, char *msg, size_t msglen);

/* Draws content c for a tag of model with the model's file (esl_render), under the
 * lock, into out (cap bytes). Returns the image size, 0 for an unknown model or when
 * cap is too small. ls may be NULL (no file). */
size_t esl_layouts_render(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, uint8_t *out, size_t cap);

/* Fills env for a model; version 0 and no names if it has no file. ls may be NULL. */
void esl_layouts_env(esl_layouts_t *ls, uint8_t model, esl_layout_env_t *env);

