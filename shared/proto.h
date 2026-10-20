/*
 * proto.h - felles pakkedefinisjoner for radioprotokollen tag <-> AP.
 *
 * Kilde: docs/protokoll.md del 1 (eget dokument, laget uavhengig av annen firmware).
 * Brukes av både tag (SDCC, 8051) og AP (GCC, RISC-V), derfor:
 *   - bare faste typer fra <stdint.h>
 *   - ingen bitfelt og ingen structer som legges rett over bytene
 *   - alle flerbyte-tall er little-endian og leses/skrives med makroene under
 */
#ifndef ESL_PROTO_H
#define ESL_PROTO_H

#include <stdint.h>

/* ---- Meldinger (nyttelasten i 802.15.4-rammen) ---- */

#define ESL_PROTO_VER        1

#define ESL_MSG_POLL         0x01   /* tag -> AP */
#define ESL_MSG_IDLE         0x02   /* AP -> tag: ingenting nytt */
#define ESL_MSG_CONTENT      0x03   /* AP -> tag: nytt innhold */
#define ESL_MSG_CONFIG       0x08   /* AP -> tag: taginnstillinger (se CONFIG under) */

/* Felles start på alle meldinger */
#define ESL_OFF_TYPE         0
#define ESL_OFF_VER          1

/* POLL, 12 byte */
#define ESL_POLL_LEN         12
#define ESL_POLL_OFF_FW      2      /* u16 firmwareversjon */
#define ESL_POLL_OFF_BATT    4      /* u16 batterispenning i mV */
#define ESL_POLL_OFF_CID     6      /* u32 content_id som vises nå (0 = ingenting) */
#define ESL_POLL_OFF_FLAGS   10     /* u8 */
#define ESL_POLL_OFF_RSVD    11     /* u8, tidligere reservert (alltid 0), nå modellfeltet under */
#define ESL_POLL_OFF_MODEL   11     /* u8 tagmodell (krav T18), samme byte som ESL_POLL_OFF_RSVD */

#define ESL_POLL_F_BOOTED    0x01   /* nettopp startet */
#define ESL_POLL_F_DRAWFAIL  0x02   /* forrige tegning feilet */
#define ESL_POLL_F_LAYOUTFAIL 0x04  /* tagen avviste layoutfila AP-en tilbød sist (for stor, feil CRC, ugyldig) */
#define ESL_POLL_F_OTA       0x08   /* the tag takes firmware over the air (FW / FW_REQ below) */
#define ESL_POLL_F_FWFAIL    0x10   /* the tag rejected or rolled back the firmware version the AP offers */
#define ESL_POLL_F_IMAGE     0x20   /* the tag shows images drawn by the AP (IMG / IMG_REQ below); content_id
                                     * in its POLL is then the image_id it shows. The AP sends CONTENT and
                                     * LAYOUT only to tags without this flag. */
#define ESL_POLL_F_IMG_RLE   0x40   /* the tag also takes ESL_IMG_FMT_RLE images */

/* Utvidet POLL: firmware som forstår CONFIG legger til cfg_ver etter de 12
 * første bytene. AP-en sender CONFIG bare til tager som sender dette feltet,
 * så eldre tager (12 byte) aldri får en meldingstype de ikke kjenner. */
#define ESL_POLL_OFF_CFG_VER 12     /* u16 cfg_ver tagen bruker nå (0 = innebygde standardverdier) */
#define ESL_POLL_LEN_CFG     14     /* POLL med cfg_ver */

/* POLL med layoutversjon, 18 byte (krav T17): cfg_ver på byte 12-13 som over, så
 * layoutversjonen på 14-17. Dagens tagfirmware sender alltid hele POLL-en på 18 byte.
 * Firmware uten layoutnedlasting sender færre enn 18 byte; AP-en ser da på tagen som
 * "kan ikke laste ned layout". Lengden avgjør hva AP-en kan sende:
 *   12 byte: bare IDLE/CONTENT,  14: også CONFIG,  18: også CONFIG og LAYOUT. */
#define ESL_POLL_LEN_LAYOUT  18
#define ESL_POLL_OFF_LAYOUT_VER 14  /* u32 versjon av layoutfila tagen har (0 = ingen, bare innebygde) */

