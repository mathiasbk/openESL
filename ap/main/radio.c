#include <stdio.h>
#include "app.h"
#include "esl_link.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "radio.h"
#include "sdkconfig.h"

/* The radio task (docs/history/ap-firmware-plan.md, Radio), the same for every link: the
 * driver (radio_ieee802154.c, ...) puts each received frame on a queue, and a
 * high-priority task hands it to esl_link_handle() (parse, look up the
 * ready-made reply, build the frame; tested on the PC), which sends the reply
 * with radio_link.send. Target: under 5 ms from received POLL to transmit
 * (requirement A3 in docs/requirements.md); the worst time seen is logged.
 *
 * The task is registered with the task watchdog and wakes at least once a
 * second to feed it, so a radio task stuck in a lock or the driver restarts
 * the AP (sdkconfig.defaults: CONFIG_ESP_TASK_WDT_PANIC). */

static const char *TAG = "radio";

/* Bool Kconfig symbols are undefined when off, so give the sniffer choice a 0/1 value */
#ifdef CONFIG_ESL_RADIO_SNIFFER
#define SNIFFER 1
#else
#define SNIFFER 0
#endif

#if SNIFFER
#include <string.h>
#include "sdkconfig.h"
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#define SNIFF_COMMANDS 1 /* "ch 20", "hop 300", "status" on the USB port (sniff_command) */
#else
#define SNIFF_COMMANDS 0
#endif
#define HOP_HOLD_US (10 * 1000000LL) /* stay on a channel this long after a frame */
static unsigned hop_ms = CONFIG_ESL_SNIFFER_HOP_MS; /* 0 = stay on one channel */
static uint8_t sniff_channel;
#endif

/* A SoluM remote as a button (remote.c, esl_remote.h): the C6's own radio hears it, and
   visits channel 11 for it while the AP runs on another channel */
#if !SNIFFER && CONFIG_ESL_RADIO_DRIVER_IEEE802154
#define REMOTE 1
#include "esp_timer.h"
#else
#define REMOTE 0
#endif

static QueueHandle_t rx_queue;
static esl_store_t *store;
static esl_layouts_t *layouts;
static uint16_t pan;

bool IRAM_ATTR radio_rx_from_isr(const esl_link_rx_t *f)
{
    BaseType_t woken = pdFALSE;
    bool ok = xQueueSendFromISR(rx_queue, f, &woken) == pdTRUE;
    if (!ok)
        radio_link.stats.dropped++;
    portYIELD_FROM_ISR(woken);
    return ok;
}

bool radio_rx(const esl_link_rx_t *f)
{
    bool ok = xQueueSend(rx_queue, f, 0) == pdTRUE;
    if (!ok)
        radio_link.stats.dropped++;
    return ok;
}

static int64_t clock_us(void)
{
    return esp_timer_get_time();
}

#if SNIFFER
/*
 * One line per frame, for tools/sniffer/sniff.py (docs/radio-sniffer.md):
 *
 *   @SNF <rx_us> <channel> <rssi> <lqi> <frame in hex, without the FCS>
 *
 * rx_us is when the frame's last byte was in, on the AP's clock (microseconds since it
 * started), so the gap between two lines is the gap on the air plus the second frame's
 * air time. The driver writes RSSI and LQI where the FCS was; a frame with a wrong FCS
 * never gets here (the radio drops it). One fwrite, so a log line cannot land inside it.
 */
static void sniff_line(const esl_link_rx_t *f)
{
    static char line[48 + 2 * ESL_PSDU_MAX];
    unsigned n = f->len >= 2 ? f->len - 2u : 0;
    int p = snprintf(line, sizeof(line), "@SNF %lld %u %d %u ", (long long)f->rx_us, f->channel, f->rssi,
                     f->len >= 2 ? f->psdu[f->len - 1] : 0);
    for (unsigned i = 0; i < n && p + 3 < (int)sizeof(line); i++)
        p += snprintf(&line[p], sizeof(line) - p, "%02X", f->psdu[i]);
    line[p++] = '\n';
    fwrite(line, 1, p, stdout);
    fflush(stdout);
}

static void sniff_status(void)
{
    const esl_link_stats_t *s = &radio_link.stats;
    printf("@SNF-ST ch %u hop %u rx %lu dropped %lu\n", sniff_channel, hop_ms, (unsigned long)s->rx,
           (unsigned long)s->dropped);
    fflush(stdout);
}

