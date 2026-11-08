#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_partition.h"

typedef struct {
    const esp_partition_t* partition;
    uint32_t identity;
    uint32_t sequence;
    int active_slot;
    size_t size;
} SettingsStore;

// Return ESP_ERR_NOT_FOUND for a fresh store; existing data is left intact.
esp_err_t settings_store_load(SettingsStore* store, uint32_t identity,
                              uint8_t* data, size_t size);
// Write the other slot, verify payload, then commit its header last.
esp_err_t settings_store_save(SettingsStore* store, const uint8_t* data);
