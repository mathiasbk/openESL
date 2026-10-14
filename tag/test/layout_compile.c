/*
 * layout_compile.c - host tool: compiles a layout file (JSON, docs/protokoll.md part 3) into
 * the binary the tags download, with the AP's own compiler (ap/components/esl_core/
 * esl_layout_compile.c), so the previews show exactly what a tag would get.
 *
 *   layout_compile <model code> <in.json> <out.bin>     model code: 1 = 2.9", 2 = 1.54"
 *
 * Prints the version and size; on an error prints the AP's message (the one PUT
 * /api/layouts/{model} answers with 422) and exits with 1. Built by tools/render_host.py.
 */
#include <stdio.h>
#include <stdlib.h>
#include "esl_layout.h"

int main(int argc, char **argv)
{
    static char js[ESL_LAYOUT_MAX_SIZE + 1];
    static esl_layout_bin_t bin;
    char err[ESL_LAYOUT_ERR_MAX];
    FILE *f;
    size_t len;

    if (argc != 4) {
        fprintf(stderr, "usage: layout_compile <model code> <in.json> <out.bin>\n");
        return 2;
    }
    f = fopen(argv[2], "rb");
    if (!f) {
        fprintf(stderr, "%s: cannot read\n", argv[2]);
        return 1;
    }
    len = fread(js, 1, sizeof(js), f);
    fclose(f);
    if (!esl_layout_compile(js, len, (uint8_t)atoi(argv[1]), &bin, err, sizeof(err))) {
        fprintf(stderr, "%s: %s\n", argv[2], err);
        return 1;
    }
    f = fopen(argv[3], "wb");
    if (!f || fwrite(bin.bin, 1, bin.size, f) != bin.size) {
        fprintf(stderr, "%s: cannot write\n", argv[3]);
        return 1;
    }
    fclose(f);
    printf("%s: version %u, %u bytes, %u layouts\n", argv[2], (unsigned)bin.version, (unsigned)bin.size,
           (unsigned)bin.count);
    return 0;
}
