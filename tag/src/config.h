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
#define CFG_LOST_AP_S       7200     /* seconds without an answer before the "no AP" mark is drawn over the picture,
                                        or "No AP found" when there is none (requirement T9, #98) */
#define CFG_LONG_POLL_S     3600     /* interval once the AP has been gone for CFG_LONG_AFTER_MIN */
#define CFG_LONG_AFTER_MIN  1440     /* minutes without an answer (a day) before CFG_LONG_POLL_S */

/* Where a tag that has lost its AP looks for it (#258, docs/protokoll.md "Finding the AP"):
   after CFG_SCAN_AFTER misses in a row once on each of the first CFG_SCAN_FIRST channels,
   then on the next one per missed round, through every channel the AP can be set to.
   15, 20 and 26 are clear of WiFi channels 1, 6 and 11, like the built-in 25 (CFG_CHANNEL),
   which those rounds try already; 11 is the lowest. */
#define CFG_SCAN_CHANNELS   15, 20, 26, 11, 12, 13, 14, 16, 17, 18, 19, 21, 22, 23, 24, 25
#define CFG_SCAN_COUNT      16
#define CFG_SCAN_FIRST      4
#define CFG_SCAN_AFTER      3

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
   (1.54") blocks of 92 bytes; the rest is room for repeats. Lost answers as for FW.
   With red the raw image is twice that (103 blocks on the 2.9" with red, #255), so a model
   with red asks for twice as many and a whole screen still goes in one wake-up. An
   expression, since MODEL_HAS_RED (model.h) is known where it is used, not here. */
#define CFG_IMG_MAX_REQ     (64 * (1 + MODEL_HAS_RED))

#endif /* CONFIG_H */