static void sniff_tune(uint8_t ch)
{
    if (radio_link.set_channel && radio_link.set_channel(&radio_link, ch) == 0)
        sniff_channel = ch;
}

#if SNIFF_COMMANDS
/* A command line from the host: "ch <11-26>" stays on that channel (and stops hopping),
   "hop <ms>" hops through 11-26 (0 stops), "status" says where it is. Nothing is kept:
   a restart is back on ESL_CHANNEL and ESL_SNIFFER_HOP_MS. */
static void sniff_command(const char *cmd, int64_t *next_hop_us)
{
    unsigned v;
    if (sscanf(cmd, "ch %u", &v) == 1 && v >= 11 && v <= 26) {
        hop_ms = 0;
        sniff_tune((uint8_t)v);
    } else if (sscanf(cmd, "hop %u", &v) == 1 && (v == 0 || (v >= 20 && v <= 10000))) {
        hop_ms = v;
        *next_hop_us = 0;
    } else if (strcmp(cmd, "status") != 0) {
        printf("@SNF-ERR unknown command: %s (ch <11-26>, hop <0 or 20-10000 ms>, status)\n", cmd);
    }
    sniff_status();
}

/* Reads what the host typed, without waiting, and runs each whole line. */
static void sniff_commands(int64_t *next_hop_us)
{
    static char cmd[32];
    static size_t n;
    uint8_t buf[32];
    int got = usb_serial_jtag_read_bytes(buf, sizeof(buf), 0);
    for (int i = 0; i < got; i++) {
        char c = (char)buf[i];
        if (c == '\r' || c == '\n') {
            cmd[n] = '\0';
            if (n)
                sniff_command(cmd, next_hop_us);
            n = 0;
        } else if (n < sizeof(cmd) - 1) {
            cmd[n++] = c;
        }
    }
}
#endif

/* Move the sniffer to the next channel unless a frame was heard lately. */
static void hop(int64_t *next_hop_us, int64_t last_frame_us)
{
    int64_t now = esp_timer_get_time();
    if (!hop_ms || !radio_link.set_channel || now < *next_hop_us || now - last_frame_us < HOP_HOLD_US)
        return;
    *next_hop_us = now + hop_ms * 1000LL;
    sniff_tune(sniff_channel >= 26 || sniff_channel < 11 ? 11 : sniff_channel + 1);
    if (sniff_channel == 11)
        ESP_LOGI(TAG, "hopping: channels 11-26, %u ms each", hop_ms);
}
#endif

static void __attribute__((unused)) handle_frame(const esl_link_rx_t *f)
{
    static uint8_t tx[ESL_PSDU_MAX];
    esl_radio_event_t ev;
    uint64_t now_ms = (uint64_t)esp_timer_get_time() / 1000;

    if (!esl_link_handle(&radio_link, store, layouts, pan, f, now_ms, tx, &ev, clock_us, NULL))
        return;
    int64_t us = radio_link.stats.last_us;
    if (us > ESL_LINK_SLOW_US)
        ESP_LOGW(TAG, "slow reply: %lld us", (long long)us);

    char m[17];
    esl_mac_format(ev.mac, m);
    if (ev.msg_type == ESL_MSG_FW_REQ) {
        /* hundreds per download: debug only; the stats line counts them as blocks */
        ESP_LOGD(TAG, "%s fw block -> %s in %lld us", m, ev.reply_type == ESL_MSG_FW ? "FW" : "IDLE", (long long)us);
        return;
    }
    if (ev.msg_type == ESL_MSG_IMG_REQ) {
        /* dozens per image: debug only, as fw blocks */
        ESP_LOGD(TAG, "%s img block -> %s in %lld us", m, ev.reply_type == ESL_MSG_IMG ? "IMG" : "IDLE", (long long)us);
        return;
    }
    const char *reply = ev.reply_type == ESL_MSG_CONFIG   ? "CONFIG"
                        : ev.reply_type == ESL_MSG_FW     ? "FW"
                        : ev.reply_type == ESL_MSG_IMG    ? "IMG"
                                                          : "IDLE";
    ESP_LOGI(TAG, "%s poll id=%08lX fw=%u bat=%u model=%u rssi=%d -> %s in %lld us", m,
             (unsigned long)ev.poll.content_id, ev.poll.fw_ver, ev.poll.battery_mv, ev.poll.model, f->rssi, reply,
             (long long)us);
}

