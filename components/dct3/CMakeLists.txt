# DCT3 emulator core as an ESP-IDF component.
#
# Sources are a direct copy of github.com/djr-747/nokia-dct3-emulator
# (src/core, src/mad2, src/models, src/services, src/harness +
#  third_party/mgba-arm), adapted for ESP-IDF:
#   - the host serial/socket transports (DSP bridge, SIM bridge, WAP host)
#     are compiled out via ESP_PLATFORM;
#   - getenv() is redirected to dct3_getenv() (provided by the firmware) so the
#     upstream diagnostic knobs stay OFF and no environment has to exist;
#   - the model registry is compiled down to the 3310 (DCT3_MODEL_3310_ONLY),
#     which also removes the C54x co-sim dependency.
#
# ESP32S3 notes:
#  - The flat address space covers the 3310's 2 MiB flash + RAM + EEPROM.
#    Upstream uses 16 MiB; 4 MiB is enough for every 3310 access (flash ends at
#    0x400000) and leaves PSRAM for the rest of the firmware. The buffer is
#    allocated by main through dct3_core_set_allocator() in PSRAM.
#  - No POSIX sockets, termios or subprocesses are used.

idf_component_register(
    SRCS
        core/dct3_core.c
        core/dct3_debug_stub.c

        mad2/mad2.c
        mad2/mad2_bus.c
        mad2/mad2_ccont.c
        mad2/mad2_diag.c
        mad2/mad2_flash.c
        mad2/mad2_keypad.c
        mad2/mad2_lcd.c
        mad2/mad2_mbus.c
        mad2/mad2_sim.c
        mad2/mad2_timers.c
        mad2/cobba.c
        mad2/dsp_bridge.c          # host transport compiled out; link stubs remain
        mad2/emu_audio.c
        mad2/ext_eeprom.c
        mad2/ext_eeprom_blobs.c
        mad2/gsm_dl.c
        mad2/mdi_gsm.c

        mad2/dsp/dsp_rom4.c
        mad2/dsp/dsp_rom6.c
        mad2/dsp/dsp_mailbox.c
        mad2/dsp/dsp_tone.c
        mad2/dsp/mdi_queue.c
        mad2/dsp/mdi_trace.c
        mad2/dsp/ppp.c
        mad2/dsp/rlp.c
        mad2/dsp/wap.c
        mad2/dsp/wap_host.c        # ESP_PLATFORM: no-op stubs

        models/model.c
        models/mad2_sigs.c
        models/3310/profile.c      # DCT3_MODEL_3310_ONLY: only profile linked

        services/identity_provision.c
        services/eeprom_provision.c
        services/dct3_calcul.c

        harness/runloop.c
        harness/fault.c
        harness/seccode.c
        harness/difftrace.c
        harness/esp_stubs.c        # no-op heap_shadow/telemetry: ~430 KiB DRAM saved

    INCLUDE_DIRS
        "."
        "mgba-arm/include"

    # Hot per-instruction objects in IRAM (see linker.lf).
    LDFRAGMENTS "linker.lf"
)

# Vendored mGBA ARM7TDMI core (only the ARM interpreter is vendored).
file(GLOB MGBA_CORE_SRCS "${CMAKE_CURRENT_LIST_DIR}/mgba-arm/src/arm/*.c")
target_sources(${COMPONENT_LIB} PRIVATE ${MGBA_CORE_SRCS})

# DCT3_RAM_SIZE is PUBLIC: main.c memsets the core RAM and must see the same
# value as the component. 4 MiB covers the 3310's 0x000000..0x400000 window.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    ENABLE_DEBUGGERS
    DCT3_ESP32
    DCT3_MODEL_3310_ONLY
    DCT3_RAM_SIZE=0x00400000u
)

# The upstream core reads diagnostic switches with getenv(). On the device all
# of them are off; route every call to the firmware's stub.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    getenv=dct3_getenv
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -O3
    # Upstream diagnostic printf() calls assume uint32_t == unsigned int. On
    # Xtensa uint32_t is unsigned long, so every %u/%X is flagged even though
    # the width is identical. These are host-debug traces only (mostly compiled
    # out on the device); keep the warnings visible but not fatal.
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=maybe-uninitialized
)

if(CONFIG_DCT3_CODE_CACHE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC DCT3_CODE_CACHE)
endif()
