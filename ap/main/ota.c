#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"

/* OTA for the AP itself (krav A10): POST /api/ota with the app image
 * (build/esl_ap.bin, the CI's openesl-ap-ota.bin) writes the app slot that
 * is not running, in one request or in parts (?offset=N&total=T, which the
 * USB port needs: its driver drops bytes when its buffer is full, so the
 * host waits for a reply per part). The last byte makes the new slot the
 * boot one and restarts the AP a second after the reply. The bootloader
 * boots the old slot again if the new image does not confirm itself
 * (see below), so a bad image costs one restart. GET /api/ota tells which slot runs and what the other holds.
 *
 * A new image counts as good once the AP can be reached: WiFi has an address,
 * or an API request was answered over WiFi or USB. An image that comes up but
 * never gets there (it broke WiFi, say) is given up after CONFIRM_S and the AP
 * restarts into the old one, so an update over WiFi cannot leave the AP
 * reachable only over USB. Without a WiFi network to join it is kept at once.
 *
 * On an AP with a radio coprocessor on UART (boards/s3-n16r8-c6) the same
 * request takes the radio's image (ap/radio/, project esl_radio) too: it goes
 * through the link to the radio as it arrives (radio_ota_*() in radio_uart.c),
 * and the radio restarts into it; the AP itself keeps running. GET /api/ota says
 * which version the radio runs ("radio").
 *
 * One upload at a time: a part at offset 0 starts over, any other offset
 * must be the next byte. The serial and HTTP tasks share the state through
 * the mutex. */

static const char *TAG = "ota";

#define CONFIRM_S 180

static SemaphoreHandle_t mutex;
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static size_t received, total;
static bool open_;
static bool to_radio; /* the open upload is the radio's image */
static volatile bool reachable, watching, kept;
static esp_timer_handle_t confirm_timer;

static void lock(void)
{
    if (!mutex)
        mutex = xSemaphoreCreateMutex();
    xSemaphoreTake(mutex, portMAX_DELAY);
}

static void unlock(void)
{
    xSemaphoreGive(mutex);
}

static void restart_cb(void *arg)
{
    (void)arg;
    store_nvs_flush(); /* the tags' last poll survives the update */
    ESP_LOGI(TAG, "restarting into the new firmware");
    esp_restart();
}

static int fail(char *err, size_t errlen, int status, const char *msg)
{
    snprintf(err, errlen, "%s", msg);
    if (open_ && !to_radio)
        esp_ota_abort(handle);
    open_ = false; /* the radio drops its own at the next OTA_BEGIN */
    received = total = 0;
    return status;
}

/* An app image's esp_app_desc_t comes after the image header (24 bytes) and the
 * first segment's header (8): its magic word, then the version at 16 and the project
 * name at 48. Another app for the same chip (ap/radio/'s esl_radio, say) would boot
 * and leave the AP without WiFi, so only an image of this project goes in. One too
 * short to say is left to esp_ota_end(). */
#define RADIO_PROJECT "esl_radio"

static const char *image_project(const void *data, size_t len)
{
    const uint8_t *d = data;
    uint32_t magic;
    if (len < 32 + 48 + 32)
        return NULL;
    memcpy(&magic, d + 32, 4);
    return magic == ESP_APP_DESC_MAGIC_WORD ? (const char *)d + 32 + 48 : NULL;
}

static bool is_ap_image(const void *data, size_t len)
{
    const char *project = image_project(data, len);
    return !project || strncmp(project, esp_app_get_description()->project_name, 32) == 0;
}

static bool is_radio_image(const void *data, size_t len)
{
    const char *project = image_project(data, len);
    return project && strncmp(project, RADIO_PROJECT, 32) == 0;
}

#if CONFIG_ESL_RADIO_DRIVER_UART
/* The rest of ota_part() for the radio's image: what radio_ota_*() say */
static char radio_version[33];

