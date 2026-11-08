// Device-side harness: boot the 3310 firmware from the "fw" partition and run
// the same per-instruction orchestration as the upstream web harness.
#include "emu_harness.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_cpu.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "settings_store.h"

#include "mad2/emu_host.h"

static const char* TAG = "dct3";
static SettingsStore g_settings;
static uint32_t g_settings_crc, g_settings_programs, g_settings_erases;
static int g_settings_dirty;
static int64_t g_settings_changed, g_settings_retry;
static int g_status_request;
#ifdef CONFIG_DCT3_CODE_CACHE
static uint8_t* g_code_cache;
static uint32_t g_code_tags[64];
#endif

#ifdef CONFIG_DCT3_CODE_CACHE
static void device_write(void* ctx, uint32_t pc, uint32_t addr, int size, uint32_t value) {
    Mad2* m = ctx;
    mad2_write(ctx, pc, addr, size, value);
    if (addr >= m->model->mem.flash_base && addr < m->model->mem.eeprom_base) {
        EmuHarness* h = (EmuHarness*)((uint8_t*)m - offsetof(EmuHarness, mad2));
        dct3_invalidate_code_cache(h->core);
    }
}
#endif

void emu_harness_request_status(void) {
    __atomic_store_n(&g_status_request, 1, __ATOMIC_RELAXED);
}

void emu_harness_service_status(EmuHarness* h) {
    if (!__atomic_exchange_n(&g_status_request, 0, __ATOMIC_RELAXED)) return;
    ESP_LOGI(TAG, "RAM free %u KiB, largest %u KiB; settings generation %lu, dirty %d, writes %lu erases %lu Vpp %u busy %u",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned long)g_settings.sequence, g_settings_dirty,
             (unsigned long)h->mad2.flash_programs, (unsigned long)h->mad2.flash_erases,
             (unsigned)h->mad2.flash_vpp, (unsigned)h->mad2.flash_busy);
    emu_harness_save_settings(h, 1);
}

void emu_harness_save_settings(EmuHarness* h, int force) {
    if (!h->booted || !g_settings.partition) return;
    const int64_t now = esp_timer_get_time();
    if (g_settings_programs != h->mad2.flash_programs ||
        g_settings_erases != h->mad2.flash_erases) {
        g_settings_programs = h->mad2.flash_programs;
        g_settings_erases = h->mad2.flash_erases;
        g_settings_dirty = 1;
        g_settings_changed = now;
    }
    if ((!g_settings_dirty && (!force || g_settings.active_slot >= 0)) || now < g_settings_retry ||
        (!force && now - g_settings_changed < 1000000) ||
        h->mad2.flash_busy || h->mad2.flash_prog || h->mad2.flash_erase)
        return;
    const uint8_t* data = h->core->ram + h->model->mem.eeprom_base;
    uint32_t crc = esp_rom_crc32_le(0, data, g_settings.size);
    if (crc == g_settings_crc && g_settings.active_slot >= 0) {
        g_settings_dirty = 0; return;
    }
    esp_err_t err = settings_store_save(&g_settings, data);
    if (err == ESP_OK) {
        g_settings_crc = crc;
        g_settings_dirty = 0;
        ESP_LOGI(TAG, "settings saved: %u KiB, generation %lu, CRC %08lx",
                 (unsigned)(g_settings.size >> 10), (unsigned long)g_settings.sequence,
                 (unsigned long)crc);
    } else {
        g_settings_retry = now + 10000000;
        ESP_LOGE(TAG, "settings save failed: %s", esp_err_to_name(err));
    }
}

// Optional profiling; compile it out of the normal per-instruction path.
#ifdef CONFIG_DCT3_PROFILE
static uint32_t g_prof_obs, g_prof_step, g_prof_tick, g_prof_rest;
static uint32_t g_prof_steps;
static uint32_t g_prof_thumb;
static uint32_t g_prof_idle;
#endif

void emu_harness_prof_report(void) {
#ifdef CONFIG_DCT3_PROFILE
    uint32_t o = g_prof_obs, s = g_prof_step, t = g_prof_tick, r = g_prof_rest;
    uint32_t n = g_prof_steps;
    uint32_t thumb = g_prof_thumb;
    uint32_t idle = g_prof_idle;
    g_prof_obs = g_prof_step = g_prof_tick = g_prof_rest = 0;
    g_prof_steps = 0;
    g_prof_thumb = 0;
    g_prof_idle = 0;
    uint64_t sum = (uint64_t)o + s + t + r;
    if (!n || !sum) return;
    ESP_LOGI(TAG, "prof: obs %2u%% step %2u%% tick %2u%% poll %2u%% | %.0f host cyc/insn",
             (unsigned)(100ull * o / sum), (unsigned)(100ull * s / sum),
             (unsigned)(100ull * t / sum), (unsigned)(100ull * r / sum),
             (double)sum / (double)n);
    ESP_LOGI(TAG, "prof: Thumb %u%%", (unsigned)(100ull * thumb / n));
    ESP_LOGI(TAG, "prof: skipped idle %u instructions", (unsigned)idle);
#endif
}