#if REMOTE
/* The visits to channel 11. A timer every ESL_REMOTE_PERIOD_MS puts a frame of length 0
   (ref VISIT_START) on the queue, and the radio task moves to channel 11; a second timer
   ends the visit after ESL_REMOTE_VISIT_MS (VISIT_END, at the front of the queue). Only the radio task
   changes the channel, so a reply being sent is never cut off. A frame heard on channel 11
   during a visit goes only to the remote: a tag scanning there is not answered from the
   wrong channel. */
enum { VISIT_START = 1, VISIT_END = 2 };
static esp_timer_handle_t visit_timer, visit_end_timer;
static uint8_t home_channel;  /* the AP's channel */
static int64_t visit_until;   /* 0 = not visiting */
static uint32_t visits;
static uint32_t visit_skipped; /* frames from the AP's channel not answered: they came during a visit */

static void visit_mark(void *arg)
{
    esl_link_rx_t m = {0};
    m.ref = (uint32_t)(uintptr_t)arg;
    if (m.ref == VISIT_START) {
        /* behind the frames waiting, so a POLL heard just before is answered first */
        if (remote_listening() && home_channel != ESL_REMOTE_CHANNEL)
            xQueueSendToBack(rx_queue, &m, 0);
    } else if (xQueueSendToFront(rx_queue, &m, 0) != pdTRUE) {
        ESP_LOGD(TAG, "visit end: queue full"); /* radio_task ends it by the clock */
    }
}

static void visit_start(void)
{
    if (visit_until || !remote_listening() || home_channel == ESL_REMOTE_CHANNEL)
        return;
    radio_link.set_channel(&radio_link, ESL_REMOTE_CHANNEL);
    visit_until = esp_timer_get_time() + ESL_REMOTE_VISIT_MS * 1000LL;
    visits++;
    esp_timer_start_once(visit_end_timer, ESL_REMOTE_VISIT_MS * 1000ULL);
}

static void visit_end(void)
{
    if (!visit_until)
        return;
    visit_until = 0;
    radio_link.set_channel(&radio_link, home_channel);
}

static void remote_start_visits(uint8_t channel)
{
    home_channel = channel;
    const esp_timer_create_args_t a = {.callback = visit_mark, .arg = (void *)VISIT_START, .name = "visit"};
    const esp_timer_create_args_t b = {.callback = visit_mark, .arg = (void *)VISIT_END, .name = "visit_end"};
    if (esp_timer_create(&a, &visit_timer) != ESP_OK || esp_timer_create(&b, &visit_end_timer) != ESP_OK ||
        esp_timer_start_periodic(visit_timer, ESL_REMOTE_PERIOD_MS * 1000ULL) != ESP_OK)
        ESP_LOGE(TAG, "remote: no visits to channel %u", ESL_REMOTE_CHANNEL);
}
#endif

#if !SNIFFER
/* A planned channel move (esl_store_move_channel), once the tags have it or the time is up */
static void move_channel(void)
{
    uint8_t ch = esl_store_channel_step(store, (uint64_t)esp_timer_get_time() / 1000);
    if (!ch)
        return;
#if REMOTE
    visit_end();
    home_channel = ch;
#endif
    int err = radio_link.set_channel ? radio_link.set_channel(&radio_link, ch) : ESP_ERR_NOT_SUPPORTED;
    ESP_LOGI(TAG, "moved to channel %u: %s", ch, esp_err_to_name(err));
}
#endif

