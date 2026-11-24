#pragma once
#include "esp_err.h"
#include "esl_image.h"
#include "esl_layout.h"
#include "esl_store.h"

/* The BLE central and the push task for Bluetooth tags with the maker's firmware
 * (docs/ble-tags.md, docs/gicisky-296-bwry.md). Needs CONFIG_ESL_BLE.
 *
 * st, ls and im are the AP's store, layout files and image buffers. The task scans for
 * Gicisky tags (model 0x2E), adds them (CONFIG_ESL_BLE_AUTO_ADD) or the one in
 * CONFIG_ESL_BLE_TAG_MAC, reports them to esl_push_seen, and every
 * CONFIG_ESL_BLE_ROUND_MS sends the image of a tag whose content changed through
 * esl_ble_send. */
esp_err_t ble_start(esl_store_t *st, esl_layouts_t *ls, esl_image_t *im);
