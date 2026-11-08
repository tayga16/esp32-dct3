// Device-side harness: the ESP32 equivalent of src/web/main.c's boot + step
// loop. It owns the DCT3 core, the MAD2 model, the shared fault harness and the
// timed key sequencer. The display / UART tasks only talk to it through the
// published framebuffer and the key queue.
#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "core/dct3_core.h"
#include "harness/harness.h"
#include "mad2/mad2.h"
#include "models/model.h"

typedef struct EmuHarness {
    DCT3Core*          core;
    const ModelProfile* model;
    Mad2               mad2;
    HarnessConfig      hc;

    int64_t  step;          // retired instructions since boot
    uint64_t cycles64;      // accumulated ARM cycles (survives the core rebase)
    uint64_t frame_target;  // pacing bound: idle skips must not overshoot this

    int      key_hold_insns;
    int      key_seq_state; // 0 idle, 1 holding, 2 settling
    int64_t  key_seq_at;
    int      key_row, key_col, key_special;

    int      pwr_auto;      // release the power-on key hold automatically
    int      pwr_released;
    int      booted;
    int      halted;        // an unrecovered fault stopped the CPU

    QueueHandle_t keyq;     // KeyId values, depth 16
    volatile int  key_pending; // producer hint: skip xQueueReceive when zero
} EmuHarness;

// Zero the struct and create the key queue.
void emu_harness_init(EmuHarness* h);

// Load the .fls from the "fw" partition, initialise MAD2 and enter the firmware.
// Returns 0 on success, negative on error. Can be called again to cold-reboot.
int  emu_harness_boot(EmuHarness* h);

// Execute one instruction with the same ordering as src/web/main.c:
// observe -> power release -> key sequencer -> step -> timers -> post -> IRQ/FIQ.
void emu_harness_step(EmuHarness* h);

// Queue a logical key (KeyId from models/model.h). Returns 0 on success.
int  emu_harness_queue_key(EmuHarness* h, int key_id);

// True once the firmware has powered itself off (after the power-key hold).
int  emu_harness_parked(const EmuHarness* h);
// Persist EEPROM after the guest completes flash writes and becomes quiet.
void emu_harness_save_settings(EmuHarness* h, int force);
void emu_harness_request_status(void);
void emu_harness_service_status(EmuHarness* h);

// Temporary: log and reset the per-stage host-cycle profile.
void emu_harness_prof_report(void);