static int radio_part(const void *data, size_t len, esl_sb_t *out, char *err, size_t errlen)
{
    int status = 0;
    if (received == 0) {
        const uint8_t *d = data;
        memcpy(radio_version, d + 32 + 16, 32);
        radio_version[32] = '\0';
    }
    if (len)
        status = radio_ota_write(data, len, err, errlen);
    if (status) {
        open_ = false;
        received = total = 0;
        return status;
    }
    received += len;
    if (received < total) {
        esl_sb_printf(out, "{\"received\":%u,\"total\":%u}", (unsigned)received, (unsigned)total);
        return 200;
    }
    open_ = false;
    status = radio_ota_end(err, errlen);
    if (status) {
        received = total = 0;
        return status;
    }
    esl_sb_printf(out, "{\"received\":%u,\"total\":%u,\"radio\":true,\"version\":", (unsigned)received,
                  (unsigned)total);
    esl_sb_json_str(out, radio_version);
    esl_sb_puts(out, ",\"restarting\":false}");
    ESP_LOGI(TAG, "radio firmware %s sent: the radio restarts into it", radio_version);
    received = total = 0;
    return 200;
}
#endif

int ota_part(size_t offset, size_t size, const void *data, size_t len, esl_sb_t *out)
{
    char err[96];
    int status = 0;
    lock();
    if (offset == 0) {
        if (open_ && !to_radio)
            esp_ota_abort(handle);
        open_ = false;
        received = total = 0;
        to_radio = false;
        target = esp_ota_get_next_update_partition(NULL);
        if (is_radio_image(data, len)) {
#if CONFIG_ESL_RADIO_DRIVER_UART
            char why[96];
            int st = size ? radio_ota_begin((uint32_t)size, why, sizeof(why)) : 0;
            if (!size)
                status = fail(err, sizeof(err), 413, "the image does not fit the app slot");
            else if (st)
                status = fail(err, sizeof(err), st, why);
            else {
                open_ = to_radio = true;
                total = size;
                ESP_LOGI(TAG, "sending %u bytes to the radio coprocessor", (unsigned)size);
            }
#else
            status = fail(err, sizeof(err), 422, "the radio coprocessor's firmware: this AP has no radio on UART to send it to");
#endif
        } else if (!target)
            status = fail(err, sizeof(err), 501, "no second app slot (the partition table has no ota_1)");
        else if (size == 0 || size > target->size)
            status = fail(err, sizeof(err), 413, "the image does not fit the app slot");
        else if (!is_ap_image(data, len))
            status = fail(err, sizeof(err), 422, "not AP firmware: another app for this chip (the radio coprocessor's?)");
        else if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle) != ESP_OK)
            status = fail(err, sizeof(err), 500, "could not start writing the app slot");
        else {
            open_ = true;
            total = size;
            ESP_LOGI(TAG, "writing %u bytes to %s", (unsigned)size, target->label);
        }
    } else if (!open_) {
        status = fail(err, sizeof(err), 409, "no upload in progress: send offset 0 first");
    } else if (offset != received || size != total) {
        snprintf(err, sizeof(err), "expected offset %u of %u", (unsigned)received, (unsigned)total);
        status = 409; /* the upload stays open: the host can resend that part */
    }
    if (!status && received + len > total)
        status = fail(err, sizeof(err), 413, "more bytes than the total");
#if CONFIG_ESL_RADIO_DRIVER_UART
    if (!status && to_radio) {
        status = radio_part(data, len, out, err, sizeof(err));
        unlock();
        if (status != 200) {
            esl_sb_puts(out, "{\"error\":");
            esl_sb_json_str(out, err);
            esl_sb_putc(out, '}');
        }
        return status;
    }
#endif
    if (!status && len && esp_ota_write(handle, data, len) != ESP_OK)
        status = fail(err, sizeof(err), 500, "writing the app slot failed (bad image header?)");
    if (!status) {
        received += len;
        if (received < total) {
            esl_sb_printf(out, "{\"received\":%u,\"total\":%u}", (unsigned)received, (unsigned)total);
            status = 200;
        } else {
            esp_err_t e = esp_ota_end(handle);
            open_ = false;
            if (e != ESP_OK)
                status = fail(err, sizeof(err), 422,
                              e == ESP_ERR_OTA_VALIDATE_FAILED ? "the image is not valid for this chip" : "the image is incomplete");
            else if (esp_ota_set_boot_partition(target) != ESP_OK)
                status = fail(err, sizeof(err), 500, "could not select the new slot");
            else {
                esp_app_desc_t d;
                const char *ver = esp_ota_get_partition_description(target, &d) == ESP_OK ? d.version : "";
                esl_sb_printf(out, "{\"received\":%u,\"total\":%u,\"slot\":\"%s\",\"version\":", (unsigned)received,
                              (unsigned)total, target->label);
                esl_sb_json_str(out, ver);
                esl_sb_puts(out, ",\"restarting\":true}");
                ESP_LOGI(TAG, "%s written: version %s, restarting in 1 s", target->label, ver);
                const esp_timer_create_args_t a = {.callback = restart_cb, .name = "ota_restart"};
                esp_timer_handle_t t;
                if (esp_timer_create(&a, &t) == ESP_OK)
                    esp_timer_start_once(t, 1000 * 1000);
                received = total = 0;
                status = 200;
            }
        }
    }
    unlock();
    if (status != 200) {
        esl_sb_puts(out, "{\"error\":");
        esl_sb_json_str(out, err);
        esl_sb_putc(out, '}');
    }
    return status;
}

