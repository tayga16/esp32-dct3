// Terminal -> Nokia keypad. Type in the serial console and the emulator gets a
// logical key tap through the same timed sequencer as the web UI.
//
// Two backends are supported:
//   - USB Serial/JTAG: intended for boards whose only data port is the native
//     USB port. If the console already owns USB Serial/JTAG we read stdin
//     (the VFS registered by the console); otherwise we install the driver.
//   - UART: a plain 3.3 V serial adapter on the configured pins.
#include "kbd_serial.h"
#include "speaker.h"
#include "emu_harness.h"

#include <stdint.h>
#include <unistd.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "models/model.h"

#if CONFIG_DCT3_KBD_BACKEND_USB_SERIAL_JTAG
#include "esp_vfs_usb_serial_jtag.h"
#if !CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#endif
#else
#include "driver/uart.h"
#endif

static const char* TAG = "kbd";

static QueueHandle_t s_keyq;
static volatile int* s_key_pending;

static int map_char(uint8_t c) {
    if (c >= '0' && c <= '9') return KK_0 + (c - '0');
    switch (c) {
        case '*':                    return KK_STAR;
        case '#':                    return KK_HASH;
        case 'u': case 'U':          return KK_UP;
        case 'd': case 'D':          return KK_DOWN;
        case 'm': case 'M':          return KK_SOFT1;
        case 'c': case 'C':          return KK_SOFT2;
        case 0x7F: case 0x08:        return KK_SOFT2;   // Backspace = C / clear
        case '\r': case '\n':        return KK_SOFT1;   // Enter = Menu / select
        case 'p': case 'P':          return KK_PWR;
        default:                     return -1;
    }
}

// Arrow keys from a normal terminal arrive as ESC [ A / B.
static void feed_byte(uint8_t c, int* esc) {
    int key = -1;
    if (*esc == 0 && (c == 'v' || c == 'V')) {
        emu_harness_request_status();
        return;
    }
    if (*esc == 0 && (c == 'b' || c == 'B')) {
        speaker_request_test();
        return;
    }

    if (*esc == 0 && c == 0x1B) { *esc = 1; return; }
    if (*esc == 1) {
        if (c == '[') { *esc = 2; return; }
        *esc = 0;
        return;
    }
    if (*esc == 2) {
        if (c == 'A') key = KK_UP;
        else if (c == 'B') key = KK_DOWN;
        *esc = 0;
    } else {
        key = map_char(c);
    }

    if (key >= 0) {
        xQueueSend(s_keyq, &key, 0);
        *s_key_pending = 1;
    }
}

#if CONFIG_DCT3_KBD_BACKEND_USB_SERIAL_JTAG

static void kbd_task(void* arg) {
    (void)arg;
    int esc = 0;

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    // The console VFS reads the USB Serial/JTAG RX FIFO directly; installing the
    // driver as well would steal bytes from it. Enter arrives as CR here, so ask
    // the VFS to normalise it to LF instead of waiting for a CRLF pair.
    esp_vfs_dev_usb_serial_jtag_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
    ESP_LOGI(TAG, "keyboard on USB Serial/JTAG (console stdin)");
#else
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.tx_buffer_size = 256;
    cfg.rx_buffer_size = 1024;
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB Serial/JTAG driver install failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "keyboard on USB Serial/JTAG (driver)");
#endif

    ESP_LOGI(TAG, "0-9 * # | u/d=up/down | m or Enter=Menu | c/Backspace=Names/C | p=PWR");

    for (;;) {
        uint8_t c;
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n != 1) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
#else
        int n = usb_serial_jtag_read_bytes(&c, 1, pdMS_TO_TICKS(50));
        if (n != 1) continue;
#endif
        feed_byte(c, &esc);
    }
}

#else  // UART backend

static void kbd_task(void* arg) {
    (void)arg;
    const uart_port_t port = (uart_port_t)CONFIG_DCT3_KBD_UART;

    uart_config_t cfg = {
        .baud_rate = CONFIG_DCT3_KBD_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_param_config(port, &cfg));
#if CONFIG_DCT3_KBD_UART_TX >= 0 || CONFIG_DCT3_KBD_UART_RX >= 0
    ESP_ERROR_CHECK(uart_set_pin(port,
                                 CONFIG_DCT3_KBD_UART_TX,
                                 CONFIG_DCT3_KBD_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
#endif
    ESP_ERROR_CHECK(uart_driver_install(port, 2048, 0, 0, NULL, 0));

    ESP_LOGI(TAG, "keyboard on UART%d @ %d baud", CONFIG_DCT3_KBD_UART, CONFIG_DCT3_KBD_BAUD);
    ESP_LOGI(TAG, "0-9 * # | u/d=up/down | m or Enter=Menu | c/Backspace=Names/C | p=PWR");

    int esc = 0;
    for (;;) {
        uint8_t c;
        int n = uart_read_bytes(port, &c, 1, pdMS_TO_TICKS(50));
        if (n != 1) continue;
        feed_byte(c, &esc);
    }
}

#endif

void kbd_serial_start(QueueHandle_t keyq, volatile int* key_pending) {
    s_keyq = keyq;
    s_key_pending = key_pending;
    xTaskCreatePinnedToCore(kbd_task, "kbd", 4096, NULL, 4, NULL, 0);
}
