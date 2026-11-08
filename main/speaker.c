#include "speaker.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mad2/emu_host.h"

static const char* TAG = "speaker";
static int g_ready;
static int g_on;
static int g_tone_on;
static uint32_t g_freq = 1000;
static uint32_t g_hold_freq;
static int64_t g_chirp_until;
static int64_t g_test_start;
static int g_test_active;
static int g_test_request;

static int audible(uint32_t freq) {
    return freq >= 100 && freq <= 20000;
}

static void output_frequency(uint32_t freq) {
    if (!g_ready) return;
    if (!audible(freq)) {
        if (g_on) {
            ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
            g_on = 0;
        }
        return;
    }
    esp_err_t err = ESP_OK;
    if (freq != g_freq) {
        err = ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0, freq);
        if (err == ESP_OK) g_freq = freq;
    }
    if (err == ESP_OK && !g_on) {
        err = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 512);
        if (err == ESP_OK)
            err = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        if (err == ESP_OK) g_on = 1;
    }
    if (err != ESP_OK) {
        ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
        g_on = g_ready = 0;
        ESP_LOGE(TAG, "PWM output failed: %s", esp_err_to_name(err));
    }
}

esp_err_t speaker_init(void) {
    const int pin = CONFIG_DCT3_SPEAKER_GPIO;
    if (pin < 0) return ESP_OK;
    if (!GPIO_IS_VALID_OUTPUT_GPIO(pin) ||
        pin == CONFIG_DCT3_I2C_SDA || pin == CONFIG_DCT3_I2C_SCL) {
        return ESP_ERR_INVALID_ARG;
    }

    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = g_freq,
        .clk_cfg = LEDC_USE_APB_CLK,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    const ledc_channel_config_t channel = {
        .gpio_num = pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&channel);
    if (err == ESP_OK) {
        g_ready = 1;
        ESP_LOGI(TAG, "PC Speaker PWM on GPIO %d; send 'b' for three test notes", pin);
    }
    return err;
}

void speaker_silence(void) {
    g_chirp_until = 0;
    g_tone_on = 0;
    g_test_active = 0;
    output_frequency(0);
}

void speaker_request_test(void) {
    __atomic_store_n(&g_test_request, 1, __ATOMIC_RELAXED);
}

void speaker_update(Mad2* m) {
    if (!g_ready) return;
    const int64_t now = esp_timer_get_time();
    if (__atomic_exchange_n(&g_test_request, 0, __ATOMIC_RELAXED)) {
        g_test_start = now;
        g_test_active = 1;
        ESP_LOGI(TAG, "speaker test: 660, 880, 1320 Hz on GPIO %d", CONFIG_DCT3_SPEAKER_GPIO);
    }
    if (g_test_active) {
        static const uint32_t notes[] = { 660, 880, 1320 };
        const int64_t elapsed = now - g_test_start;
        if (elapsed < 480000) {
            output_frequency(elapsed % 160000 < 120000 ? notes[elapsed / 160000] : 0);
            return;
        }
        g_test_active = 0;
        g_chirp_until = 0;
        g_tone_on = 0;
    }
    if (!m) {
        g_chirp_until = 0;
        g_tone_on = 0;
        output_frequency(0);
        return;
    }
    const unsigned chirp = (unsigned)emu_buzzer_chirp_drain(m);
    const uint32_t edge_div = chirp >> 8;
    const uint16_t div = emu_buzzer_div(m);
    const uint32_t buzz_freq = div ? 13000000u / div : 0;
    if ((chirp & 0xFFu) && (edge_div || div)) {
        g_hold_freq = 13000000u / (edge_div ? edge_div : div);
        g_chirp_until = now + 15000;
    }
    const DspOps* dsp = m->dsp_override ? m->dsp_override
                                      : (m->model ? m->model->dsp : NULL);
    int f1 = 0, f2 = 0;
    const int tone_on = dsp && dsp->hle_tone && dsp->hle_tone(m, &f1, &f2) && f1 > 0;
    if (tone_on && !g_tone_on) {
        g_hold_freq = (uint32_t)f1;
        g_chirp_until = now + 15000;
    }
    g_tone_on = tone_on;
    // One PWM voice: ringtone/game buzzer first, then the DSP keypad tone.
    // Dual-frequency DTMF / codec PCM require a sampled-audio output.
    uint32_t freq = 0;
    if (emu_buzzer_on(m) && audible(buzz_freq)) freq = buzz_freq;
    else if (tone_on && audible((uint32_t)f1)) freq = (uint32_t)f1;
    else if (now < g_chirp_until) freq = g_hold_freq;
    output_frequency(freq);
}
