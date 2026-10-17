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

/* OTA for the AP itself (krav A10): POST /api/ota with the app image
 * (build/esl_ap.bin, the CI's openesl-ap-ota.bin) writes the app slot that
 * is not running, in one request or in parts (?offset=N&total=T, which the
 * USB port needs: its driver drops bytes when its buffer is full, so the
 * host waits for a reply per part). The last byte makes the new slot the
 * boot one and restarts the AP a second after the reply. The bootloader
 * boots the old slot again if the new image does not confirm itself
 * (ota_confirm_running, called once the AP is up), so a bad image costs one
 * restart. GET /api/ota tells which slot runs and what the other holds.
 *
 * One upload at a time: a part at offset 0 starts over, any other offset
 * must be the next byte. The serial and HTTP tasks share the state through
 * the mutex. */

static const char *TAG = "ota";

static SemaphoreHandle_t mutex;
static esp_ota_handle_t handle;
static const esp_partition_t *target;
static size_t received, total;
static bool open_;

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
    if (open_)
        esp_ota_abort(handle);
    open_ = false;
    received = total = 0;
    return status;
}

int ota_part(size_t offset, size_t size, const void *data, size_t len, esl_sb_t *out)
{
    char err[96];
    int status = 0;
    lock();
    if (offset == 0) {
        if (open_)
            esp_ota_abort(handle);
        open_ = false;
        received = total = 0;
        target = esp_ota_get_next_update_partition(NULL);
        if (!target)
            status = fail(err, sizeof(err), 501, "no second app slot (the partition table has no ota_1)");
        else if (size == 0 || size > target->size)
            status = fail(err, sizeof(err), 413, "the image does not fit the app slot");
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

void ota_confirm_running(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (run && esp_ota_get_state_partition(run, &st) == ESP_OK && st == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "new firmware confirmed on %s", run->label);
    }
}
