"""Test the real MAD2 timer body and next-event selection in isolation."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
GCC = Path("C:/msys64/mingw64/bin/gcc.exe")
TEST = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "mad2/mad2_internal.h"
#include "mad2/dsp/mdi_trace.h"
char* dct3_getenv(const char *name) { (void)name; return NULL; }
void cc_int_update(Mad2 *m) { (void)m; }
uint64_t sim_next_wake(Mad2 *m) { (void)m; return UINT64_MAX; }
void sim_advance_to(Mad2 *m,uint64_t cycles) { (void)m; (void)cycles; }
void identity_provision_tick(Mad2 *m) { (void)m; }
void mdi_trace_poll(const Mad2 *m,MdiTrace *t,const char *tag,MdiPeekFn fn,void *ctx) {
    (void)m; (void)t; (void)tag; (void)fn; (void)ctx;
}
uint8_t mdi_trace_peek_mem(const Mad2 *m,void *ctx,uint32_t addr) { (void)m; (void)ctx; (void)addr; return 0; }
int main(void) {
    static Mad2 m;
    ModelProfile model={0};
    m.model=&model; m.t1_hz=33055; m.t0_div=255;
    m.rtc_mono=210000000; m.rtc_last_cyc=210000000;
    m.rtc_min_next=780000000; m.rtc_sec_next=208000000;
    m.t1_wrap_next=300000000; m.fiq8_ctrl=4; m.fiq8_next_cyc=200000000;
    m.wdt_reg=0; m.wdt_deadline_cyc=199000000;
    mad2_timers_tick(&m,210000000);
    assert(m.rtc_sec_next==221000000);
    assert(m.cc_int_lines & 0x10);
    uint64_t next=mad2_next_event_cycle(&m);
    assert(next>m.rtc_mono && next==221000000);
    m.rtc_mono=m.rtc_last_cyc=221000000;
    mad2_timers_tick(&m,221000000);
    assert(m.rtc_sec_next==234000000);
    assert(mad2_next_event_cycle(&m)>m.rtc_mono);
    m.fiq8_ctrl=1; m.fiq8_next_cyc=222000000;
    assert(mad2_next_event_cycle(&m)==222000000);
    m.fiq8_ctrl=4; m.wdt_reg=m.wdt_cc=49; m.wdt_deadline_cyc=223000000;
    assert(mad2_next_event_cycle(&m)==223000000);
    m.wdt_disabled=1; assert(mad2_next_event_cycle(&m)==234000000);
    puts("PASS: RTC-SEC deadline advances on fast path; masked FIQ8 and disabled watchdog do not block idle");
}
'''


def main():
    os.environ["PATH"] = str(GCC.parent) + os.pathsep + os.environ["PATH"]
    core = ROOT / "components/dct3"
    with tempfile.TemporaryDirectory(prefix="dct3-timers-") as directory:
        work = Path(directory)
        (work / "verify.c").write_text(TEST)
        program = work / "verify.exe"
        subprocess.run([str(GCC), "-std=c11", "-O2", "-Dgetenv=dct3_getenv",
                        "-I", str(core), "-I", str(core / "mgba-arm/include"),
                        str(work / "verify.c"), str(core / "mad2/mad2_timers.c"),
                        "-o", str(program)], check=True)
        subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    main()
