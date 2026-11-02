#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_content.h"
#include "esl_font.h"
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

#ifndef ESL_LAYOUT_MAX_SIZE
#define ESL_LAYOUT_MAX_SIZE 8192 /* JSON file; per AP board, as ESL_LBIN_MAX in shared/proto.h */
#endif
#define ESL_LAYOUT_FIELD_NAME_MAX 16
#define ESL_LAYOUT_ERR_MAX 160

/* ---- compiler ------------------------------------------------------------ */

/* The limits the compiler checks a file against: the AP board's (ESL_DRAW_MAX_*, ESL_LBIN_MAX
 * and ESL_LAYOUT_MAX_SIZE, set per board in ap/boards/<board>/board.json). GET /api/layouts
 * reports them. The web page's renderer (web/wasm/web_api.c) is built with room for the
 * largest board (ESL_*_CAP) and sets them to those of the AP it talks to; nothing else
 * changes them. Each is at most its ESL_*_CAP. */
typedef struct {
    uint16_t runs;       /* lines of text per screen */
    uint16_t rects;      /* rectangles and inverted fields per screen */
    uint16_t lines;      /* line elements per screen */
    uint16_t icons;      /* icons per screen */
    uint16_t file_bytes; /* compiled file */
    uint16_t json_bytes; /* JSON file */
} esl_limits_t;

extern esl_limits_t esl_limits;

typedef struct {
    uint32_t version; /* CRC32 of bin, never 0 */
    uint16_t size;    /* bytes used in bin */
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
    /* ESL_CLOCK_* of each layout: whether its fixed texts have date fields (esl_clock.h) */
    uint8_t clock[ESL_LAYOUT_FILE_MAX];
    /* Bit n: a layout draws with font file n (esl_font.h). Its version is part of the file's. */
    uint8_t user_fonts;
    /* Where each layout's "fields" list (the JSON array) is in the JSON file that was
     * compiled, so a client can name the fields (esl_layouts_field_index) without the AP
     * keeping the names twice. Not part of the compiled bytes or the version. */
    uint16_t fields_at[ESL_LAYOUT_FILE_MAX];
    uint16_t fields_len[ESL_LAYOUT_FILE_MAX];
    /* Last, so a stored file (esl_layout_t) is allocated only up to bin[size]: the
     * compiler writes into a whole one, the store keeps ESL_LAYOUT_BIN_BYTES(size). */
    uint8_t bin[ESL_LBIN_CAP];
} esl_layout_bin_t;

/* Bytes of an esl_layout_bin_t up to and including bin[size - 1] */
#define ESL_LAYOUT_BIN_BYTES(size) (offsetof(esl_layout_bin_t, bin) + (size))

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
    /* Font files (esl_layouts_font_*): saves the blob of the font in slot n, or erases it
       (data NULL). false when it could not be saved (no room); NULL = nothing is persisted. */
    bool (*font_save)(void *ctx, unsigned slot, const uint8_t *data, size_t len);
} esl_layout_ops_t;

typedef struct {
    uint16_t len;            /* bytes in data, never 0 */
    char *data;              /* the JSON, len bytes (not terminated) */
    esl_layout_bin_t bin;    /* last: allocated only up to bin.bin[bin.size] (ESL_LAYOUT_BYTES), so
                                never copy it whole */
} esl_layout_t;

/* Bytes of a stored file whose compiled bytes are size long */
#define ESL_LAYOUT_BYTES(size) (offsetof(esl_layout_t, bin) + ESL_LAYOUT_BIN_BYTES(size))

/* One esl_layout_t on the heap for each model that has a file, NULL for the rest,
 * each as big as its compiled file (#187): the metadata is about 0.7 KB, the compiled
 * bytes up to ESL_LBIN_MAX, plus the JSON. Holding ESL_LAYOUT_MAX_SIZE and
 * ESL_LBIN_MAX for every model in the registry would not fit in the C6's heap. */
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

/* A model with same_as in shared/models.json (#181) has no file of its own: the functions
 * below that read a model's file (info, write, names, find, check, render, env, image_ver,
 * clock) give it the file of the model it names (esl_model_layout_model), and restore, set and
 * delete refuse it (false or 0), so its tags follow that model's file. */

