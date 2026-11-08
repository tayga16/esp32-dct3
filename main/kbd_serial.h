#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Start the keyboard reader task (USB Serial/JTAG or UART, per Kconfig).
// Received characters are translated to KeyId values and pushed into `keyq`
// (consumed by the emulator task). `key_pending` is raised after every send so
// the hot loop can skip xQueueReceive() while no key is waiting.
void kbd_serial_start(QueueHandle_t keyq, volatile int* key_pending);
