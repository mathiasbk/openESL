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

/* Felles start på alle meldinger */
#define ESL_OFF_TYPE         0
#define ESL_OFF_VER          1

/* POLL, 12 byte */
#define ESL_POLL_LEN         12
#define ESL_POLL_OFF_FW      2      /* u16 firmwareversjon */
#define ESL_POLL_OFF_BATT    4      /* u16 batterispenning i mV */
#define ESL_POLL_OFF_CID     6      /* u32 content_id som vises nå (0 = ingenting) */
#define ESL_POLL_OFF_FLAGS   10     /* u8 */
#define ESL_POLL_OFF_RSVD    11     /* u8, alltid 0 */

#define ESL_POLL_F_BOOTED    0x01   /* nettopp startet */
#define ESL_POLL_F_DRAWFAIL  0x02   /* forrige tegning feilet */

/* IDLE, 4 byte */
#define ESL_IDLE_LEN         4
#define ESL_OFF_NEXT_POLL    2      /* u16 sekunder til neste POLL (0 = behold), også i CONTENT */

/* CONTENT, 10 byte hode + tekstfelt */
#define ESL_CONTENT_HDR_LEN  10
#define ESL_CONTENT_OFF_CID  4      /* u32 ny content_id */
#define ESL_CONTENT_OFF_LAYOUT 8    /* u8 */
#define ESL_CONTENT_OFF_FLAGS  9    /* u8 */
#define ESL_CONTENT_OFF_FIELDS 10   /* felt: lengde (u8) + Latin-1-tekst, inntil 4 */

#define ESL_LAYOUT_PRICE     1      /* felt: navn, pris, undertekst */
#define ESL_LAYOUT_TEXT      2      /* felt: linje1..linje4 */

#define ESL_CF_PRICE_RED     0x01   /* rød pris */
#define ESL_CF_RED_FRAME     0x02   /* rød ramme/stripe */

#define ESL_MAX_FIELDS       4
#define ESL_MAX_PAYLOAD      110    /* 127 - 15 (MAC-hode) - 2 (FCS) */
#define ESL_MIC_RESERVE      4      /* holdt av til framtidig MIC (krav T16) */
/* Største sum av tekstbytes AP-en kan sende når alle fire felt er med: 92 */
#define ESL_MAX_TEXT_BYTES   (ESL_MAX_PAYLOAD - ESL_CONTENT_HDR_LEN - ESL_MAX_FIELDS - ESL_MIC_RESERVE)

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
