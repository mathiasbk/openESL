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
/* 0x03, 0x04 and 0x05 were CONTENT, LAYOUT and LAYOUT_REQ, used while the tag drew the
 * screen itself (before the AP drew images). They are not used again. */
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
#define ESL_POLL_OFF_MODEL   11     /* u8 tagmodell (krav T18); byten var reservert (0) i eldre firmware */

#define ESL_POLL_F_BOOTED    0x01   /* nettopp startet */
#define ESL_POLL_F_DRAWFAIL  0x02   /* forrige tegning feilet */
/* 0x04 was LAYOUTFAIL (the tag rejected the layout file); not used again */
#define ESL_POLL_F_OTA       0x08   /* the tag takes firmware over the air (FW / FW_REQ below) */
#define ESL_POLL_F_FWFAIL    0x10   /* the tag rejected or rolled back the firmware version the AP offers */
#define ESL_POLL_F_IMAGE     0x20   /* the tag shows images drawn by the AP (IMG / IMG_REQ below); content_id
                                     * in its POLL is then the image_id it shows. A tag without it (no SPI
                                     * flash) cannot show content. */
#define ESL_POLL_F_IMG_RLE   0x40   /* the tag also takes ESL_IMG_FMT_RLE images */
#define ESL_POLL_F_IMG_2D    0x80   /* ... and ESL_IMG_FMT_2D images */
/* The flags byte is full: every bit is taken, 0x04 included (retired, and kept unused
 * since older firmware set it). A new capability needs a field of its own after cfg_ver. */

/* Utvidet POLL: firmware som forstår CONFIG legger til cfg_ver etter de 12
 * første bytene. AP-en sender CONFIG bare til tager som sender dette feltet,
 * så eldre tager (12 byte) aldri får en meldingstype de ikke kjenner. */
#define ESL_POLL_OFF_CFG_VER 12     /* u16 cfg_ver tagen bruker nå (0 = innebygde standardverdier) */
#define ESL_POLL_LEN_CFG     14     /* POLL med cfg_ver */

/* The link block after cfg_ver: how the tag hears the AP (docs/protokoll.md, "Signal").
 * The AP keeps it with its own RSSI of the POLL as the tag's signal history. Firmware
 * that downloaded layout files sent 18 bytes with a layout file version at 14-17; no
 * tag runs that any more. */
#define ESL_POLL_OFF_RX_RSSI 14     /* u8 the radio's raw RSSI byte for the AP's last answer the tag took
                                     * (scale not measured yet; higher = stronger), 0 = none since start */
#define ESL_POLL_OFF_RX_LQI  15     /* u8 LQI of that answer (0..127), 0 = none */
#define ESL_POLL_OFF_LOST    16     /* u8 poll rounds in a row with no answer before this one (255 = 255 or more) */
#define ESL_POLL_OFF_LINKF   17     /* u8 ESL_POLL_LF_* */
#define ESL_POLL_LEN_LINK    18     /* POLL with cfg_ver and the link block */

#define ESL_POLL_LF_RETRY    0x01   /* the round before this one was answered only on its second try (the retry itself is sent unchanged) */

/* Tagmodell i POLL. 0 er det eldre firmware sender i den reserverte byten,
 * så AP-en leser den som "ukjent" og alt virker som før. De kjente modellene
 * (ESL_MODEL_29, ESL_MODEL_154, ..., ESL_MODEL_COUNT, skjermstørrelsene og
 * ESL_MODEL_LIST) kommer fra modellregisteret shared/models.json. */
#define ESL_MODEL_UNKNOWN    0
#include "models_gen.h"

/* IDLE, 4 byte */
#define ESL_IDLE_LEN         4
#define ESL_OFF_NEXT_POLL    2      /* u16 sekunder til neste POLL (0 = behold), også i CONFIG, FW og IMG */