int ota_info(esl_sb_t *out)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_app_desc_t d;
    esl_sb_printf(out, "{\"slot\":\"%s\",\"version\":", run ? run->label : "?");
    esl_sb_json_str(out, esp_app_get_description()->version);
    esl_sb_printf(out, ",\"slot_size\":%u,\"other_slot\":", next ? (unsigned)next->size : 0u);
    if (next) {
        esl_sb_printf(out, "\"%s\",\"other_version\":", next->label);
        if (esp_ota_get_partition_description(next, &d) == ESP_OK)
            esl_sb_json_str(out, d.version);
        else
            esl_sb_puts(out, "null");
    } else {
        esl_sb_puts(out, "null,\"other_version\":null");
    }
#if CONFIG_ESL_DISPLAY
    const bool display = true;
#else
    const bool display = false;
#endif
    esl_sb_printf(out, ",\"confirmed\":%s,\"display\":%s,\"chip\":\"%s\"", kept ? "true" : "false",
                  display ? "true" : "false", CONFIG_IDF_TARGET);
#if CONFIG_ESL_RADIO_DRIVER_UART
    esl_sb_puts(out, ",\"radio\":");
    radio_info(out);
#endif
    lock();
    esl_sb_printf(out, ",\"uploading\":%s}", open_ ? "true" : "false");
    unlock();
    return 200;
}

/* The query string of POST /api/ota: ?offset=N&total=T, both 0 when absent. */
bool ota_parse_query(const char *uri, size_t *offset, size_t *total)
{
    const char *q = strchr(uri, '?');
    *offset = *total = 0;
    bool got_offset = false, got_total = false;
    while (q) {
        q++;
        if (strncmp(q, "offset=", 7) == 0) {
            *offset = (size_t)strtoul(q + 7, NULL, 10);
            got_offset = true;
        } else if (strncmp(q, "total=", 6) == 0) {
            *total = (size_t)strtoul(q + 6, NULL, 10);
            got_total = true;
        }
        q = strchr(q, '&');
    }
    return got_offset == got_total; /* both or neither */
}

static void keep(void)
{
    if (kept)
        return;
    kept = true;
    if (confirm_timer)
        esp_timer_stop(confirm_timer);
    esp_ota_mark_app_valid_cancel_rollback();
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "new firmware confirmed on %s", run ? run->label : "?");
}

/* WiFi or the API answered. Before ota_watch_running() (WiFi can come up
 * before the radio has started) this is only noted. */
void ota_confirm_running(void)
{
    reachable = true;
    if (watching)
        keep();
}

static void give_up_cb(void *arg)
{
    (void)arg;
    if (kept)
        return;
    ESP_LOGE(TAG, "the new firmware was not reachable for %d s: back to the old one", CONFIRM_S);
    store_nvs_flush();
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void ota_watch_running(bool wait)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) {
        kept = true; /* nothing to confirm: flashed over USB, or kept before */
        return;
    }
    if (!wait || reachable) {
        keep();
        return;
    }
    ESP_LOGI(TAG, "new firmware on %s: kept once WiFi or the API answers (within %d s)", run->label, CONFIRM_S);
    const esp_timer_create_args_t a = {.callback = give_up_cb, .name = "ota_confirm"};
    if (esp_timer_create(&a, &confirm_timer) != ESP_OK) {
        keep();
        return;
    }
    watching = true;
    esp_timer_start_once(confirm_timer, (uint64_t)CONFIRM_S * 1000 * 1000);
    if (reachable) /* it answered while the timer was being set up */
        keep();
}
