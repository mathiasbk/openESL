#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"
#include "esl_proto.h"

/* Layout files per tag model (krav A12, G7; docs/protokoll.md part 3).
 *
 * The file format from the design tool is not specified yet, so the AP keeps
 * the file as it was uploaded (a JSON object, at most ESL_LAYOUT_MAX_SIZE
 * bytes) with a version (CRC32 of the file, never 0). Sending a layout to the
 * tags over radio waits for the block transfer in protokoll.md part 3.
 *
 * Same locking and persistence pattern as esl_store: every public function
 * takes the lock, the persist hooks are called after it is released. */

#define ESL_LAYOUT_MAX_SIZE 8192

typedef struct {
    void *ctx;
    void (*lock)(void *ctx);
    void (*unlock)(void *ctx);
    void (*persist_save)(void *ctx, uint8_t model, const char *data, size_t len);
    void (*persist_erase)(void *ctx, uint8_t model);
} esl_layout_ops_t;

typedef struct {
    uint32_t version; /* 0 = no layout for this model */
    uint16_t len;
    char data[ESL_LAYOUT_MAX_SIZE];
} esl_layout_t;

typedef struct {
    esl_layout_t models[ESL_MODEL_COUNT - 1]; /* index model - 1 */
    esl_layout_ops_t ops;
} esl_layouts_t;

/* ops may be NULL (no locking, no persistence). */
void esl_layouts_init(esl_layouts_t *ls, const esl_layout_ops_t *ops);

/* Loads a persisted file at boot. Does not call persist_save. */
bool esl_layouts_restore(esl_layouts_t *ls, uint8_t model, const char *data, size_t len);

/* Stores a file. The caller has validated it. Returns the version, or 0 for
 * an unknown model or a file that is too big. *changed is false (and nothing
 * is persisted) when the model already has exactly this file. */
uint32_t esl_layouts_set(esl_layouts_t *ls, uint8_t model, const char *data, size_t len, bool *changed);

bool esl_layouts_delete(esl_layouts_t *ls, uint8_t model);

/* Version and size of a model's file; false if it has none. */
bool esl_layouts_info(esl_layouts_t *ls, uint8_t model, uint32_t *version, size_t *len);

/* Appends a model's file to out; false if it has none. */
bool esl_layouts_write(esl_layouts_t *ls, uint8_t model, esl_sb_t *out);