#define DCT3_CYCLE_REBASE 0x20000000
#define PWR_RELEASE_STEP  12000000LL
#define KEY_HOLD_INSNS    200000LL
#define KEY_SETTLE_INSNS  1200000LL
#define KEY_Q_N           16
#define FW_READ_CHUNK     4096

// Instructions executed between two harness bookkeeping passes. The mGBA
// interpreter itself is a tight IRAM loop; calling observe/classify/timers for
// every guest instruction tripled the cost. Eight keeps interrupt delivery and
// the key sequencer within ~3 us of a per-instruction loop at real-time speed.
#define EMU_BATCH         8

// The upstream core calls getenv() for diagnostic switches. The component is
// compiled with -Dgetenv=dct3_getenv; on the device every switch is off.
char* dct3_getenv(const char* name) {
    (void)name;
    return NULL;
}

// Keep CPU registers and memory callbacks in fast internal RAM; only the
// large flat guest address space needs PSRAM. Both allocations use this hook.
static void* psram_calloc(size_t nmemb, size_t size) {
    if (size && nmemb > SIZE_MAX / size) return NULL;
    const uint32_t caps = nmemb * size >= DCT3_RAM_SIZE
        ? MALLOC_CAP_SPIRAM : MALLOC_CAP_INTERNAL;
    void* p = heap_caps_calloc(nmemb, size, caps | MALLOC_CAP_8BIT);
    if (!p) p = calloc(nmemb, size);
    return p;
}

void emu_harness_init(EmuHarness* h) {
    memset(h, 0, sizeof *h);
    h->keyq = xQueueCreate(KEY_Q_N, sizeof(int));
    h->key_hold_insns = KEY_HOLD_INSNS;
    h->key_seq_at = -1;
    h->key_pending = 0;
}

static int load_firmware(EmuHarness* h) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, CONFIG_DCT3_FW_LABEL);
    if (!part) {
        ESP_LOGE(TAG, "partition '%s' not found (check partitions.csv)", CONFIG_DCT3_FW_LABEL);
        return -1;
    }

    const uint32_t flash_base = h->model->mem.flash_base;
    const uint32_t fw_size    = h->model->mem.flash_size;
    if (part->size < fw_size) {
        ESP_LOGE(TAG, "partition '%s' is %u bytes, need %u",
                 CONFIG_DCT3_FW_LABEL, (unsigned)part->size, (unsigned)fw_size);
        return -2;
    }

    uint8_t* dst = h->core->ram + flash_base;
    for (uint32_t off = 0; off < fw_size; off += FW_READ_CHUNK) {
        uint32_t n = fw_size - off;
        if (n > FW_READ_CHUNK) n = FW_READ_CHUNK;
        esp_err_t err = esp_partition_read(part, off, dst + off, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_partition_read(%u) failed: %s", (unsigned)off, esp_err_to_name(err));
            return -3;
        }
    }

    // A blank partition reads as 0xFF. Refuse to "boot" it: the CPU would run
    // an erased vector table and look like a random crash.
    int erased = 1;
    for (int i = 0; i < 4096; ++i) {
        if (dst[i] != 0xFF) { erased = 0; break; }
    }
    if (erased) {
        ESP_LOGE(TAG, "firmware partition is blank; flash a .fls at offset 0x1A0000");
        return -4;
    }

    // The registry is compiled down to the 3310, but keep the upstream
    // signature check as a sanity warning (some cooked images lack the string).
    const ModelProfile* det = model_detect(dst, fw_size);
    if (!det) {
        ESP_LOGW(TAG, "no NHM-5 model signature in the image; booting the 3310 profile anyway");
    } else {
        h->model = det;
    }

    if (dct3_fix_mcu_checksum(h->core)) {
        ESP_LOGI(TAG, "repaired the MCU flash checksum for a patched image");
    }
    return 0;
}