/* Tagmodell i POLL. 0 er det eldre firmware sender i den reserverte byten,
 * så AP-en leser den som "ukjent" og alt virker som før. De kjente modellene
 * (ESL_MODEL_29, ESL_MODEL_154, ..., ESL_MODEL_COUNT, skjermstørrelsene og
 * ESL_MODEL_LIST) kommer fra modellregisteret shared/models.json. */
#define ESL_MODEL_UNKNOWN    0
#include "models_gen.h"

/* IDLE, 4 byte */
#define ESL_IDLE_LEN         4
#define ESL_OFF_NEXT_POLL    2      /* u16 sekunder til neste POLL (0 = behold), også i CONTENT */

/* CONTENT, 10 byte hode + tekstfelt */
#define ESL_CONTENT_HDR_LEN  10
#define ESL_CONTENT_OFF_CID  4      /* u32 ny content_id */
#define ESL_CONTENT_OFF_LAYOUT 8    /* u8 */
#define ESL_CONTENT_OFF_FLAGS  9    /* u8 */
#define ESL_CONTENT_OFF_FIELDS 10   /* felt: lengde (u8) + Latin-1-tekst, inntil ESL_MAX_FIELDS */

#define ESL_LAYOUT_PRICE     1      /* felt: navn, pris, undertekst */
#define ESL_LAYOUT_TEXT      2      /* felt: linje1..linje4 */
#define ESL_LAYOUT_FILE      0x10   /* 0x10 + n = layout nr. n i den nedlastede layoutfila */
#define ESL_LAYOUT_FILE_MAX  16     /* høyst 16 layouter i en fil: 0x10..0x1F */

#define ESL_CF_PRICE_HIGHLIGHT 0x01 /* framhevet pris: hvite siffer i svart felt (invertert) */
#define ESL_CF_FRAME         0x02   /* svart ramme rundt skjermen */

/* CONFIG, 12 byte: taginnstillinger. Samme next_poll_s på byte 2 som IDLE.
 * Hver innstilling er u16, 0 = tagens innebygde standardverdi (tag/src/config.h).
 * Nye innstillinger legges bare til på slutten: tagen leser feltene den kjenner
 * og hopper over resten, og et felt som mangler i en kortere melding er 0. */
#define ESL_CONFIG_LEN       12
#define ESL_CONFIG_OFF_VER   4      /* u16 cfg_ver; tagen sender den tilbake i POLL som kvittering */
#define ESL_CONFIG_OFF_POLL  6      /* u16 vanlig intervall i sekunder (krav T4) */
#define ESL_CONFIG_OFF_MAX_POLL 8   /* u16 lengste intervall uten svar fra AP-en (krav T11) */
#define ESL_CONFIG_OFF_LOST_AP 10   /* u16 sekunder uten svar før "No AP found" (krav T9) */

#define ESL_CFG_POLL_MIN_S   10     /* grenser for poll_s og max_poll_s */
#define ESL_CFG_POLL_MAX_S   3600
#define ESL_CFG_LOST_AP_MIN_S 60    /* grenser for lost_ap_s */
#define ESL_CFG_LOST_AP_NEVER 0xFFFFu /* lost_ap_s: vis aldri "No AP found" */

#define ESL_MAX_FIELDS       6      /* tekstfelt i CONTENT (tagger med v0.1.0 leser bare de fire første) */
#define ESL_MAX_PAYLOAD      110    /* 127 - 15 (MAC-hode) - 2 (FCS) */
#define ESL_MIC_RESERVE      4      /* holdt av til framtidig MIC (krav T16) */
/* Plass til feltene i CONTENT: tekst + én lengdebyte per felt som sendes, 96.
 * Altså 92 byte tekst med fire felt og 90 med seks. */
#define ESL_CONTENT_FIELD_SPACE (ESL_MAX_PAYLOAD - ESL_CONTENT_HDR_LEN - ESL_MIC_RESERVE)
/* Største sum av tekstbytes med n felt */
#define ESL_MAX_TEXT_FOR(n)  (ESL_CONTENT_FIELD_SPACE - (n))
/* Største sum av tekstbytes AP-en kan sende når alle felt er med: 90 */
#define ESL_MAX_TEXT_BYTES   ESL_MAX_TEXT_FOR(ESL_MAX_FIELDS)

/* ---- Layoutoverføring (docs/protokoll.md del 1, "Layout transfer") ---- */

#define ESL_MSG_LAYOUT       0x04   /* AP -> tag: én blokk av layoutfila */
#define ESL_MSG_LAYOUT_REQ   0x05   /* tag -> AP: be om neste blokk */