/* CONFIG, 18 byte (12 before the long backoff, 16 before the channel; a tag reads missing fields as 0): taginnstillinger. Samme next_poll_s på byte 2 som IDLE.
 * Hver innstilling er u16, 0 = tagens innebygde standardverdi (tag/src/config.h).
 * Nye innstillinger legges bare til på slutten: tagen leser feltene den kjenner
 * og hopper over resten, og et felt som mangler i en kortere melding er 0. */
#define ESL_CONFIG_LEN       18
#define ESL_CONFIG_OFF_VER   4      /* u16 cfg_ver; tagen sender den tilbake i POLL som kvittering */
#define ESL_CONFIG_OFF_POLL  6      /* u16 vanlig intervall i sekunder (krav T4) */
#define ESL_CONFIG_OFF_MAX_POLL 8   /* u16 lengste intervall uten svar fra AP-en (krav T11) */
#define ESL_CONFIG_OFF_LOST_AP 10   /* u16 sekunder uten svar før "No AP found" (krav T9) */
#define ESL_CONFIG_OFF_LONG_POLL 12 /* u16 interval in seconds once the AP has been gone for long_after_min */
#define ESL_CONFIG_OFF_LONG_AFTER 14 /* u16 minutes without an answer before long_poll_s, ESL_CFG_LONG_NEVER = never */
#define ESL_CONFIG_OFF_CHANNEL 16   /* u16 the AP's channel, now or after a planned move (#256);
                                     * 0 = ESL_DEFAULT_CHANNEL. The tag stays where it hears the
                                     * AP, and tries this channel first when the AP stops answering
                                     * (docs/protokoll.md, "Changing the channel") */

#define ESL_CFG_POLL_MIN_S   10     /* grenser for poll_s og max_poll_s */
#define ESL_CFG_POLL_MAX_S   3600
#define ESL_CFG_LOST_AP_MIN_S 60    /* grenser for lost_ap_s */
#define ESL_CFG_LOST_AP_NEVER 0xFFFFu /* lost_ap_s: vis aldri "No AP found" */
#define ESL_CFG_LONG_POLL_MIN_S 60  /* lower limit of long_poll_s (no upper one: 65535 s is 18 h) */
#define ESL_CFG_LONG_AFTER_MIN_MIN 10 /* lower limit of long_after_min */
#define ESL_CFG_LONG_NEVER   0xFFFFu /* long_after_min: never go to long_poll_s */

#define ESL_MAX_PAYLOAD      110    /* 127 - 15 (MAC-hode) - 2 (FCS) */
#define ESL_MIC_RESERVE      4      /* holdt av til framtidig MIC (krav T16) */

/* ---- What a tag shows (the AP's content; not sent on the air as such) ----
 * The AP draws the content into an image (IMG below). These are the built-in layouts,
 * the flags and the text limits of a tag's content. Content is only kept on the AP (NVS),
 * so the limits are the AP's: up to 0.9.2 they were those of the CONTENT message the tag
 * used to get (6 fields, 96 bytes). Content within those is stored, and gets its
 * content_id, exactly as before (#182). */
#define ESL_LAYOUT_PRICE     1      /* felt: navn, pris, undertekst */
#define ESL_LAYOUT_TEXT      2      /* felt: linje1..linje4 */
#define ESL_LAYOUT_FILE      0x10   /* 0x10 + n = layout nr. n i layoutfila for modellen */
#define ESL_LAYOUT_FILE_MAX  16     /* høyst 16 layouter i en fil: 0x10..0x1F */

#define ESL_CF_PRICE_HIGHLIGHT 0x01 /* framhevet pris: hvite siffer i svart felt (invertert) */
#define ESL_CF_FRAME         0x02   /* svart ramme rundt skjermen */

#define ESL_MAX_FIELDS       16     /* tekstfelt (6 til og med 0.9.2) */
/* Plass til feltene: tekst + én lengdebyte per felt, 256 (96 til og med 0.9.2).
 * Ett felt har høyst 255 byte tekst. */
