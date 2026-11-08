"""Exercise the actual flash snapshot store, including interrupted commits."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GCC = Path("C:/msys64/mingw64/bin/gcc.exe")
HEADERS = r'''
#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_CRC 4
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_ANY 255
typedef struct { size_t size; } esp_partition_t;
const esp_partition_t *esp_partition_find_first(int,int,const char*);
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
esp_err_t esp_partition_write(const esp_partition_t*,size_t,const void*,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t,size_t);
uint32_t esp_rom_crc32_le(uint32_t,const uint8_t*,uint32_t);
'''
TEST = r'''
#include "mock.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "settings_store.h"
static uint8_t flash[0x62000], a[0x30000], b[0x30000], loaded[0x30000];
static const esp_partition_t partition={sizeof flash};
static int missing,fail_write,write_count,fail_read,fail_commit;
const esp_partition_t *esp_partition_find_first(int t,int s,const char *label) {
    (void)t; (void)s; assert(strcmp(label,"settings")==0); return missing?NULL:&partition;
}
esp_err_t esp_partition_read(const esp_partition_t *p,size_t off,void *data,size_t n) {
    assert(p==&partition && off+n<=sizeof flash);
    if(fail_read) return 9;
    memcpy(data,flash+off,n); return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p,size_t off,size_t n) {
    assert(p==&partition && off+n<=sizeof flash && !(off%4096) && !(n%4096));
    memset(flash+off,255,n); return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p,size_t off,const void *data,size_t n) {
    assert(p==&partition && off+n<=sizeof flash);
    write_count++;
    const uint8_t *src=data;
    if(fail_commit && !(off%0x31000)) {
        for(size_t i=0;i<n/2;i++) flash[off+i]&=src[i];
        return 9;
    }
    if(write_count==fail_write) { for(size_t i=0;i<n/2;i++) flash[off+i]&=src[i]; return 9; }
    for(size_t i=0;i<n;i++) { assert((flash[off+i]&src[i])==src[i]); flash[off+i]=src[i]; }
    return ESP_OK;
}
uint32_t esp_rom_crc32_le(uint32_t crc,const uint8_t *data,uint32_t n) {
    crc=~crc;
    while(n--) { crc^=*data++; for(int i=0;i<8;i++) crc=(crc>>1)^((crc&1)?0xedb88320u:0); }
    return ~crc;
}
int main(void) {
    SettingsStore s,reboot;
    for(size_t i=0;i<sizeof a;i++) { a[i]=(uint8_t)i; b[i]=(uint8_t)(i*3+1); }
    memset(flash,255,sizeof flash);
    assert(settings_store_load(&s,42,loaded,sizeof loaded)==ESP_ERR_NOT_FOUND);
    assert(s.partition && s.active_slot==-1);
    assert(settings_store_save(&s,a)==ESP_OK && s.active_slot==0);
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_OK);
    assert(memcmp(loaded,a,sizeof a)==0);
    assert(settings_store_save(&s,b)==ESP_OK && s.active_slot==1);
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_OK);
    assert(memcmp(loaded,b,sizeof b)==0 && reboot.sequence==2);
    // Truncated data and truncated commit must leave the previous slot usable.
    fail_write=write_count+1;
    assert(settings_store_save(&s,a)!=ESP_OK);
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_OK);
    assert(memcmp(loaded,b,sizeof b)==0);
    fail_write=0; fail_commit=1;
    assert(settings_store_save(&s,a)!=ESP_OK);
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_OK);
    assert(memcmp(loaded,b,sizeof b)==0);
    fail_write=0; fail_commit=0;
    assert(settings_store_save(&s,a)==ESP_OK);
    // Corrupted latest data falls back to the previous complete generation.
    flash[0x1000+700]^=1;
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_OK);
    assert(memcmp(loaded,b,sizeof b)==0);
    memset(loaded,0x5a,sizeof loaded);
    assert(settings_store_load(&reboot,77,loaded,sizeof loaded)==ESP_ERR_NOT_FOUND);
    assert(loaded[0]==0x5a);
    missing=1;
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)==ESP_ERR_NOT_FOUND);
    assert(!reboot.partition);
    missing=0; fail_read=1;
    assert(settings_store_load(&reboot,42,loaded,sizeof loaded)!=ESP_OK);
    assert(!reboot.partition);
    puts("PASS: EEPROM restore, alternating slots, interrupted data/header writes, CRC fallback, firmware identity, missing partition, read errors");
}
'''


def main():
    os.environ["PATH"] = str(GCC.parent) + os.pathsep + os.environ["PATH"]
    with tempfile.TemporaryDirectory(prefix="dct3-settings-") as directory:
        work = Path(directory)
        (work / "mock.h").write_text(HEADERS)
        for name in ["esp_err.h", "esp_partition.h", "esp_rom_crc.h"]:
            (work / name).write_text('#include "mock.h"\n')
        (work / "verify.c").write_text(TEST)
        program = work / "verify.exe"
        subprocess.run([str(GCC), "-std=c11", "-O2", "-Wall", "-Wextra",
                        "-I", str(work), "-I", str(ROOT / "main"),
                        str(work / "verify.c"), str(ROOT / "main/settings_store.c"),
                        "-o", str(program)], check=True)
        subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    main()