/* LAYOUT, 12 byte hode + inntil ESL_LAYOUT_CHUNK_MAX byte data.
 * next_poll_s ligger på ESL_OFF_NEXT_POLL som i IDLE og CONTENT. */
#define ESL_LAYOUT_HDR_LEN   12
#define ESL_LAYOUT_OFF_VER   4      /* u32 versjonen AP-en har for tagmodellen (0 = ingen: bruk innebygde) */
#define ESL_LAYOUT_OFF_SIZE  8      /* u16 hele layoutfila i byte */
#define ESL_LAYOUT_OFF_POS   10     /* u16 hvor i fila dataene starter */
#define ESL_LAYOUT_OFF_DATA  12
#define ESL_LAYOUT_CHUNK_MAX (ESL_MAX_PAYLOAD - ESL_LAYOUT_HDR_LEN - ESL_MIC_RESERVE)   /* 94 */

/* LAYOUT_REQ, 10 byte */
#define ESL_LREQ_LEN         10
#define ESL_LREQ_OFF_MODEL   2      /* u8 tagmodell, som i POLL */
#define ESL_LREQ_OFF_MAX     3      /* u8 største antall databyte tagen vil ha (1..ESL_LAYOUT_CHUNK_MAX) */
#define ESL_LREQ_OFF_VER     4      /* u32 versjonen tagen laster ned */
#define ESL_LREQ_OFF_POS     8      /* u16 første byte tagen mangler */

/* ---- Firmware transfer (docs/protokoll.md part 1, "Firmware transfer"; docs/tag-ota.md) ----
 * The same pattern as the layout transfer: the AP answers a POLL that has ESL_POLL_F_OTA,
 * from a tag whose model has an image on the AP and whose firmware version differs, with
 * the first FW block (after content, layout and CONFIG are done). The tag asks for the
 * rest with FW_REQ. The data is the application image, linked at ESL_FW_APP_BASE. */

#define ESL_MSG_FW           0x06   /* AP -> tag: one block of the firmware image */
#define ESL_MSG_FW_REQ       0x07   /* tag -> AP: ask for the next block */

/* FW, 15 byte header + up to ESL_FW_CHUNK_MAX bytes of data.
 * next_poll_s at ESL_OFF_NEXT_POLL as in IDLE. */
#define ESL_FW_HDR_LEN       15
#define ESL_FW_OFF_VER       4      /* u16 firmware version of the image (POLL format) */
#define ESL_FW_OFF_SIZE      6      /* u16 image size in bytes */
#define ESL_FW_OFF_POS       8      /* u16 where in the image the data starts */
#define ESL_FW_OFF_CRC       10     /* u32 CRC-32 (IEEE) of the whole image */
#define ESL_FW_OFF_MODEL     14     /* u8 tag model the image is built for */
#define ESL_FW_OFF_DATA      15
#define ESL_FW_CHUNK_MAX     (ESL_MAX_PAYLOAD - ESL_FW_HDR_LEN - ESL_MIC_RESERVE)   /* 91 */

/* FW_REQ, 12 byte */
#define ESL_FREQ_LEN         12
#define ESL_FREQ_OFF_MODEL   2      /* u8 tag model */
#define ESL_FREQ_OFF_MAX     3      /* u8 most data bytes the tag wants (1..ESL_FW_CHUNK_MAX) */
#define ESL_FREQ_OFF_VER     4      /* u16 version being downloaded */
#define ESL_FREQ_OFF_POS     6      /* u16 first byte the tag is missing */
#define ESL_FREQ_OFF_CRC     8      /* u32 CRC-32 of the image being downloaded */

/* ---- Image transfer (docs/protokoll.md part 1, "Image transfer") ----
 * The AP draws the whole screen and the tag only shows it. The same pattern as the
 * firmware transfer: the AP answers a POLL that has ESL_POLL_F_IMAGE and a content_id
 * other than the image the AP wants shown with the first IMG block (after CONFIG,
 * before FW); the tag asks for the rest with IMG_REQ, keeps the bytes in its SPI flash
 * and writes them to the panel when it has them all. image_id is the CRC-32 of the
 * image bytes as sent (size bytes), so the same picture always has the same id and is
 * never sent twice; a CRC of 0 is sent as 1, since 0 means "no image". */

#define ESL_MSG_IMG          0x09   /* AP -> tag: one block of a screen image */
#define ESL_MSG_IMG_REQ      0x0A   /* tag -> AP: ask for the next block */