#define ESL_CONTENT_FIELD_SPACE 256
/* Største sum av tekstbytes med n felt */
#define ESL_MAX_TEXT_FOR(n)  (ESL_CONTENT_FIELD_SPACE - (n))
/* Største sum av tekstbytes når alle felt er med: 240 */
#define ESL_MAX_TEXT_BYTES   ESL_MAX_TEXT_FOR(ESL_MAX_FIELDS)

/* ---- Firmware transfer (docs/protokoll.md part 1, "Firmware transfer"; docs/tag-ota.md) ----
 * The AP answers a POLL that has ESL_POLL_F_OTA, from a tag whose model has an image on
 * the AP and whose firmware version differs, with the first FW block (after CONFIG and
 * images are done). The tag asks for the rest with FW_REQ. The data is the application
 * image, linked at ESL_FW_APP_BASE. */

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
 * the raw size; image_id is the CRC-32 of the coded bytes.
 *
 * ESL_IMG_FMT_2D is the raw image coded column against column, modelled on CCITT T.6
 * (G4) with its own run codes, sent only to a tag whose POLL has ESL_POLL_F_IMG_2D. It
 * is a bit string, most significant bit first, and columns follow each other with no
 * padding: every column of the raw image (the red plane's after the black plane's),
 * height pixels each, coded against the column before it (the first against an empty
 * one). Ink is colour 1. In a column, a change is a row y whose pixel differs from row
 * y - 1 (row -1 has no ink). Coding goes down the column from a0 = -1 with colour 0: b1
 * is the first change in the reference column below a0 to the other colour than the
 * current one, b2 the change after b1 (height if none), and from s = max(a0, 0):
 *   1, 011, 010, 000011, 000010, 0000011, 0000010  vertical: the next change a1 is at b1,
 *       b1 + 1, b1 - 1, b1 + 2, b1 - 2, b1 + 3, b1 - 3; rows s..a1 - 1 get the colour,
 *       the colour flips and a0 = a1
 *   0001  pass: rows s..b2 - 1 get the colour, a0 = b2
 *   001 r1 r2  horizontal: r1 rows of the colour from s, then r2 of the other, a0 = s + r1
 *       + r2; r1 and r2 are Exp-Golomb codes of order 2 (r + 4 in binary, b bits, after
 *       b - 3 zeros)
 * until a0 is the height. After the last column the last byte is filled with zeros, and
 * nothing follows. Decoded it is exactly the raw image; image_id is the CRC-32 of the
 * coded bytes. */
#define ESL_IMG_FMT_RAW      0
#define ESL_IMG_FMT_RLE      1
#define ESL_IMG_FMT_2D       2

/* IMG_REQ, 10 byte */
#define ESL_IREQ_LEN         10
#define ESL_IREQ_OFF_MODEL   2      /* u8 tag model */
#define ESL_IREQ_OFF_MAX     3      /* u8 most data bytes the tag wants (1..ESL_IMG_CHUNK_MAX) */
#define ESL_IREQ_OFF_ID      4      /* u32 image_id being downloaded */
#define ESL_IREQ_OFF_POS     8      /* u16 first byte the tag is missing */

/* The image: the application linked from ESL_FW_APP_BASE (page 0 holds the bootloader),
 * at most up to page 0x3D (0xF400), which is free; the settings and the stock MAC follow. */
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
 * Layoutfila kompilert (binær, laget av AP-en fra JSON-fila; esl_render.c tegner med den):
 *   0 format (= ESL_LBIN_FORMAT)   1 modell (ESL_MODEL_*)   2 antall layouter n (1..16)
 *   3 antall faste tekster t (0..ESL_LBIN_TEXT_MAX)   4 antall ikoner i (0..ESL_LBIN_ICON_MAX)
 *   så n layouter etter hverandre: antall elementer (u8) + elementene, ESL_LE_SIZE byte hver,
 *   så t faste tekster: lengde (u8, 1..ESL_LBIN_TEXT_LEN_MAX) + Latin-1-tekst,
 *   så i ikoner: bredde (u16), høyde (u8), så bildet som et skjermbilde (ESL_IMG_FMT_RAW):
 *   bredde kolonner fra venstre, (høyde + 7) / 8 byte hver, bit 7 i byte 0 = øverste rad,
 *   1 = blekk.
 * Et tekst- eller strekkodeelement med felt ESL_LE_FIELD_FIXED | i tegner fast tekst i i stedet
 * for et tekstfelt.
 * Versjonen er CRC32 (IEEE, som esl_crc32 på AP-en) av hele fila, 0 regnes som 1.
 */