int emu_harness_boot(EmuHarness* h) {
    emu_harness_save_settings(h, 1);
    if (!h->keyq) h->keyq = xQueueCreate(KEY_Q_N, sizeof(int));
    else          xQueueReset(h->keyq);

    h->booted = 0;
    h->halted = 0;
    h->step = 0;
    h->cycles64 = 0;
    h->pwr_auto = 1;
    h->pwr_released = 0;
    h->key_seq_state = 0;
    h->key_seq_at = -1;
    h->key_row = h->key_col = h->key_special = 0;
    h->key_pending = 0;
    h->model = model_default();

    size_t psram = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (!h->core) {
        dct3_core_set_allocator(psram_calloc, free);
        h->core = dct3_core_create();
        if (!h->core) {
            ESP_LOGE(TAG, "dct3_core_create failed (PSRAM total=%u, need %u MiB)",
                     (unsigned)psram, (unsigned)(DCT3_RAM_SIZE >> 20));
            return -5;
        }
    } else {
        dct3_core_reset(h->core);
    }

    // Cold power-on: wipe working RAM, exactly like dct3_web_boot().
    memset(h->core->ram, 0, DCT3_RAM_SIZE);

    int rc = load_firmware(h);
    if (rc) return rc;

    const uint32_t identity = esp_rom_crc32_le(0,
        h->core->ram + h->model->mem.flash_base,
        h->model->mem.eeprom_base - h->model->mem.flash_base);
    uint8_t* settings = h->core->ram + h->model->mem.eeprom_base;
    esp_err_t settings_err = settings_store_load(&g_settings, identity, settings,
                                                 h->model->mem.eeprom_size);
    if (settings_err == ESP_OK) {
        ESP_LOGI(TAG, "settings restored: generation %lu, CRC %08lx",
                 (unsigned long)g_settings.sequence,
                 (unsigned long)esp_rom_crc32_le(0, settings, h->model->mem.eeprom_size));
    } else if (settings_err == ESP_ERR_NOT_FOUND && g_settings.partition) {
        ESP_LOGI(TAG, "no saved settings; using firmware defaults");
    } else {
        ESP_LOGE(TAG, "settings unavailable: %s", esp_err_to_name(settings_err));
    }
    g_settings_crc = esp_rom_crc32_le(0, settings, h->model->mem.eeprom_size);
    g_settings_programs = g_settings_erases = 0;
    g_settings_dirty = 0;
    g_settings_retry = 0;

    memset(&h->mad2, 0, sizeof h->mad2);
    mad2_init(&h->mad2, h->model);

    h->mad2.sim_present = 1;
    h->mad2.verbose = 0;
    h->mad2.mem = h->core->ram;
    h->mad2.mem_mask = DCT3_RAM_MASK;
    h->mad2.pcm_sink = NULL;          // speaker.c outputs buzzer / DSP tones via LEDC PWM

    model_resolve(h->model, h->core->ram, DCT3_RAM_MASK, &h->mad2.fw);

    dct3_set_io_hooks(h->core, &h->mad2,
                      h->model->mem.io_lo, h->model->mem.io_hi,
                      mad2_read,
#ifdef CONFIG_DCT3_CODE_CACHE
                      device_write);
#else
                      mad2_write);
#endif
    dct3_set_io_range2(h->core,
                       h->model->mem.flash_base,
                       h->model->mem.flash_base + h->model->mem.flash_size);

#ifdef CONFIG_DCT3_CODE_CACHE
    if (!g_code_cache)
        g_code_cache = heap_caps_malloc(16384, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (g_code_cache)
        dct3_set_code_cache(h->core, g_code_cache, g_code_tags, h->model->mem.eeprom_base);
#endif

    dct3_boot_at(h->core, h->model->mem.boot_entry);

    h->key_hold_insns = h->model->keypad.hold_insns
                      ? h->model->keypad.hold_insns : KEY_HOLD_INSNS;

    memset(&h->hc, 0, sizeof h->hc);
    harness_init(&h->hc, &h->mad2, 1 /* recover */, 0 /* spin default */, 16 /* wild-PC warmup */);

    h->booted = 1;
    ESP_LOGI(TAG, "internal RAM free %u KiB, largest block %u KiB, core %u bytes",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)sizeof *h->core);
    ESP_LOGI(TAG, "booted %s: flash %u KiB from '%s', PSRAM %u KiB",
             h->model->name,
             (unsigned)(h->model->mem.flash_size >> 10),
             CONFIG_DCT3_FW_LABEL,
             (unsigned)(psram >> 10));
    return 0;
}

