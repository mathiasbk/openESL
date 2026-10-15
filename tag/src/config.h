/*
 * config.h - fixed settings for the tag firmware.
 */
#ifndef CONFIG_H
#define CONFIG_H

/* FW_VERSION (u16, sent in POLL) and FW_VERSION_STR (start screen) come from the VERSION file
   at the top of the repository: tools/build.py passes them with -D. 1.2.3 is sent as 10203. */
#if !defined(FW_VERSION) || !defined(FW_VERSION_STR)
#error "FW_VERSION and FW_VERSION_STR are set by tools/build.py from the VERSION file"
#endif

#define CFG_CHANNEL         ESL_DEFAULT_CHANNEL
#define CFG_PAN_ID          ESL_DEFAULT_PAN_ID

#define CFG_POLL_S          60       /* default interval (requirement T4) */
#define CFG_MAX_POLL_S      600      /* longest interval when the AP does not answer (requirement T11) */
#define CFG_BACKOFF_AFTER   3        /* misses in a row before the interval starts doubling */
#define CFG_LOST_AP_S       7200     /* seconds without an answer before "No AP found" is shown (requirement T9) */

#define CFG_RX_TIMEOUT_MS   20       /* listening window after POLL (protokoll.md) */
#define CFG_RETRY_MIN_MS    2        /* random pause before one retry */
#define CFG_RETRY_MAX_MS    10

/* Layout download: LAYOUT_REQ messages per round at most. A full file (ESL_LBIN_MAX = 768
   bytes in 94-byte blocks) needs 8 after the first block; the rest is room for repeats.
   An unfinished download continues from where it stopped in a later round. */
#define CFG_LAYOUT_MAX_REQ  16
/* FW_REQ per wake-up during a firmware download (docs/tag-ota.md): about 11 KB, a
   second or two of radio. An image of 49 KB takes five wake-ups. */
#define CFG_FW_MAX_REQ      128

#endif /* CONFIG_H */