#define ESL_LBIN_FORMAT      2      /* 2: ikoner, linjer og runde/åpne rektangler */
#define ESL_LBIN_HDR_LEN     5
#define ESL_LBIN_OFF_NTEXT   3
#define ESL_LBIN_OFF_NICON   4
#define ESL_LBIN_TEXT_MAX    32     /* faste tekster per fil */
#define ESL_LBIN_TEXT_LEN_MAX 64    /* byte per fast tekst */
#define ESL_LBIN_ICON_MAX    16     /* ikoner per fil */
#define ESL_LBIN_ICON_HDR    3      /* u16 bredde, u8 høyde */
#define ESL_ICON_MAX_W       128    /* største ikon i piksler */
#define ESL_ICON_MAX_H       128
#define ESL_LE_FIELD_FIXED   0x80   /* felt: fast tekst nr. (felt & 0x7F) i fila */
/* Største kompilerte fil (AP-ens RAM; 8 KB JSON fyller seg først). Denne og ESL_DRAW_MAX_RUNS,
 * _RECTS, _LINES og _ICONS er AP-kortets grenser: et kort kan sette andre i "limits" i
 * ap/boards/<kort>/board.json (ap/CMakeLists.txt), og verdiene her er standarden (ESP32-C6).
 * ESL_*_CAP er plassen koden setter av; den er grensen, unntatt i nettsidens renderer, som
 * bygges med plass til det største kortet og tar grensene til AP-en den snakker med
 * (esl_limits i esl_layout.h). */
#ifndef ESL_LBIN_MAX
#define ESL_LBIN_MAX         4096
#endif
#ifndef ESL_LBIN_CAP
#define ESL_LBIN_CAP         ESL_LBIN_MAX
#endif

/* Ett element, 14 byte, slik esl_render.c leser det */
#define ESL_LE_SIZE          14
#define ESL_LE_OFF_KIND      0      /* u8 ESL_LE_TEXT / ESL_LE_RECT */
#define ESL_LE_OFF_FIELD     1      /* u8 tekstfelt 0..ESL_MAX_FIELDS-1, eller ESL_LE_FIELD_FIXED | fast tekst */
#define ESL_LE_OFF_FONT      2      /* 3 x u8 fonter som prøves i rekkefølge, ESL_FONT_NONE avslutter */
#define ESL_LE_OFF_OPTS      5      /* u8 justering */
#define ESL_LE_OFF_LINES     6      /* u8 maks linjer, 0 = så mange boksen rommer (høyst 3); tekst: bit 6-7 = ESL_LE_ROT */
#define ESL_LE_OFF_COLOR     7      /* u8 */
#define ESL_LE_OFF_X         8      /* u16 */
#define ESL_LE_OFF_Y         10     /* u8 */
#define ESL_LE_OFF_W         11     /* u16 */
#define ESL_LE_OFF_H         13     /* u8 */

/* Tallformat for et tekstelement, i bit 4-7 av ESL_LE_OFF_OPTS (0 = teksten som sendt).
 * Bit 4-6 er hvor mange desimaler, bit 7 tusenskille (mellomrom). Teksten må være et tall
 * ("1299.9", "1299,90", "1 299,90", "199,-"); annen tekst tegnes uendret. Desimaltegnet er komma.
 * Et formatert tall tegnes alltid på én linje. */