/* Loads a persisted file at boot (compiling it again). Does not call
 * persist_save. False for an unknown model, a model with same_as, or a file that no longer
 * compiles. */
bool esl_layouts_restore(esl_layouts_t *ls, uint8_t model, const char *data, size_t len);

/* Stores a file that esl_layout_compile() accepted as bin for this model.
 * Returns the version (0 for an unknown model, a model with same_as, a bad size, or no memory; the model then
 * keeps the file it had). *changed is false (and nothing is persisted) when the model
 * already has exactly this file, or when there was no memory. */
uint32_t esl_layouts_set(esl_layouts_t *ls, uint8_t model, const char *data, size_t len,
                         const esl_layout_bin_t *bin, bool *changed);

/* False if the model has no file of its own (none, or same_as) */
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

/* The index of the field called field in the layout called name in model's file (as its
 * "fields" list has them), -1 when the layout has no such field, -2 when the file has no
 * such layout. */
int esl_layouts_field_index(esl_layouts_t *ls, uint8_t model, const char *name, size_t len, const char *field,
                            size_t flen);

/* esl_layout_check_fields for content c (a named layout) and the layout of that name in
 * model's file, under the lock. True if the file has no such layout. */
bool esl_layouts_check_content(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, char *msg, size_t msglen);

/* Draws content c for a tag of model with the model's file (esl_render), under the
 * lock, into out (cap bytes). Returns the image size, 0 for an unknown model or when
 * cap is too small. ls may be NULL (no file). */
size_t esl_layouts_render(esl_layouts_t *ls, uint8_t model, const esl_content_t *c, uint8_t *out, size_t cap);

/* The version a tag's image is drawn with (esl_store image_layout_ver): the model's file's
 * version, with the day or the week mixed in when the tag's layout (layout_hash, as
 * esl_tag_t has it) has date fields (esl_clock.h), so a new day draws it again. 0 when the
 * model has no file. */
uint32_t esl_layouts_image_ver(esl_layouts_t *ls, uint8_t model, uint32_t layout_hash);

/* ESL_CLOCK_* of the tag's layout (layout_hash as esl_tag_t has it): whether it has date fields */
uint8_t esl_layouts_clock(esl_layouts_t *ls, uint8_t model, uint32_t layout_hash);

/* Fills env for a model; version 0 and no names if it has no file. ls may be NULL. */
void esl_layouts_env(esl_layouts_t *ls, uint8_t model, esl_layout_env_t *env);

/* ---- font files (esl_font.h) ------------------------------------------------------------- */

/* The fonts are one table per program (esl_font.c), changed only through these, under the
 * layouts' lock, since esl_layouts_render draws with them under it. */

/* Loads a font file persisted in slot n at boot, before the layout files (which may name
 * it). False for a bad blob, a slot in use or no room. Does not call font_save. */
bool esl_layouts_font_restore(esl_layouts_t *ls, unsigned slot, const uint8_t *blob, size_t len);

/* Stores font f (esl_font_from_json; taken over in any case): a new one in a free slot, or in
 * place of the one with its name. The layout files that draw with the old one are compiled
 * again with the new one, so their version changes and their tags are drawn again; a file it
 * no longer compiles (a box now too small for it) turns the font down and nothing changes.
 * Returns 0, or an HTTP status with a message in err: 507 (no free slot, too many bytes, no
 * room in NVS), 422 (a layout file no longer compiles), 500 (no memory). *models gets bit
 * (model - 1) for each model whose file was compiled again, *replaced whether it replaced one. */
int esl_layouts_font_put(esl_layouts_t *ls, esl_ufont_t *f, unsigned *models, bool *replaced, char *err,
                         size_t errlen);

/* Deletes the font file called name. 0, 404 when there is none, or 409 (with the models in
 * err) while a layout file draws with it. */
int esl_layouts_font_delete(esl_layouts_t *ls, const char *name, size_t len, char *err, size_t errlen);

/* Writes the font file called name (JSON) to out; false if there is none. */
bool esl_layouts_font_write(esl_layouts_t *ls, const char *name, size_t len, esl_sb_t *out);

/* Writes [{name, height, ..., "used_by": ["2.9", ...]}, ...] of the font files to out. */
void esl_layouts_font_list(esl_layouts_t *ls, esl_sb_t *out);