/* IMG, 14 byte header + up to ESL_IMG_CHUNK_MAX bytes of data.
 * next_poll_s at ESL_OFF_NEXT_POLL as in IDLE. */
#define ESL_IMG_HDR_LEN      14
#define ESL_IMG_OFF_ID       4      /* u32 image_id: CRC-32 (IEEE) of the size bytes sent, 0 sent as 1 */
#define ESL_IMG_OFF_SIZE     8      /* u16 image size in bytes (the bytes sent, so a compressed image is shorter) */
#define ESL_IMG_OFF_POS      10     /* u16 where in the image the data starts */
#define ESL_IMG_OFF_MODEL    12     /* u8 tag model the image is drawn for */
#define ESL_IMG_OFF_FMT      13     /* u8 ESL_IMG_FMT_* */
#define ESL_IMG_OFF_DATA     14
#define ESL_IMG_CHUNK_MAX    (ESL_MAX_PAYLOAD - ESL_IMG_HDR_LEN - ESL_MIC_RESERVE)   /* 92 */

/* fmt: how the image bytes are laid out. ESL_IMG_FMT_RAW is 1 bit per pixel in screen
 * coordinates, column by column as the tag's draw_column() gives it: (height + 7) / 8
 * bytes per column, byte 0 bit 7 = row 0 (the top), column 0 (the left) first; the black
 * plane and then, on models with red, the red plane. Bit 1 = ink in that plane, and a
 * pixel is never set in both. The tag's epd layer rotates, transposes and inverts it for
 * its panel. size is then exactly planes x width x ((height + 7) / 8) (4736 on the 2.9",
 * 5000 on the 1.54"). Other values are compressed images, which may be no larger than
 * the raw image; a tag refuses a format it does not know.
 *
 * ESL_IMG_FMT_RLE is the raw image run-length coded, sent only to a tag whose POLL has
 * ESL_POLL_F_IMG_RLE: a control byte c, then for c < 0x80 the next c + 1 bytes as they
 * are, for c >= 0x80 the next byte c - 0x80 + 2 times (2 to 129). It ends exactly at
 * the raw size; image_id is the CRC-32 of the coded bytes. */
#define ESL_IMG_FMT_RAW      0
#define ESL_IMG_FMT_RLE      1

/* IMG_REQ, 10 byte */
#define ESL_IREQ_LEN         10
#define ESL_IREQ_OFF_MODEL   2      /* u8 tag model */
#define ESL_IREQ_OFF_MAX     3      /* u8 most data bytes the tag wants (1..ESL_IMG_CHUNK_MAX) */
#define ESL_IREQ_OFF_ID      4      /* u32 image_id being downloaded */
#define ESL_IREQ_OFF_POS     8      /* u16 first byte the tag is missing */

/* The image: the application linked from ESL_FW_APP_BASE (page 0 holds the bootloader),
 * at most up to the layout store page. */
#define ESL_FW_APP_BASE      0x0400u
#define ESL_FW_APP_END       0xF400u
#define ESL_FW_MAX           (ESL_FW_APP_END - ESL_FW_APP_BASE)   /* 61440 */

/* The OTA file a release carries (openesl-tag-<model>-<version>-ota.bin) and the AP takes: this header,
 * then the image. Little-endian as everything else. */
#define ESL_FWF_MAGIC        0x5746534Fu   /* "OSFW" */
#define ESL_FWF_HDR_LEN      16
#define ESL_FWF_OFF_MAGIC    0      /* u32 */
#define ESL_FWF_OFF_FORMAT   4      /* u8, = ESL_FWF_FORMAT */
#define ESL_FWF_OFF_MODEL    5      /* u8 ESL_MODEL_* */
#define ESL_FWF_OFF_VER      6      /* u16 firmware version (POLL format) */
#define ESL_FWF_OFF_SIZE     8      /* u32 image size */
#define ESL_FWF_OFF_CRC      12     /* u32 CRC-32 of the image */
#define ESL_FWF_FORMAT       1