#define ESL_LE_NUM_MASK      0x70
#define ESL_LE_NUM_KEEP      0x10   /* desimalene som sendt */
#define ESL_LE_NUM_0         0x20   /* ingen desimaler, avrundet */
#define ESL_LE_NUM_2         0x30   /* alltid to desimaler */
#define ESL_LE_NUM_AUTO      0x40   /* to desimaler, ingen når de er ,00 */
#define ESL_LE_NUM_DASH      0x50   /* to desimaler, ",-" når de er ,00 */
#define ESL_LE_NUM_0_DASH    0x60   /* ingen desimaler, avrundet, med ",-" */
#define ESL_LE_NUM_THOUSANDS 0x80   /* mellomrom mellom hver tredje siffer: 1 299,90 */
#define ESL_LE_NUM_MAX_LEN   24     /* lengste formaterte tall: -, 12 siffer, 3 mellomrom, komma, 6 desimaler */
#define ESL_DRAW_MAX_NUMS    8      /* tekstelementer med tallformat per layout (esl_render.c) */

#define ESL_LE_TEXT          1
#define ESL_LE_RECT          2
#define ESL_LE_BARCODE       3      /* text field drawn as a barcode, bars the full height of the box */
#define ESL_LE_LINE          4      /* a straight line between two corners of the box */
#define ESL_LE_ICON          5      /* an icon from the file, its size is the box */
#define ESL_LE_QR            6      /* text field drawn as a QR code (ISO/IEC 18004, byte mode) */

/* A rect element also uses (0 in a plain filled rect, so older files compile the same):
 *   ESL_LE_OFF_RADIUS  corner radius in pixels, at most half the shorter side
 *   ESL_LE_OFF_BORDER  0 = filled, else only a border this many pixels wide
 * A line element:
 *   ESL_LE_OFF_OPTS    ESL_LE_RISING: from the bottom-left to the top-right corner of the box,
 *                      else from the top-left to the bottom-right
 *   ESL_LE_OFF_WIDTH   line width in pixels, 1..ESL_LINE_W_MAX
 *   a box 1 pixel high or wide is a straight line: exactly width pixels thick, (width - 1) / 2
 *   of them above or left of the box, with square ends. A slanted line is every pixel whose
 *   centre is at most width / 2 from it, so it reaches width / 2 past the box, with round ends
 * An icon element:
 *   ESL_LE_OFF_FIELD   the icon's number in the file, or ESL_LE_ICON_FIELD | a text field: the
 *                      icon whose name is that field's text, centred in the box (an unknown name,
 *                      or an icon bigger than the box, draws nothing)
 *   ESL_LE_OFF_NAMES   with ESL_LE_ICON_FIELD: the fixed text that holds the name of icon 0;
 *                      icon i's name is the fixed text after it by i
 *   ESL_LE_OFF_COLOR   black, white or red: the icon's 1 bits are drawn, 0 bits left as they are */
#define ESL_LE_OFF_RADIUS    ESL_LE_OFF_LINES
#define ESL_LE_OFF_BORDER    ESL_LE_OFF_OPTS
#define ESL_LE_OFF_WIDTH     ESL_LE_OFF_LINES
#define ESL_LE_RISING        0x01
#define ESL_LE_ICON_FIELD    0x40
#define ESL_LE_OFF_NAMES     ESL_LE_OFF_FONT
#define ESL_LINE_W_MAX       16

/* A QR element uses the text element's bytes like this (the rest as for text):
 *   ESL_LE_OFF_ECC     error correction level ESL_QR_LEVEL_L..H
 *   ESL_LE_OFF_OPTS    alignment in the box, as for text
 *   ESL_LE_OFF_MODULE  module size in pixels, 1..ESL_QR_MODULE_MAX, 0 = the largest that fits
 *   ESL_LE_OFF_COLOR   ESL_LE_BLACK
 * The code takes the smallest version (1..10) that holds the field. The box holds the code
 * and a white quiet zone of ESL_QR_QUIET modules on every side, which is drawn white, so
 * phones read it on any background. A field that does not fit the box with 1-pixel modules
 * draws nothing (the AP rejects such content). */
