/*
 * sim.h - the tag firmware and the AP core on one PC, joined by a simulated radio link.
 *
 * The tag side (sim_tag.c) is tag/src/app.c with all its logic (POLL, CONFIG, image
 * download, drawing, flash) on a HAL that sends its frames to the AP side instead
 * of a radio. The AP side (sim_ap.c) is ap/components/esl_core as the C6 runs it: the
 * table of tags, the layout files, esl_radio_handle() for every frame and esl_api_handle()
 * for every API request, with "NVS" kept in RAM so the AP can be restarted.
 *
 * Time is simulated: sim_now_ms only moves when the tag sleeps, waits or listens, so an
 * hour without the AP takes milliseconds. The two sides only share this header, because
 * the tag code (C99, SDCC keywords) and the AP code have headers that do not mix.
 */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Simulated time since the simulation started */
extern uint64_t sim_now_ms;

/* ---- AP (sim_ap.c) ---- */

#define SIM_TOKEN "sim"   /* bearer token the API accepts */

/* Starts (or restarts) the AP: an empty table, then everything "NVS" has, as after a
   power cycle. The first call also clears NVS. */
void sim_ap_boot(void);
/* The channel in the AP's NVS (0 = none, menuconfig's), as if set without telling the tags:
   it is on it from its next boot (#258) */
void sim_ap_set_nvs_channel(uint8_t channel);
/* Switches the AP off: frames from the tag get no answer, the API answers 503. */
void sim_ap_off(void);
bool sim_ap_up(void);
/* The channel the AP's radio is on, after a planned move that is due (as its radio task) */
uint8_t sim_ap_channel(void);

/* One API request through esl_api_handle(), as the HTTP server and the serial port
   pass it on. body may be NULL. Returns the HTTP status; *resp (may be NULL) gets
   the JSON body, valid until the next call. */
int sim_api(const char *method, const char *uri, const char *body, const char **resp);

/* The same with a body of len bytes that need not be text (PUT /api/tag-firmware). */
int sim_api_bin(const char *method, const char *uri, const void *body, size_t len, const char **resp);

/* Number and bool from the top level of the last JSON reply (-1 if missing or null). */
long sim_json_num(const char *key);
int sim_json_bool(const char *key);

/* ---- drawing on the AP (esl_render) against drawing on the tag ---- */

/* Compiles a layout file (JSON) for the tag's model with the AP's compiler and keeps it
   for sim_ap_render. Returns the number of layouts, or 0 with the message in err. */
int sim_ap_compile(const char *json, size_t len, char *err, size_t errlen);
/* The AP draws content (layout 1 = price, 2 = text, 16 + n = layout n of the compiled file;
   n UTF-8 fields) for the tag's model into out. Returns the image size, 0 on error. */
size_t sim_ap_render(uint8_t layout, uint8_t flags, int n, const char *const *fields, uint8_t *out, size_t cap);
/* The AP draws what it holds for the tag mac (its content against the model's layout file). */
size_t sim_ap_render_tag(uint64_t mac, uint8_t *out, size_t cap);

/* ---- radio link ---- */

/* The tag sends frame (without FCS) on channel. Returns the length of the AP's reply in
   reply (without FCS), or 0 if nothing comes back: AP off or on another channel, or a
   loss below. */
uint8_t sim_link(uint8_t channel, const uint8_t *frame, uint8_t len, uint8_t *reply);

/* Losses: drop the next n frames the tag sends (the AP never sees them), or the next n
   replies of message type (ESL_MSG_*, 0 = any) after the AP has handled the frame,
   the way an AP that dies in the middle of an exchange would leave it. */
void sim_drop_uplink(int n);
void sim_drop_reply(uint8_t msg_type, int n);
/* The AP switches off when it drops a reply (it died in the middle of the exchange). */
extern bool sim_die_on_drop;

typedef struct {
    unsigned polls, img_reqs;               /* frames from the tag the AP handled */
    unsigned idle, config;                  /* replies the tag received */
    unsigned image;                         /* image transfers started (IMG as the answer to a POLL) */
    unsigned unanswered;                    /* frames with no reply (AP off or lost) */
} sim_stats_t;
extern sim_stats_t sim_stats;

/* ---- tag (sim_tag.c) ---- */

extern uint64_t sim_tag_mac;     /* as in the API: 00000123456789A8 */
extern const char *sim_tag_model; /* "2.9" or "1.54" */
extern bool sim_verbose;          /* print every exchange */

void sim_tag_flash_erase(void);   /* a new tag: nothing in flash */
/* Power-on: app_init() (start screen or the content from flash), then the first poll at once. */
void sim_tag_boot(void);
/* Runs the tag until time t_ms: every wake-up that falls before it, then sim_now_ms = t_ms. */
void sim_tag_run_until(uint64_t t_ms);
/* Runs until the tag has drawn something new or t_ms has passed. True if it drew. */
bool sim_tag_run_until_draw(uint64_t t_ms);

uint32_t sim_tag_shown_id(void); /* content_id on the screen, 0 = none */
bool sim_tag_has_key(void);      /* paired: it has a radio key (sec.h) */
int sim_tag_draws(void);         /* times the display was drawn */
uint16_t sim_tag_interval_s(void);
uint16_t sim_tag_last_sleep_s(void);
uint8_t sim_tag_misses(void);
uint8_t sim_tag_channel(void);   /* the channel its radio is on (radio_init) */
uint64_t sim_tag_next_wake_ms(void);
uint64_t sim_tag_last_draw_ms(void);
/* Firmware update (docs/tag-ota.md): the tag's firmware version and model byte as in
   POLL, how often it restarted itself (into the bootloader, which then ran), whether
   the chip holds img (len bytes) as its application, and whether the update state in
   its SPI flash says the running image was confirmed. */
uint16_t sim_tag_fw_version(void);
uint8_t sim_tag_model_code(void);
int sim_tag_restarts(void);
bool sim_tag_app_is(const uint8_t *img, size_t len);
bool sim_tag_ota_confirmed(void);
/* The screen as the tag drew it last, in the image format of esl_render.h. Returns the size. */
size_t sim_tag_screen(uint8_t *out, size_t cap);
/* The screen as PNG (2x, black/white; red as grey). The tag is drawn in the orientation of the layouts. */
bool sim_tag_save_png(const char *path);
/* Called after every wake-up with a new drawing (NULL = none). */
extern void (*sim_on_draw)(void);

/* ---- PNG (png.c) ---- */
/* Grey image, 1 byte per pixel, written as an 8-bit greyscale PNG. */
bool png_write_gray(const char *path, const uint8_t *pix, int w, int h);

#endif
