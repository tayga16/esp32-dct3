#include "settings_store.h"

#include <string.h>
#include "esp_rom_crc.h"

#define SLOT_SIZE 0x31000u
#define DATA_OFFSET 0x1000u
#define MAGIC 0x334E5652u
#define VERSION 1u

typedef struct {
    uint32_t magic, version, identity, size, sequence, data_crc, header_crc;
} Header;

static esp_err_t slot_crc(const SettingsStore* store, int slot, uint32_t* crc) {
    uint8_t chunk[4096];
    *crc = 0;
    for (size_t offset = 0; offset < store->size; offset += sizeof chunk) {
        size_t count = store->size - offset;
        if (count > sizeof chunk) count = sizeof chunk;
        esp_err_t err = esp_partition_read(store->partition,
            (size_t)slot * SLOT_SIZE + DATA_OFFSET + offset, chunk, count);
        if (err != ESP_OK) return err;
        *crc = esp_rom_crc32_le(*crc, chunk, count);
    }
    return ESP_OK;
}

esp_err_t settings_store_load(SettingsStore* store, uint32_t identity,
                              uint8_t* data, size_t size) {
    if (!store || !data || !size || size > SLOT_SIZE - DATA_OFFSET)
        return ESP_ERR_INVALID_ARG;
    memset(store, 0, sizeof *store);
    store->active_slot = -1;
    store->size = size;
    store->identity = identity;
    store->partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                               ESP_PARTITION_SUBTYPE_ANY, "settings");
    if (!store->partition || store->partition->size < 2 * SLOT_SIZE) {
        store->partition = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    Header headers[2];
    int valid[2] = {0};
    for (int slot = 0; slot < 2; ++slot) {
        Header* h = &headers[slot];
        esp_err_t err = esp_partition_read(store->partition,
                                           (size_t)slot * SLOT_SIZE, h, sizeof *h);
        if (err != ESP_OK) { store->partition = NULL; return err; }
        if (h->magic != MAGIC || h->version != VERSION || h->identity != identity ||
            h->size != size || h->header_crc != esp_rom_crc32_le(0, (uint8_t*)h,
                                                               offsetof(Header, header_crc)))
            continue;
        uint32_t crc;
        err = slot_crc(store, slot, &crc);
        if (err != ESP_OK) { store->partition = NULL; return err; }
        valid[slot] = crc == h->data_crc;
    }
    int slot = valid[1] && (!valid[0] || (int32_t)(headers[1].sequence - headers[0].sequence) > 0)
               ? 1 : (valid[0] ? 0 : -1);
    if (slot < 0) return ESP_ERR_NOT_FOUND;
    esp_err_t err = esp_partition_read(store->partition,
                                       (size_t)slot * SLOT_SIZE + DATA_OFFSET, data, size);
    if (err != ESP_OK) { store->partition = NULL; return err; }
    store->active_slot = slot;
    store->sequence = headers[slot].sequence;
    return ESP_OK;
}

esp_err_t settings_store_save(SettingsStore* store, const uint8_t* data) {
    if (!store || !store->partition || !data) return ESP_ERR_INVALID_STATE;
    const int slot = store->active_slot == 0 ? 1 : 0;
    const size_t offset = (size_t)slot * SLOT_SIZE;
    Header h = {.magic = MAGIC, .version = VERSION, .identity = store->identity,
                .size = (uint32_t)store->size, .sequence = store->sequence + 1};
    h.data_crc = esp_rom_crc32_le(0, data, store->size);
    h.header_crc = esp_rom_crc32_le(0, (uint8_t*)&h, offsetof(Header, header_crc));
    // Invalidate only the destination header; the active slot stays intact.
    esp_err_t err = esp_partition_erase_range(store->partition, offset, DATA_OFFSET);
    if (err != ESP_OK) return err;
    uint8_t sector[4096];
    for (size_t pos = 0; pos < store->size; pos += sizeof sector) {
        size_t count = store->size - pos;
        if (count > sizeof sector) count = sizeof sector;
        const size_t target = offset + DATA_OFFSET + pos;
        err = esp_partition_read(store->partition, target, sector, count);
        if (err != ESP_OK) return err;
        if (memcmp(sector, data + pos, count) == 0) continue;
        err = esp_partition_erase_range(store->partition, target, sizeof sector);
        if (err != ESP_OK) return err;
        err = esp_partition_write(store->partition, target, data + pos, count);
        if (err != ESP_OK) return err;
    }
    uint32_t crc;
    err = slot_crc(store, slot, &crc);
    if (err != ESP_OK) return err;
    if (crc != h.data_crc) return ESP_ERR_INVALID_CRC;
    err = esp_partition_write(store->partition, offset, &h, sizeof h);
    if (err != ESP_OK) return err;
    Header readback;
    err = esp_partition_read(store->partition, offset, &readback, sizeof readback);
    if (err != ESP_OK) return err;
    if (memcmp(&h, &readback, sizeof h) != 0) return ESP_ERR_INVALID_CRC;
    store->active_slot = slot;
    store->sequence = h.sequence;
    return ESP_OK;
}