/*
 * Layoutfila slik tagen får den (binær, laget av AP-en fra JSON-fila):
 *   0 format (= ESL_LBIN_FORMAT)   1 modell (ESL_MODEL_*)   2 antall layouter n (1..16)
 *   3 antall faste tekster t (0..ESL_LBIN_TEXT_MAX)
 *   så n layouter etter hverandre: antall elementer (u8) + elementene, ESL_LE_SIZE byte hver,
 *   så t faste tekster: lengde (u8, 1..ESL_LBIN_TEXT_LEN_MAX) + Latin-1-tekst.
 * Et tekst- eller strekkodeelement med felt ESL_LE_FIELD_FIXED | i tegner fast tekst i i stedet
 * for et felt fra CONTENT. Firmware 0.1.0 kjenner ikke faste tekster og avviser slike filer.
 * Versjonen er CRC32 (IEEE, som esl_crc32 på AP-en) av hele fila, 0 regnes som 1.
 */
#define ESL_LBIN_FORMAT      1
#define ESL_LBIN_HDR_LEN     4
#define ESL_LBIN_OFF_NTEXT   3
#define ESL_LBIN_TEXT_MAX    32     /* faste tekster per fil */
#define ESL_LBIN_TEXT_LEN_MAX 64    /* byte per fast tekst */
#define ESL_LE_FIELD_FIXED   0x80   /* felt: fast tekst nr. (felt & 0x7F) i fila */
#define ESL_LBIN_MAX         768    /* største fil tagen tar imot (XRAM-buffer på tagen) */

/* Ett element, 14 byte, i samme rekkefølge som layout_elem_t i tag/src/layout.h */
#define ESL_LE_SIZE          14
#define ESL_LE_OFF_KIND      0      /* u8 ESL_LE_TEXT / ESL_LE_RECT */
#define ESL_LE_OFF_FIELD     1      /* u8 tekstfelt 0..5, eller ESL_LE_FIELD_FIXED | fast tekst */
#define ESL_LE_OFF_FONT      2      /* 3 x u8 fonter som prøves i rekkefølge, ESL_FONT_NONE avslutter */
#define ESL_LE_OFF_OPTS      5      /* u8 justering */
#define ESL_LE_OFF_LINES     6      /* u8 maks linjer, 0 = så mange boksen rommer (høyst 3) */
#define ESL_LE_OFF_COLOR     7      /* u8 */
#define ESL_LE_OFF_X         8      /* u16 */
#define ESL_LE_OFF_Y         10     /* u8 */
#define ESL_LE_OFF_W         11     /* u16 */
#define ESL_LE_OFF_H         13     /* u8 */

#define ESL_LE_TEXT          1
#define ESL_LE_RECT          2
#define ESL_LE_BARCODE       3      /* text field drawn as a barcode, bars the full height of the box */

/* A barcode element uses the text element's bytes like this (the rest as for text):
 *   ESL_LE_OFF_FONT    symbology (ESL_BC_*), the two other font bytes ESL_FONT_NONE
 *   ESL_LE_OFF_OPTS    horizontal alignment only
 *   ESL_LE_OFF_LINES   module width in pixels, 1..ESL_BC_MODULE_MAX, 0 = the widest that fits
 *   ESL_LE_OFF_COLOR   ESL_LE_BLACK
 * The box holds the code and its quiet zones. Text that cannot be encoded, or does not fit
 * the box with 1-pixel modules, draws nothing (the AP rejects such content). */
#define ESL_LE_OFF_SYM       ESL_LE_OFF_FONT
#define ESL_LE_OFF_MODULE    ESL_LE_OFF_LINES
#define ESL_BC_CODE128       0      /* ASCII 32-127; all digits are packed two per symbol (code set C) */
#define ESL_BC_EAN13         1      /* 12 digits (the check digit is added) or 13 with a correct check digit */
#define ESL_BC_COUNT         2
#define ESL_BC_MODULE_MAX    4
#define ESL_BC_MIN_H         10     /* lowest box the AP accepts */
#define ESL_BC_QUIET_128     10     /* quiet zone each side, in modules */
#define ESL_BC_QUIET_EAN_L   11
#define ESL_BC_QUIET_EAN_R   7
#define ESL_DRAW_MAX_BARCODES 1     /* = DRAW_MAX_BARCODES in tag/src/draw.h */
#define ESL_LE_LEFT          0x00   /* opts: vannrett | loddrett */
#define ESL_LE_CENTER        0x01
#define ESL_LE_RIGHT         0x02
#define ESL_LE_MIDDLE        0x04
#define ESL_LE_BOTTOM        0x08
#define ESL_LE_BLACK         0      /* farger */
#define ESL_LE_WHITE         1
#define ESL_LE_INVERT        2      /* hvit tekst i svart boks */
#define ESL_LE_PRICE_HL      3      /* som ESL_LE_INVERT hvis ESL_CF_PRICE_HIGHLIGHT, ellers svart */
#define ESL_LE_FRAME         4      /* svart hvis ESL_CF_FRAME, ellers ikke tegnet */
#define ESL_LE_RED           5      /* rød tekst eller rødt rektangel; bare modeller med rødt (has_red) */
#define ESL_LE_RED_INVERT    6      /* hvit tekst i rød boks (bare tekst); bare modeller med rødt */