int emu_harness_queue_key(EmuHarness* h, int key_id) {
    if (!h->keyq) return -1;
    int rc = xQueueSend(h->keyq, &key_id, 0) == pdTRUE ? 0 : -1;
    if (rc == 0) h->key_pending = 1;
    return rc;
}

int IRAM_ATTR emu_harness_parked(const EmuHarness* h) {
    return h->booted && h->mad2.power_off && h->step > PWR_RELEASE_STEP;
}

static void IRAM_ATTR release_power_key(EmuHarness* h) {
    if (h->pwr_auto && !h->pwr_released && h->step >= PWR_RELEASE_STEP) {
        h->mad2.kbd_special_cols = 0;      // release the held power key
        h->mad2.irq_pending |= 0x01;       // keypad IRQ so the up-edge is seen
        h->pwr_released = 1;
    }
}

// Cheap inlined equivalents of mad2_irq_poll/mad2_fiq_poll's "is anything
// pending" test. Most steps have no interrupt, and the two out-of-line polls
// were ~180 host cycles per step.
static inline int IRAM_ATTR irq_ready(const Mad2* m) {
    if (m->intctrl_gate && (m->int_ctrl & 0x08)) return 0;
    return (m->irq_pending & ~m->irq_mask) != 0;
}
static inline int IRAM_ATTR fiq_ready(const Mad2* m) {
    if (m->intctrl_gate && (m->int_ctrl & 0x02)) return 0;
    if (m->fiq_pending & ~m->fiq_mask) return 1;
    return (m->fiq8_ctrl & 0x02) && !(m->fiq8_ctrl & 0x04);
}

// One tap at a time: down for key_hold_insns, release, then settle before the
// next queued key. Mirrors src/web/main.c's g_key_seq_state machine.
//
// The queue is only probed when the producer raised key_pending: xQueueReceive()
// is far too expensive to call for every guest instruction. After a successful
// dequeue the hint is raised again so a second already-queued key is seen after
// the hold/settle window; it falls back to 0 on the first empty probe.
static void IRAM_ATTR run_key_sequencer(EmuHarness* h) {
    if (h->key_seq_state == 0) {
        if (!h->key_pending) return;
        h->key_pending = 0;
        int key_id;
        if (xQueueReceive(h->keyq, &key_id, 0) != pdTRUE) return;
        h->key_pending = 1;

        const KeyLine* line = emu_keyline(h->model, key_id);
        if (!line) return;                 // key absent on this model

        h->key_row = line->row;
        h->key_col = line->col;
        h->key_special = line->special;
        if (line->special) {
            h->mad2.kbd_special_cols |= line->special;
        } else {
            h->mad2.kbd_norm_cols[line->row & 7] |=
                (uint8_t)(1u << (line->col & 7));
        }
        mad2_keypad_irq(&h->mad2);

        h->key_seq_state = 1;
        h->key_seq_at = h->step + h->key_hold_insns;
    } else if (h->key_seq_state == 1) {
        if (h->step >= h->key_seq_at) {
            if (h->key_special) {
                h->mad2.kbd_special_cols &= (uint8_t)~h->key_special;
            } else {
                h->mad2.kbd_norm_cols[h->key_row & 7] &=
                    (uint8_t)~(1u << (h->key_col & 7));
            }
            mad2_keypad_irq(&h->mad2);
            h->key_seq_state = 2;
            h->key_seq_at = h->step + KEY_SETTLE_INSNS;
        }
    } else {
        if (h->step >= h->key_seq_at) {
            h->key_seq_state = 0;
            h->key_seq_at = -1;
        }
    }
}