static void radio_task(void *arg)
{
    (void)arg;
    esl_link_rx_t f;
    int64_t last_stats = esp_timer_get_time();
#if SNIFFER
    int64_t next_hop_us = 0, last_frame_us = -HOP_HOLD_US;
    /* often enough for the hops (20 ms at the least) and for a command from the host */
    const TickType_t wait = pdMS_TO_TICKS(20);
    sniff_status();
#else
    const TickType_t wait = pdMS_TO_TICKS(1000);
#endif
    ESL_CHECK(esp_task_wdt_add(NULL));
    for (;;) {
        esp_task_wdt_reset();
#if REMOTE
        TickType_t w = visit_until ? 1 : wait; /* during a visit: its end, should its mark be lost */
#else
        TickType_t w = wait;
#endif
        if (xQueueReceive(rx_queue, &f, w ? w : 1) == pdTRUE) {
#if SNIFFER
            radio_link.stats.rx++;
            sniff_line(&f);
            last_frame_us = f.rx_us;
#elif REMOTE
            if (f.len == 0) { /* a mark from the visit timers */
                if (f.ref == VISIT_START)
                    visit_start();
                else
                    visit_end();
            } else {
                /* A tag is answered only while the radio is on its channel: not a frame heard on
                   11 during a visit, nor one from the AP's channel that waited in the queue behind
                   VISIT_START (a busy radio task): its reply would go out on 11, where the tag is
                   not, and the time sending it would be lost to the remote. The tag retries. */
                if (!visit_until && (f.channel != ESL_REMOTE_CHANNEL || home_channel == ESL_REMOTE_CHANNEL))
                    handle_frame(&f);
                else if (f.channel != ESL_REMOTE_CHANNEL)
                    visit_skipped++;
                remote_frame(&f);
            }
#else
            handle_frame(&f);
#endif
        }
#if REMOTE
        if (visit_until && esp_timer_get_time() >= visit_until + 2000) /* its mark was lost */
            visit_end();
#endif
#if SNIFFER
#if SNIFF_COMMANDS
        sniff_commands(&next_hop_us);
#endif
        hop(&next_hop_us, last_frame_us);
#endif
#if !SNIFFER
        move_channel();
#endif
        if (esp_timer_get_time() - last_stats > 60 * 1000000LL) {
            const esl_link_stats_t *s = &radio_link.stats;
            last_stats = esp_timer_get_time();
            ESP_LOGI(TAG, "heap: free %u, largest block %u, lowest free %u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT));
#if REMOTE
            if (visits)
                ESP_LOGI(TAG, "remote: %lu visits to channel %u, %lu frames left for the tag's retry", (unsigned long)visits,
                         ESL_REMOTE_CHANNEL, (unsigned long)visit_skipped);
            visits = 0;
            visit_skipped = 0;
#endif
            ESP_LOGI(TAG, "stats: rx %lu polls %lu blocks %lu replies %lu tx_failed %lu tx_resent %lu dropped %lu worst %lld us",
                     (unsigned long)s->rx, (unsigned long)s->polls, (unsigned long)s->blocks,
                     (unsigned long)s->replies, (unsigned long)s->tx_failed, (unsigned long)s->tx_resent,
                     (unsigned long)s->dropped, (long long)s->worst_us);
        }
    }
}

esp_err_t radio_start(esl_store_t *st, esl_layouts_t *ls, uint8_t channel, uint16_t pan_id)
{
    store = st;
    layouts = ls;
    pan = pan_id;
#if SNIFFER
    sniff_channel = channel;
#if SNIFF_COMMANDS
    /* The sniffer reads its commands from the USB port: the driver, with stdout through it
       too, as serial_api.c has it in AP mode (the sniffer has no serial API) */
    usb_serial_jtag_driver_config_t ucfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    ucfg.tx_buffer_size = 4096;
    if (usb_serial_jtag_driver_install(&ucfg) == ESP_OK) {
        usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
        usb_serial_jtag_vfs_use_driver();
    }
#endif
#endif
    rx_queue = xQueueCreate(8, sizeof(esl_link_rx_t));
    if (!rx_queue)
        return ESP_ERR_NO_MEM;
    /* esl_radio_handle copies one model's layout names (about 0.5 KB) onto the stack, and
       drawing an image holds a tag's content and its NVS blob (about 0.6 KB since #182) */
    if (xTaskCreate(radio_task, "radio", 6656, NULL, configMAX_PRIORITIES - 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    esp_err_t err = radio_link.start(&radio_link, channel, pan_id, ESL_AP_SHORT_ADDR, SNIFFER);
#if REMOTE
    if (err == ESP_OK)
        remote_start_visits(channel);
#endif
    int8_t dbm = radio_tx_power()->dbm;
    if (err == ESP_OK && dbm != CONFIG_ESL_TX_POWER_DBM) /* the driver starts with menuconfig's */
        radio_set_tx_power(dbm);
    ESP_LOGI(TAG, "%s %s on channel %u, PAN 0x%04X, %d dBm: %s", radio_link.name, SNIFFER ? "sniffer" : "AP",
             channel, pan_id, dbm, esp_err_to_name(err));
    return err;
}

void radio_set_tx_power(int8_t dbm)
{
    if (radio_link.set_tx_power)
        radio_link.set_tx_power(&radio_link, dbm);
}
