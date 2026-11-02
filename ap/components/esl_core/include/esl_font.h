#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esl_json.h"

/* Font files (#177): fonts for layouts that are data on the AP instead of code in its firmware.
 *
 * The six built-in fonts (small, medium, large, mid, text20, price44) are compiled into the AP
 * and the web page's renderer from tag/fonts/font_*.c. A font file adds another one without a
 * new firmware: tag/tools/gen_font.py --file writes one ("format": "esl-font", JSON), PUT
 * /api/fonts/{name} stores it on the AP, and a layout names it in "fonts" like a built-in one.
 * The designer loads the same file into its renderer (web/wasm/web_api.c), so its preview is
 * still what the tag gets. Only the AP draws with these fonts: the tags get the drawn image and
 * never see a font, so nothing changes on the radio or in the tag firmware.
 *
 * The file holds the same tables as a built-in font (tag/src/font.h): height, baseline, the
 * first and last character (Latin-1), the width of each character (0 = not in the font) and
 * the bitmap, column by column, ceil(height / 8) bytes a column, MSB of the first byte at the
 * top, the characters one after the other:
 *
 *   {"format": "esl-font", "version": 1, "name": "text28", "height": 28, "baseline": 22,
 *    "first": 32, "last": 255, "widths": [7, 0, ...], "bitmap": "<base64>",
 *    "source": "Inter-SemiBold.ttf, 22 px, OFL 1.1"}
 *
 * "source" (optional, up to ESL_FONT_SOURCE_MAX bytes) says where the glyphs come from. Keys
 * starting with _ are ignored.
 *
 * A font is held in RAM (the AP draws from it) as a blob: the AP keeps the blob in NVS too
 * and writes the JSON again for GET /api/fonts/{name}. Blob, version 1:
 *   ver(1) height(1) baseline(1) first(1) last(1) name_len(1) name src_len(1) src
 *   widths[last - first + 1] bitmap
 *
 * Font ids: the built-in fonts are 0..ESL_FONT_COUNT - 1 (shared/proto.h); font file n
 * (a slot, 0..ESL_FONT_USER_MAX - 1) is ESL_FONT_USER_BASE + n in a compiled layout file. The
 * layouts name fonts, and are compiled again at boot after the fonts are loaded, so a slot
 * number never leaves the AP. */

#define ESL_FONT_USER_BASE 0x10
#define ESL_FONT_USER_MAX  8
#define ESL_FONT_NAME_MAX  15
#define ESL_FONT_SOURCE_MAX 63
#define ESL_FONT_MIN_H     6
#define ESL_FONT_MAX_H     64     /* 8 bytes a column */
#define ESL_FONT_BLOB_VER  1
/* All font files on the AP together, in blob bytes (their RAM, and their room in NVS) */
#ifndef ESL_FONT_MAX_BYTES
#define ESL_FONT_MAX_BYTES 32768
#endif
#define ESL_FONT_ERR_MAX   128

/* A font file in RAM: the font as tag/src/font.h has it, pointing into blob */
typedef struct esl_ufont esl_ufont_t;

/* ---- font files ------------------------------------------------------------------------- */

/* Checks a font file (JSON) and turns it into a new font (malloc'd, freed with
 * esl_font_free). name: the name it must have (the URL's), or NULL to take the file's; the
 * file may leave "name" out when name is given. NULL with a message in err on a bad file. */
esl_ufont_t *esl_font_from_json(const char *json, size_t len, const char *name, char *err, size_t errlen);

/* The same from a blob (NVS). NULL if it is not a valid blob. */
esl_ufont_t *esl_font_from_blob(const uint8_t *blob, size_t len);

void esl_font_free(esl_ufont_t *f);

/* The font's blob and its size, and its name */
const uint8_t *esl_font_blob(const esl_ufont_t *f, size_t *len);
const char *esl_font_name(const esl_ufont_t *f);

/* Writes the font file (JSON) of f to out. */
void esl_font_write_json(const esl_ufont_t *f, esl_sb_t *out);

/* Writes {"name","height","baseline","first","last","characters","bytes","version","source"}
 * of f to out (no closing brace when open is true, for a caller that adds keys). */
void esl_font_write_info(const esl_ufont_t *f, esl_sb_t *out, bool open);

/* true for a name a font file may have: 1 to ESL_FONT_NAME_MAX of a-z, 0-9, _ and -, and not
 * the name of a built-in font */
bool esl_font_name_ok(const char *name, size_t len);

/* ---- the fonts the AP draws with ----------------------------------------------------------- */

/* The table of font files is one per program: the AP's, or the web page renderer's. Changes
 * to it go through esl_layouts_font_* (esl_layout.h), under the layouts' lock, since the
 * renderer reads it while it draws. */

/* esl_font_get(id), declared in esl_render.c (it needs tag/src/font.h): the font with this id,
 * a font file's, or font_get(id) for the rest (an unknown id gives the medium font, as
 * font_get does). */

/* Id of the font called name (built-in or a font file), -1 if there is none. */
int esl_font_find(const char *name, size_t len);

/* Height in pixels of font id; 0 for an id with no font. */
uint8_t esl_font_height(uint8_t id);

/* The version of font file id (ESL_FONT_USER_BASE + slot); 0 when the slot is empty. */
uint32_t esl_font_version(uint8_t id);

/* The font file in slot n, or NULL */
const esl_ufont_t *esl_font_slot(unsigned n);

/* The slot of the font file called name, or -1 */
int esl_font_slot_of(const char *name, size_t len);

/* A free slot, or -1 */
int esl_font_free_slot(void);

/* Puts f into slot n (f may be NULL to empty it) and returns what was there. */
esl_ufont_t *esl_font_swap(unsigned n, esl_ufont_t *f);

/* Blob bytes of all font files */
size_t esl_font_bytes(void);

/* Writes [{"name","height","baseline"}, ...] of the built-in fonts to out. */
void esl_font_write_builtin(esl_sb_t *out);

/* Empties every slot (tests; the web page's renderer before it loads an AP's fonts). */
void esl_font_clear(void);
