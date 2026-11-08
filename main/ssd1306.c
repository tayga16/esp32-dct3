// Minimal SSD1306 (128x64) I2C driver. No external graphics library: the
// PCD8544 framebuffer is page-major just like the SSD1306, so a 3310 frame is a
// straight 84-byte-per-page blit into the centred window.
#include "ssd1306.h"

#include <stdbool.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "ssd1306";

static i2c_master_bus_handle_t  s_bus;
static i2c_master_dev_handle_t  s_dev;
static uint8_t                  s_shadow[8][128];
static bool                     s_shadow_valid;

static esp_err_t write_cmds(const uint8_t* cmds, size_t n) {
    uint8_t buf[64];
    if (n + 1 > sizeof buf) return ESP_ERR_INVALID_SIZE;
    buf[0] = 0x00;                       // Co=0, D/C#=0: command stream
    memcpy(buf + 1, cmds, n);
    return i2c_master_transmit(s_dev, buf, n + 1, 100);
}

static esp_err_t set_window(int page, int col0, int col1) {
    uint8_t cmds[6] = {
        0x21, (uint8_t)col0, (uint8_t)col1,   // column address range
        0x22, (uint8_t)page, (uint8_t)page,   // page address range
    };
    return write_cmds(cmds, sizeof cmds);
}

static esp_err_t write_page(int page, const uint8_t* data, int n) {
    uint8_t buf[1 + 128];
    buf[0] = 0x40;                       // Co=0, D/C#=1: data stream
    memcpy(buf + 1, data, (size_t)n);
    return i2c_master_transmit(s_dev, buf, (size_t)n + 1, 100);
}

esp_err_t ssd1306_init(void) {
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = CONFIG_DCT3_I2C_PORT,
        .sda_io_num = CONFIG_DCT3_I2C_SDA,
        .scl_io_num = CONFIG_DCT3_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = CONFIG_DCT3_I2C_ADDR,
        .scl_speed_hz = CONFIG_DCT3_I2C_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(50));       // panel power-on settle

    static const uint8_t init_cmds[] = {
        0xAE,              // display off
        0xD5, 0x80,        // clock divide
        0xA8, 0x3F,        // multiplex 1/64
        0xD3, 0x00,        // display offset
        0x40,              // start line 0
        0x8D, 0x14,        // charge pump on
        0x20, 0x00,        // horizontal addressing
        0xA1,              // segment remap (left/right orientation)
        0xC8,              // COM scan dec (top/bottom orientation)
        0xDA, 0x12,        // COM pins
        0x81, 0xCF,        // contrast
        0xD9, 0xF1,        // pre-charge
        0xDB, 0x40,        // VCOMH
        0xA4,              // resume from RAM
        0xA6,              // normal (not inverse)
        0xAF,              // display on
    };
    err = write_cmds(init_cmds, sizeof init_cmds);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init sequence: %s", esp_err_to_name(err));
        // Do not leave a half-working device handle: main will run headless and
        // must not retry a 100 ms I2C timeout on every frame.
        i2c_master_bus_rm_device(s_dev);
        s_dev = NULL;
        return err;
    }

    ssd1306_clear();
    ESP_LOGI(TAG, "SSD1306 ready on I2C%d (SDA=%d SCL=%d addr=0x%02X)",
             CONFIG_DCT3_I2C_PORT, CONFIG_DCT3_I2C_SDA, CONFIG_DCT3_I2C_SCL,
             CONFIG_DCT3_I2C_ADDR);
    return ESP_OK;
}

void ssd1306_clear(void) {
    if (!s_dev) return;
    static const uint8_t zeros[128] = { 0 };
    s_shadow_valid = false;
    for (int page = 0; page < 8; ++page) {
        if (set_window(page, 0, 127) != ESP_OK) return;
        if (write_page(page, zeros, 128) != ESP_OK) return;
    }
    memset(s_shadow, 0, sizeof s_shadow);
    s_shadow_valid = true;
}

void ssd1306_set_contrast(uint8_t contrast) {
    if (!s_dev) return;
    uint8_t cmds[2] = { 0x81, contrast };
    write_cmds(cmds, sizeof cmds);
}

void ssd1306_show_lcd(const uint8_t* fb, int width, int height, int banks, int mode) {
    if (!s_dev || !fb || width <= 0 || height <= 0 || banks <= 0) return;

    int x0 = (128 - width) / 2;
    int ypage = (64 - height) / 16;      // 3310: (64-48)/16 = 1 -> one blank page top/bottom
    if (x0 < 0) x0 = 0;
    if (ypage < 0) ypage = 0;

    // The shadow always describes successfully transmitted panel RAM, even
    // across mode changes. An invalid shadow requires every row to be sent.
    const bool repaint = !s_shadow_valid;

    for (int p = 0; p < banks; ++p) {
        int page = ypage + p;
        if (page >= 8) break;

        int n = width;
        if (x0 + n > 128) n = 128 - x0;

        uint8_t row[128];
        if (mode == 0 || mode == 1) {
            memset(row, mode == 1 ? 0xFF : 0, (size_t)n);
        } else if (mode == 3) {
            for (int x = 0; x < n; ++x)
                row[x] = (uint8_t)~fb[p * width + x];
        } else {
            memcpy(row, fb + p * width, (size_t)n);
        }

        int first = 0, last = n - 1;
        if (!repaint) {
            while (first < n && s_shadow[page][x0 + first] == row[first]) first++;
            if (first == n) continue;
            while (last > first && s_shadow[page][x0 + last] == row[last]) last--;
        }
        const int count = last - first + 1;
        if (set_window(page, x0 + first, x0 + last) != ESP_OK ||
            write_page(page, row + first, count) != ESP_OK) {
            s_shadow_valid = false;
            return;
        }
        memcpy(&s_shadow[page][x0 + first], row + first, (size_t)count);
    }
    s_shadow_valid = true;
}
