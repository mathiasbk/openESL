#pragma once
#include <stddef.h>

/* The dashboard the AP serves itself (GET /dashboard and /ui/...), packed from
 * web/ by webui.py at build time: gzipped files in flash. */
typedef struct {
    const char *path; /* NULL ends the table */
    const char *type;
    const unsigned char *gz;
    size_t len;
} webui_file_t;

extern const webui_file_t webui_files[];