#define ESL_FONT_SMALL       0      /* 17 px, Latin-1 */
#define ESL_FONT_MEDIUM      1      /* 24 px, Latin-1 */
#define ESL_FONT_LARGE       2      /* 52 px, bare siffer og " ,-.:" */
#define ESL_FONT_MID         3      /* 36 px, samme tegn som LARGE */
#define ESL_FONT_COUNT       4
#define ESL_FONT_NONE        0xFF
/* Fonthøyder i piksler (= height i tag/fonts/font_*.c), brukt av AP-en når den sjekker layouter */
#define ESL_FONT_SMALL_H     17
#define ESL_FONT_MEDIUM_H    24
#define ESL_FONT_LARGE_H     52
#define ESL_FONT_MID_H       36
#define ESL_INVERT_PAD       4      /* = LAYOUT_INVERT_PAD i tag/src/layout.h */
#define ESL_DRAW_MAX_RUNS    16     /* = DRAW_MAX_RUNS i tag/src/draw.h: tekstlinjer per skjerm */
#define ESL_DRAW_MAX_RECTS   10     /* = DRAW_MAX_RECTS: rektangler og inverterte felt per skjerm */
#define ESL_LE_MAX_LINES     3      /* = LAYOUT_MAX_LINES */

/* ---- 802.15.4 MAC-ramme (IEEE 802.15.4, offentlig standard) ---- */

#define ESL_DEFAULT_CHANNEL  25
#define ESL_DEFAULT_PAN_ID   0x4553u   /* "ES" */
#define ESL_AP_SHORT_ADDR    0x0000u

/*
 * Frame control (bit 0-2 rammetype, 3 security, 4 frame pending, 5 ACK request,
 * 6 PAN ID compression, 10-11 dst-adressemodus, 12-13 rammeversjon, 14-15 src-adressemodus).
 * Data (1), ingen ACK, PAN ID compression, versjon 0.
 */
#define ESL_FC_TAG_TO_AP     0xC841u   /* dst kort (2), src utvidet (3) */
#define ESL_FC_AP_TO_TAG     0x8C41u   /* dst utvidet (3), src kort (2) */

#define ESL_MAC_HDR_LEN      15        /* FC 2 + seq 1 + PAN 2 + adresser 8+2 */
#define ESL_MAC_FCS_LEN      2
#define ESL_MAC_MAX_FRAME    127

/* Tag -> AP: FC, seq, dst PAN, dst kort, src utvidet */
#define ESL_TA_OFF_FC        0
#define ESL_TA_OFF_SEQ       2
#define ESL_TA_OFF_PAN       3
#define ESL_TA_OFF_DST       5
#define ESL_TA_OFF_SRC       7
/* AP -> tag: FC, seq, dst PAN, dst utvidet, src kort */
#define ESL_AT_OFF_FC        0
#define ESL_AT_OFF_SEQ       2
#define ESL_AT_OFF_PAN       3
#define ESL_AT_OFF_DST       5
#define ESL_AT_OFF_SRC       13

/* ---- Little-endian-hjelpere (p er en byte-peker) ---- */

#define ESL_GET_U16(p)  ((uint16_t)((uint16_t)(p)[0] | ((uint16_t)(p)[1] << 8)))
#define ESL_GET_U32(p)  ((uint32_t)(p)[0] | ((uint32_t)(p)[1] << 8) | \
                         ((uint32_t)(p)[2] << 16) | ((uint32_t)(p)[3] << 24))
#define ESL_PUT_U16(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((uint16_t)(v) >> 8); } while (0)
#define ESL_PUT_U32(p, v) do { (p)[0] = (uint8_t)(v); (p)[1] = (uint8_t)((uint32_t)(v) >> 8); \
                               (p)[2] = (uint8_t)((uint32_t)(v) >> 16); (p)[3] = (uint8_t)((uint32_t)(v) >> 24); } while (0)

#endif /* ESL_PROTO_H */