#define ESL_LE_OFF_ECC       ESL_LE_OFF_FONT
#define ESL_QR_LEVEL_L       0
#define ESL_QR_LEVEL_M       1
#define ESL_QR_LEVEL_Q       2
#define ESL_QR_LEVEL_H       3
#define ESL_QR_MODULE_MAX    8
#define ESL_QR_QUIET         2u     /* modules of white round the code, inside the box */
#define ESL_QR_MIN_SIDE      (21 + 2 * ESL_QR_QUIET) /* version 1 with 1-pixel modules */

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
#define ESL_DRAW_MAX_BARCODES 1     /* strekkoder per skjerm (esl_render.c) */
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
#define ESL_FONT_TEXT20      4      /* 20 px, Latin-1 (AP-en; tagen tegner den ikke) */
#define ESL_FONT_PRICE44     5      /* 44 px, samme tegn som LARGE (AP-en; tagen tegner den ikke) */
#define ESL_FONT_COUNT       6
#define ESL_FONT_NONE        0xFF
/* Fonthøyder i piksler (= height i tag/fonts/font_*.c), brukt av AP-en når den sjekker layouter */
#define ESL_FONT_SMALL_H     17
#define ESL_FONT_MEDIUM_H    24
#define ESL_FONT_LARGE_H     52
#define ESL_FONT_MID_H       36
#define ESL_FONT_TEXT20_H    20
#define ESL_FONT_PRICE44_H   44
#define ESL_INVERT_PAD       4      /* rammen rundt et invertert felt (esl_render.c) */
/* Per skjerm (esl_render.c); per AP-kort, som ESL_LBIN_MAX */
#ifndef ESL_DRAW_MAX_RUNS
#define ESL_DRAW_MAX_RUNS    32     /* tekstlinjer */
#endif
#ifndef ESL_DRAW_MAX_RECTS
#define ESL_DRAW_MAX_RECTS   20     /* rektangler og inverterte felt */
#endif
#ifndef ESL_DRAW_MAX_LINES
#define ESL_DRAW_MAX_LINES   16     /* linjer */
#endif
#ifndef ESL_DRAW_MAX_ICONS
#define ESL_DRAW_MAX_ICONS   8      /* ikoner */
#endif
#ifndef ESL_DRAW_CAP_RUNS
#define ESL_DRAW_CAP_RUNS    ESL_DRAW_MAX_RUNS
#endif
#ifndef ESL_DRAW_CAP_RECTS
#define ESL_DRAW_CAP_RECTS   ESL_DRAW_MAX_RECTS
#endif
#ifndef ESL_DRAW_CAP_LINES
#define ESL_DRAW_CAP_LINES   ESL_DRAW_MAX_LINES
#endif
#ifndef ESL_DRAW_CAP_ICONS
#define ESL_DRAW_CAP_ICONS   ESL_DRAW_MAX_ICONS
#endif
#define ESL_DRAW_MAX_QR      1      /* QR-koder per skjerm (esl_render.c) */
#define ESL_LE_MAX_LINES     3
/* A text element turned clockwise by 90 degrees times bits 6-7 of ESL_LE_OFF_LINES (0 in older
 * files, so they compile and draw the same). The box keeps its screen coordinates; the text is
 * laid out in the box turned with it: at 90 and 270 degrees the lines run along h and are
 * stacked across w, so "lines", the fonts and the alignment work along the turned box. At 90
 * the text reads top to bottom, at 270 bottom to top, at 180 upside down. */
#define ESL_LE_ROT_SHIFT     6
#define ESL_LE_ROT_MASK      0xC0
#define ESL_LE_LINES_MASK    0x3F

/* ---- 802.15.4 MAC-ramme (IEEE 802.15.4, offentlig standard) ---- */

#define ESL_DEFAULT_CHANNEL  25
#define ESL_CHANNEL_MIN      11     /* the 2.4 GHz channels of 802.15.4 */
#define ESL_CHANNEL_MAX      26
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
