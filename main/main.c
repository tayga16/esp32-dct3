// ESP32-S3 Nokia 3310 emulator: task wiring, display bridge and real-time pacing.
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "emu_harness.h"
#include "kbd_serial.h"
#include "speaker.h"
#include "ssd1306.h"

#include "mad2/emu_host.h"
#include "models/model.h"

static const char* TAG = "main";

static EmuHarness g_emu;

// Published PCD8544 frame. The emulator task writes under the mutex; the
// display task snapshots it and blits only pages that changed.
static SemaphoreHandle_t g_fb_lock;
static uint8_t           g_fb[84 * 6];
static int               g_fb_w = 84;
static int               g_fb_h = 48;
static int               g_fb_banks = 6;
static volatile int      g_fb_mode = 2;
static volatile uint32_t g_fb_seq;

static void publish_fb(void) {
    if (!g_emu.booted) return;

    xSemaphoreTake(g_fb_lock, portMAX_DELAY);
    int w = emu_lcd_w(&g_emu.mad2);
    int h = emu_lcd_h(&g_emu.mad2);
    int banks = emu_lcd_banks(&g_emu.mad2);
    size_t bytes = (size_t)w * (size_t)banks;
    if (bytes > sizeof g_fb) bytes = sizeof g_fb;
    const int mode = emu_lcd_mode(&g_emu.mad2);
    if (g_fb_seq && w == g_fb_w && h == g_fb_h && banks == g_fb_banks &&
        mode == g_fb_mode && memcmp(g_fb, emu_lcd_fb(&g_emu.mad2), bytes) == 0) {
        xSemaphoreGive(g_fb_lock);
        return;
    }
    memcpy(g_fb, emu_lcd_fb(&g_emu.mad2), bytes);
    g_fb_w = w;
    g_fb_h = h;
    g_fb_banks = banks;
    g_fb_mode = mode;
    g_fb_seq++;
    xSemaphoreGive(g_fb_lock);
}

static void display_task(void* arg) {
    (void)arg;
    uint8_t local[sizeof g_fb];
    uint32_t seen = 0;

    for (;;) {
        if (g_fb_seq != seen) {
            xSemaphoreTake(g_fb_lock, portMAX_DELAY);
            memcpy(local, g_fb, sizeof local);
            int w = g_fb_w, h = g_fb_h, banks = g_fb_banks, mode = g_fb_mode;
            seen = g_fb_seq;
            xSemaphoreGive(g_fb_lock);

            ssd1306_show_lcd(local, w, h, banks, mode);
        }
        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

static int take_power_key(void) {
    int key;
    // While the phone is parked, any queued non-power keys are meaningless.
    // Drain them so a later PWR entry cannot be stuck behind them.
    while (xQueuePeek(g_emu.keyq, &key, 0) == pdTRUE) {
        xQueueReceive(g_emu.keyq, &key, 0);
        if (key == KK_PWR) return 1;
    }
    return 0;
}

static void emu_task(void* arg) {
    (void)arg;

    ESP_LOGI(TAG, "ESP32-S3 Nokia 3310 emulator");
    int rc = emu_harness_boot(&g_emu);
    if (rc) {
        ESP_LOGE(TAG, "boot failed (%d); check the 'fw' partition and PSRAM", rc);
        for (;;) {
            speaker_update(NULL);
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    const int64_t  frame_us = (int64_t)CONFIG_DCT3_FRAME_MS * 1000;
    const uint64_t cycles_per_frame =
        ((uint64_t)CONFIG_DCT3_EMU_HZ * (uint64_t)CONFIG_DCT3_FRAME_MS) / 1000u;

    int64_t next_frame = esp_timer_get_time();
    int64_t last_stats_us = next_frame;
    uint64_t last_stats_cycles = g_emu.cycles64;

    for (;;) {
        if (g_emu.halted) {
            emu_harness_service_status(&g_emu);
            emu_harness_save_settings(&g_emu, 1);
            speaker_update(NULL);
            publish_fb();
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (emu_harness_parked(&g_emu)) {
            emu_harness_service_status(&g_emu);
            emu_harness_save_settings(&g_emu, 1);
            speaker_update(NULL);
            publish_fb();
            if (take_power_key()) {
                ESP_LOGI(TAG, "power key while off: cold reboot");
                if (emu_harness_boot(&g_emu) == 0) {
                    next_frame = esp_timer_get_time();
                    last_stats_us = next_frame;
                    last_stats_cycles = g_emu.cycles64;
                }
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        next_frame += frame_us;
        const uint64_t target = g_emu.cycles64 + cycles_per_frame;
        g_emu.frame_target = target;

        // Run one display frame worth of emulated cycles. If the S3 cannot keep
        // up, this simply takes longer; pacing resynchronises below.
        while (g_emu.cycles64 < target &&
               !g_emu.halted && !emu_harness_parked(&g_emu)) {
            for (int i = 0; i < 256 && g_emu.cycles64 < target; ++i) {
                emu_harness_step(&g_emu);
            }
            if (g_emu.halted || emu_harness_parked(&g_emu))
                speaker_silence();
            else
                speaker_update(&g_emu.mad2);
        }

        publish_fb();

        emu_harness_save_settings(&g_emu, 0);
        emu_harness_service_status(&g_emu);
        int64_t now = esp_timer_get_time();
        if (next_frame > now) {
            int64_t wait_ms = (next_frame - now + 999) / 1000;
            vTaskDelay(pdMS_TO_TICKS(wait_ms));
        } else {
            if (now - next_frame > 1000000) {
                next_frame = now;          // drop only a backlog longer than 1 s
            }
            // Even when behind schedule, block briefly so IDLE1 can run and the
            // task watchdog stays happy.
            vTaskDelay(1);
        }

        if (now - last_stats_us >= 5000000) {
            double mhz = (double)(g_emu.cycles64 - last_stats_cycles) /
                         (double)(now - last_stats_us);
            ESP_LOGI(TAG, "emu %.2f MHz, %llu cycles, fb %dx%dx%d",
                     mhz, (unsigned long long)g_emu.cycles64,
                     g_fb_w, g_fb_h, g_fb_banks);
            emu_harness_prof_report();
            last_stats_us = now;
            last_stats_cycles = g_emu.cycles64;
        }
    }
}

void app_main(void) {
    esp_err_t speaker_err = speaker_init();
    if (speaker_err != ESP_OK) {
        ESP_LOGE(TAG, "speaker init failed: %s", esp_err_to_name(speaker_err));
    }
    esp_err_t err = ssd1306_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SSD1306 init failed: %s (running headless)", esp_err_to_name(err));
    }

    g_fb_lock = xSemaphoreCreateMutex();
    if (!g_fb_lock) {
        ESP_LOGE(TAG, "framebuffer mutex allocation failed");
        return;
    }

    emu_harness_init(&g_emu);
    if (!g_emu.keyq) {
        ESP_LOGE(TAG, "key queue allocation failed");
        return;
    }
    kbd_serial_start(g_emu.keyq, &g_emu.key_pending);

    if (xTaskCreatePinnedToCore(display_task, "display", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "display task creation failed");
    }
    if (xTaskCreatePinnedToCore(emu_task, "emu", 16384, NULL, 10, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "emulator task creation failed");
    }
}
