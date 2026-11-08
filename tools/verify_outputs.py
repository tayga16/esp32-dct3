"""Compile the real display/buzzer drivers against host GPIO and I2C mocks."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GCC = Path("C:/msys64/mingw64/bin/gcc.exe")

STUBS = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static inline const char *esp_err_to_name(int e) { (void)e; return "mock"; }
#define CONFIG_DCT3_SPEAKER_GPIO 5
#define CONFIG_DCT3_I2C_SDA 8
#define CONFIG_DCT3_I2C_SCL 9
#define CONFIG_DCT3_I2C_PORT 0
#define CONFIG_DCT3_I2C_ADDR 0x3c
#define CONFIG_DCT3_I2C_HZ 400000
#define GPIO_IS_VALID_OUTPUT_GPIO(p) ((p)>=0 && (p)<49)
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode,pull_up_en,pull_down_en,intr_type; } gpio_config_t;
esp_err_t gpio_set_level(int pin, int level);
esp_err_t gpio_config(const gpio_config_t *config);
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_TIMER_10_BIT 10
#define LEDC_TIMER_0 0
#define LEDC_CHANNEL_0 0
#define LEDC_USE_APB_CLK 1
#define LEDC_INTR_DISABLE 0
typedef struct { int speed_mode,duty_resolution,timer_num; uint32_t freq_hz; int clk_cfg; } ledc_timer_config_t;
typedef struct { int gpio_num,speed_mode,channel,intr_type,timer_sel; uint32_t duty,hpoint; } ledc_channel_config_t;
esp_err_t ledc_timer_config(const ledc_timer_config_t*);
esp_err_t ledc_channel_config(const ledc_channel_config_t*);
esp_err_t ledc_set_freq(int,int,uint32_t);
esp_err_t ledc_set_duty(int,int,uint32_t);
esp_err_t ledc_update_duty(int,int);
esp_err_t ledc_stop(int,int,int);
int64_t esp_timer_get_time(void);
typedef struct Mad2 Mad2;
typedef struct { int (*hle_tone)(Mad2*,int*,int*); } DspOps;
typedef struct { const DspOps *dsp; } ModelProfile;
struct Mad2 { const ModelProfile *model; const DspOps *dsp_override; int buzz_on,edges,tone; uint16_t div; };
static inline int emu_buzzer_on(const Mad2 *m) { return m->buzz_on; }
static inline uint16_t emu_buzzer_div(const Mad2 *m) { return m->div; }
static inline int emu_buzzer_chirp_drain(Mad2 *m) { int e=m->edges; m->edges=0; return e; }
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int i2c_port,sda_io_num,scl_io_num,clk_source,glitch_ignore_cnt;
    struct { bool enable_internal_pullup; } flags; } i2c_master_bus_config_t;
typedef struct { int dev_addr_length,device_address,scl_speed_hz; } i2c_device_config_t;
#define I2C_CLK_SRC_DEFAULT 0
#define I2C_ADDR_BIT_LEN_7 0
#define pdMS_TO_TICKS(x) (x)
static inline void vTaskDelay(int ticks) { (void)ticks; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t*,i2c_master_bus_handle_t*);
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t,const i2c_device_config_t*,i2c_master_dev_handle_t*);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t,const uint8_t*,size_t,int);
'''

TEST = r'''
#include "mock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "ssd1306.h"
#include "speaker.h"
static uint8_t panel[8][128];
static int page,col,endcol,packets,bytes,fail_data,fail_command,level;
static uint32_t pwm_freq, tone_freq=900, duty;
static int freq_changes, fail_freq;
static int64_t now;
int64_t esp_timer_get_time(void) { return now; }
esp_err_t gpio_set_level(int pin,int value) { assert(pin==5); level=value; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *cfg) { assert(cfg->pin_bit_mask==(1ULL<<5)); return ESP_OK; }
esp_err_t ledc_timer_config(const ledc_timer_config_t *cfg) { assert(cfg->duty_resolution==10); pwm_freq=cfg->freq_hz; return ESP_OK; }
esp_err_t ledc_channel_config(const ledc_channel_config_t *cfg) { assert(cfg->gpio_num==5 && cfg->duty==0); level=0; return ESP_OK; }
esp_err_t ledc_set_freq(int mode,int timer,uint32_t freq) { (void)mode; (void)timer; if(fail_freq) return 3; pwm_freq=freq; freq_changes++; return ESP_OK; }
esp_err_t ledc_set_duty(int mode,int channel,uint32_t value) { (void)mode; (void)channel; assert(value==512); duty=value; return ESP_OK; }
esp_err_t ledc_update_duty(int mode,int channel) { (void)mode; (void)channel; assert(duty==512); level=1; return ESP_OK; }
esp_err_t ledc_stop(int mode,int channel,int idle) { (void)mode; (void)channel; assert(idle==0); level=0; return ESP_OK; }
esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg,void **bus) { (void)cfg; *bus=(void*)1; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(void *bus,const i2c_device_config_t *cfg,void **dev) { (void)bus; (void)cfg; *dev=(void*)1; return ESP_OK; }
esp_err_t i2c_master_bus_rm_device(void *dev) { (void)dev; return ESP_OK; }
esp_err_t i2c_master_transmit(void *dev,const uint8_t *buf,size_t n,int timeout) {
    (void)dev; (void)timeout; packets++;
    if(buf[0]==0x00 && n==7 && buf[1]==0x21) {
        if(fail_command) { fail_command=0; return 3; }
        col=buf[2]; endcol=buf[3]; page=buf[5];
        assert(page<8 && col<=endcol && endcol<128);
    } else if(buf[0]==0x40) {
        if(fail_data) { fail_data=0; return 3; }
        assert(n-1==(size_t)(endcol-col+1));
        memcpy(&panel[page][col],buf+1,n-1); bytes+=(int)n-1;
    }
    return ESP_OK;
}
static int tone(Mad2 *m,int *f1,int *f2) { *f1=(int)tone_freq; *f2=0; return m->tone; }
static int muted(Mad2 *m,int *f1,int *f2) { (void)m; (void)f1; (void)f2; return 0; }
int main(void) {
    uint8_t fb[84*6]={0};
    assert(ssd1306_init()==ESP_OK);
    packets=bytes=0;
    fb[84*2+10]=0x55;
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(packets==2 && bytes==1 && panel[3][32]==0x55);
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(packets==2);
    ssd1306_show_lcd(fb,84,48,6,0);
    assert(panel[3][32]==0);
    ssd1306_show_lcd(fb,84,48,6,1);
    for(int p=1;p<7;p++) for(int x=22;x<106;x++) assert(panel[p][x]==255);
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(panel[3][32]==0x55 && panel[1][22]==0);
    fb[0]=0xa5; fail_data=1;
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(panel[1][22]==0);
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(panel[1][22]==0xa5);
    fb[1]=0x3c; fail_command=1;
    ssd1306_show_lcd(fb,84,48,6,2);
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(panel[1][23]==0x3c);
    fail_data=1; ssd1306_clear();
    ssd1306_show_lcd(fb,84,48,6,2);
    assert(panel[1][22]==0xa5 && panel[1][23]==0x3c);
    ssd1306_show_lcd(fb,84,48,6,3);
    assert(panel[1][22]==0x5a && panel[1][23]==0xc3);
    const DspOps base={tone}, override={muted};
    const ModelProfile model={&base};
    Mad2 m={.model=&model,.div=13000};
    assert(speaker_init()==ESP_OK && level==0);
    now=1000; m.tone=1; speaker_update(&m); assert(level==1 && pwm_freq==900);
    int changes=freq_changes; speaker_update(&m); assert(freq_changes==changes);
    now=2000; m.tone=0; speaker_update(&m); assert(level==1);
    now=16001; speaker_update(&m); assert(level==0);
    now=20000; m.edges=1 | (13000<<8); speaker_update(&m); assert(level==1 && m.edges==0 && pwm_freq==1000);
    now=35001; speaker_update(&m); assert(level==0);
    m.buzz_on=1; speaker_update(&m); assert(level==1 && pwm_freq==1000);
    m.div=6500; speaker_update(&m); assert(pwm_freq==2000);
    speaker_silence(); assert(level==0);
    m.buzz_on=0; m.tone=1; m.dsp_override=&override;
    speaker_update(&m); assert(level==0);
    m.dsp_override=NULL; speaker_update(&m); assert(level==1);
    speaker_silence(); assert(level==0);
    m.tone=0; speaker_update(&m); assert(level==0);
    now=100000; speaker_request_test(); speaker_update(NULL); assert(level==1 && pwm_freq==660);
    now=220000; speaker_update(NULL); assert(level==0);
    now=260000; speaker_update(NULL); assert(level==1 && pwm_freq==880);
    now=380000; speaker_update(NULL); assert(level==0);
    now=420000; speaker_update(NULL); assert(level==1 && pwm_freq==1320);
    now=580000; speaker_update(NULL); assert(level==0);
    now=600000; tone_freq=1200; m.tone=1; speaker_update(&m); assert(pwm_freq==1200);
    tone_freq=1500; speaker_update(&m); assert(pwm_freq==1500);
    speaker_silence(); assert(level==0);
    fail_freq=1; tone_freq=1800; speaker_update(&m); assert(level==0);
    puts("PASS: display partial updates, modes, I2C retries; PWM pitch, pulses, test notes, DSP override, silence, errors");
    return 0;
}
'''


def main():
    os.environ["PATH"] = str(GCC.parent) + os.pathsep + os.environ["PATH"]
    with tempfile.TemporaryDirectory(prefix="dct3-outputs-") as directory:
        work = Path(directory)
        (work / "mock.h").write_text(STUBS)
        headers = ["esp_err.h", "esp_log.h", "esp_timer.h", "driver/gpio.h", "driver/ledc.h",
                   "driver/i2c_master.h", "freertos/FreeRTOS.h", "freertos/task.h",
                   "mad2/mad2.h", "mad2/emu_host.h"]
        for name in headers:
            header = work / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n')
        source = work / "verify.c"
        source.write_text(TEST)
        program = work / "verify.exe"
        subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-Wno-unused-variable", "-Wno-unused-parameter",
                        "-I", str(work), "-I", str(ROOT / "main"),
                        str(source), str(ROOT / "main/ssd1306.c"),
                        str(ROOT / "main/speaker.c"), "-o", str(program)], check=True)
        subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    main()
