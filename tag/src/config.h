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
#define CFG_BUILTIN_AFTER   3        /* the built-in channel is tried from this miss in a row on */
#define CFG_PAN_ID          ESL_DEFAULT_PAN_ID

#define CFG_POLL_S          60       /* default interval (requirement T4) */
#define CFG_MAX_POLL_S      600      /* longest interval when the AP does not answer (requirement T11) */
#define CFG_BACKOFF_AFTER   3        /* misses in a row before the interval starts doubling */
#define CFG_LOST_AP_S       7200     /* seconds without an answer before "No AP found" is shown (requirement T9) */
#define CFG_LONG_POLL_S     3600     /* interval once the AP has been gone for CFG_LONG_AFTER_MIN */
#define CFG_LONG_AFTER_MIN  1440     /* minutes without an answer (a day) before CFG_LONG_POLL_S */

#define CFG_RX_TIMEOUT_MS   20       /* listening window after POLL (protokoll.md) */
#define CFG_RETRY_MIN_MS    2        /* random pause before one retry */
#define CFG_RETRY_MAX_MS    10

/* FW_REQ per wake-up during a firmware download (docs/tag-ota.md): about 11 KB, a
   second or two of radio. An image of 49 KB takes five wake-ups. */
#define CFG_FW_MAX_REQ      128
/* FW_REQ per wake-up that may go unanswered (after its one retry) before the round
   stops. Measured on the jig 2026-10-15: about 5 % of the AP's answers were lost, and
   stopping at the first miss made some rounds end after 2-9 blocks. */
#define CFG_FW_MAX_MISS     8
/* IMG_REQ per wake-up during an image download: a whole screen is 52 (2.9") or 55
   (1.54") blocks of 92 bytes; the rest is room for repeats. Lost answers as for FW. */
#define CFG_IMG_MAX_REQ     64

#endif /* CONFIG_H */