void IRAM_ATTR emu_harness_step(EmuHarness* h) {
    if (!h->booted || h->halted) return;
    if (emu_harness_parked(h)) return;

#ifdef CONFIG_DCT3_PROFILE
    uint32_t c0 = esp_cpu_get_cycle_count();
#endif

    HarnessStatus status = harness_observe(&h->hc, &h->mad2, h->core, h->step);
#ifdef CONFIG_DCT3_PROFILE
    uint32_t c1 = esp_cpu_get_cycle_count();
    g_prof_obs += c1 - c0;
#endif
    if (status == HARNESS_FAULT_HALT) {
        harness_fault_report(&h->hc, &h->mad2, h->core, h->hc.fault_reason);
        ESP_LOGE(TAG, "unrecovered fault: %s\n%s",
                 h->hc.fault_reason ? h->hc.fault_reason : "?",
                 h->mad2.postmortem);
        h->halted = 1;
        return;
    }

    release_power_key(h);
    run_key_sequencer(h);
#ifdef CONFIG_DCT3_PROFILE
    uint32_t c1b = esp_cpu_get_cycle_count();
    g_prof_obs += c1b - c1;
    c1 = c1b;
#endif

    const int64_t step_before = h->step;
#ifdef CONFIG_DCT3_PROFILE
    const int was_thumb = h->core->cpu.executionMode == MODE_THUMB;
#endif
    int32_t cyc_before = h->core->cpu.cycles;
    unsigned retired = 0;
    // Only after startup and while input is quiet. Bound the skip by the
    // platform's next event, the core rebase event, and a 1 ms upper limit.
    if (h->pwr_released && h->mad2.rtc_mono > 130000000u &&
        !h->mad2.recover_pending &&
        (h->core->cpu.prefetch[0] == 0x7830 ||
         (uint32_t)((uint32_t)h->core->cpu.gprs[15] - 2u - h->core->byte_poll_pc) < 8u) &&
        !irq_ready(&h->mad2) && !fiq_ready(&h->mad2)) {
        mad2_timers_tick(&h->mad2, (uint32_t)h->core->cpu.cycles);
        uint64_t next = mad2_next_event_cycle(&h->mad2);
        if (next > h->mad2.rtc_mono) {
            uint64_t limit = next - h->mad2.rtc_mono;
            if (limit > 13000u) limit = 13000u;
            if (h->frame_target > h->cycles64 &&
                limit > h->frame_target - h->cycles64)
                limit = h->frame_target - h->cycles64;
            // A key stays held/released for an exact retired-instruction count.
            // Each idle iteration costs 12 cycles for 4 instructions, so cap
            // the skip at the next key transition even while a key is held.
            if (h->key_seq_state && h->key_seq_at > h->step &&
                limit > (uint64_t)(h->key_seq_at - h->step) * 3u)
                limit = (uint64_t)(h->key_seq_at - h->step) * 3u;
            if (!irq_ready(&h->mad2) && !fiq_ready(&h->mad2))
                retired = dct3_skip_byte_poll(h->core, (uint32_t)limit);
        }
    }
#ifdef CONFIG_DCT3_PROFILE
    g_prof_idle += retired;
#endif
    if (!retired) {
        dct3_run(h->core, EMU_BATCH);
        retired = EMU_BATCH;
    }
#ifdef CONFIG_DCT3_PROFILE
    if (was_thumb) g_prof_thumb += retired;
#endif
    int32_t cyc_delta = h->core->cpu.cycles - cyc_before;
    if (cyc_delta < 0) cyc_delta += DCT3_CYCLE_REBASE;   // undo the periodic rebase
    h->cycles64 += (uint32_t)cyc_delta;
    h->step += retired;
#ifdef CONFIG_DCT3_PROFILE
    uint32_t c2 = esp_cpu_get_cycle_count();
    g_prof_step += c2 - c1;
#endif

    // The platform tick is cycle-driven and the fastest consumer (Timer0) runs
    // at 33 kHz: one tick per 32 guest instructions is far below one timer
    // period, so batching it does not change timing but removes the 64-bit
    // divide + model pumps from the hot path.
    if (retired != EMU_BATCH || ((step_before & 31) == 0))
        mad2_timers_tick(&h->mad2, (uint32_t)h->core->cpu.cycles);
#ifdef CONFIG_DCT3_PROFILE
    uint32_t c3 = esp_cpu_get_cycle_count();
    g_prof_tick += c3 - c2;
#endif

    // post_step only acts on a pending recovery; telemetry is compiled out on
    // the device, so skip the call in the common case.
    if (h->mad2.recover_pending)
        harness_post_step(&h->hc, &h->mad2, h->core);

    int raised = 0;
    if (!h->core->cpu.cpsr.f && fiq_ready(&h->mad2)) {
        int fiq = mad2_fiq_poll(&h->mad2);
        if (fiq >= 0) {
            ARMRaiseFIQ(&h->core->cpu);
            h->mad2.fiqs_raised++;
            raised = 1;
        }
    }
    if (!raised && !h->core->cpu.cpsr.i && irq_ready(&h->mad2)) {
        int irq = mad2_irq_poll(&h->mad2);
        if (irq >= 0) {
            ARMRaiseIRQ(&h->core->cpu);
            h->mad2.irqs_raised++;
        }
    }

#ifdef CONFIG_DCT3_PROFILE
    uint32_t c4 = esp_cpu_get_cycle_count();
    g_prof_rest += c4 - c3;
    g_prof_steps += retired;
#endif
}
