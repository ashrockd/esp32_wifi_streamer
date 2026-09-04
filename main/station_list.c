#include "station_list.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"

#include "app_config.h"

static const char *TAG = "STATIONS";

#define STATION_NVS_NAMESPACE "radio"
#define STATION_NVS_KEY       "station_idx"

/* Index of RADIO_TUNEIN_STATION_ID within RADIO_STATIONS, resolved once by
 * name rather than hardcoded as a number - keeps this correct even if the
 * table above is ever reordered. Logs an error and falls back to 0 if the
 * compile-time default id is somehow not in the table (should never happen;
 * a mismatch here is a build-time mistake, not a runtime condition). */
static uint8_t default_index(void)
{
    for (uint8_t i = 0; i < RADIO_STATION_COUNT; i++) {
        if (strcmp(RADIO_STATIONS[i].id, RADIO_TUNEIN_STATION_ID) == 0) {
            return i;
        }
    }
    ESP_LOGE(TAG, "RADIO_TUNEIN_STATION_ID '%s' is not in RADIO_STATIONS; defaulting to index 0",
             RADIO_TUNEIN_STATION_ID);
    return 0;
}

uint8_t station_list_load_index(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STATION_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* ESP_ERR_NVS_NOT_FOUND: namespace never written yet (first boot
         * after this feature was added) - not a real error. */
        ESP_LOGI(TAG, "No saved station selection yet (%s); starting at default '%s'",
                 esp_err_to_name(err), RADIO_TUNEIN_STATION_ID);
        return default_index();
    }

    uint8_t idx = 0;
    err = nvs_get_u8(handle, STATION_NVS_KEY, &idx);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "No saved station index yet (%s); starting at default '%s'",
                 esp_err_to_name(err), RADIO_TUNEIN_STATION_ID);
        return default_index();
    }

    if (idx >= RADIO_STATION_COUNT) {
        ESP_LOGW(TAG, "Saved station index %u is out of range (have %u stations); starting at default '%s'",
                 (unsigned)idx, (unsigned)RADIO_STATION_COUNT, RADIO_TUNEIN_STATION_ID);
        return default_index();
    }

    ESP_LOGI(TAG, "Resuming saved station: %s (%s)", RADIO_STATIONS[idx].name, RADIO_STATIONS[idx].id);
    return idx;
}

static uint8_t save_index(uint8_t idx)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(STATION_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not open NVS to persist station index: %s (selection will not survive reboot)",
                 esp_err_to_name(err));
        return idx;
    }

    err = nvs_set_u8(handle, STATION_NVS_KEY, idx);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Could not persist station index %u: %s (selection will not survive reboot)",
                 (unsigned)idx, esp_err_to_name(err));
    }
    nvs_close(handle);
    return idx;
}

uint8_t station_list_next(uint8_t current)
{
    uint8_t idx = (uint8_t)((current + 1) % RADIO_STATION_COUNT);
    ESP_LOGI(TAG, "Station -> next: %s (%s)", RADIO_STATIONS[idx].name, RADIO_STATIONS[idx].id);
    return save_index(idx);
}

uint8_t station_list_prev(uint8_t current)
{
    uint8_t idx = (uint8_t)((current + RADIO_STATION_COUNT - 1) % RADIO_STATION_COUNT);
    ESP_LOGI(TAG, "Station -> previous: %s (%s)", RADIO_STATIONS[idx].name, RADIO_STATIONS[idx].id);
    return save_index(idx);
}

/* See station_list.h's doc comment - a live status mirror, not the
 * persisted selection. */
static volatile uint8_t s_now_playing_idx;

void station_list_set_now_playing(uint8_t idx)
{
    s_now_playing_idx = idx;
}

uint8_t station_list_get_now_playing(void)
{
    return s_now_playing_idx;
}
