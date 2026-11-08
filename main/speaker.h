#pragma once

#include "esp_err.h"
#include "mad2/mad2.h"

// Passive PC Speaker / buzzer: LEDC PWM, GPIO 5 by default.
esp_err_t speaker_init(void);
void speaker_update(Mad2* m);
void speaker_silence(void);
// Thread-safe request; the emulator task plays three notes on its next update.
void speaker_request_test(void);
