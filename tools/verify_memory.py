"""Run memory semantics checks against the real core and vendored ARM CPU."""
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
#include "core/dct3_core.h"
#include "mad2/mad2_internal.h"
static unsigned stores, reads;
static unsigned events;
static void test_events(struct ARMCore *cpu) { events++; cpu->nextEvent=cpu->cycles+17; }
static void compare_execution(DCT3Core *a,DCT3Core *b,int n) {
    events=0;
    for(int i=0;i<n;i++) dct3_step(a);
    unsigned expected_events=events;
    events=0; dct3_run(b,n);
    assert(events==expected_events);
    assert(memcmp(a->cpu.gprs,b->cpu.gprs,sizeof a->cpu.gprs)==0);
    assert(a->cpu.cycles==b->cpu.cycles && a->cpu.nextEvent==b->cpu.nextEvent);
    assert(a->cpu.cpsr.packed==b->cpu.cpsr.packed);
    assert(a->cpu.executionMode==b->cpu.executionMode);
    assert(memcmp(a->cpu.prefetch,b->cpu.prefetch,sizeof a->cpu.prefetch)==0);
    assert(memcmp(a->ram,b->ram,DCT3_RAM_SIZE)==0);
}
static uint32_t io_read(void *ctx,uint32_t pc,uint32_t addr,int size,uint32_t value) {
    (void)ctx; (void)pc; (void)addr; (void)size; reads++; return value^0x11111111u;
}
static void io_write(void *ctx,uint32_t pc,uint32_t addr,int size,uint32_t value) {
    (void)ctx; (void)pc; (void)addr; (void)size; (void)value; stores++;
}
int main(void) {
    for(uint16_t div=0;div<256;div++) {
        for(uint64_t i=0;i<1000;i++) {
            uint64_t cyc=i*UINT64_C(1000000007)+i%3;
            assert(mad2_t0_raw(cyc,div)==(cyc*DCT3_T0_HZ)/(DCT3_ARM_HZ*((uint64_t)div+1)));
        }
    }
    DCT3Core *c=dct3_core_create(); assert(c);
    struct ARMCore *cpu=&c->cpu;
    int cycles=0;
    for(unsigned i=0;i<10000;i++) {
        uint32_t a=(i*401u)&DCT3_RAM_MASK, v=i*0x9e3779b9u;
        cpu->memory.store32(cpu,a,(int32_t)v,&cycles);
        uint32_t base=(a&DCT3_RAM_MASK)&~3u;
        assert(c->ram[base]==(uint8_t)(v>>24));
        assert(c->ram[base+1]==(uint8_t)(v>>16));
        assert(c->ram[base+2]==(uint8_t)(v>>8));
        assert(c->ram[base+3]==(uint8_t)v);
        assert(dct3_read32(c,base)==v);
        unsigned rot=(a&3u)*8;
        uint32_t expected=rot ? (v>>rot)|(v<<(32-rot)) : v;
        assert(cpu->memory.load32(cpu,a,&cycles)==expected);
        uint16_t h=(uint16_t)v;
        cpu->memory.store16(cpu,a,(int16_t)h,&cycles);
        base=(a&DCT3_RAM_MASK)&~1u;
        assert(c->ram[base]==(uint8_t)(h>>8) && c->ram[base+1]==(uint8_t)h);
        assert(cpu->memory.load16(cpu,a,&cycles)==h);
    }
    dct3_write32(c,DCT3_RAM_SIZE+100,0x12345678);
    assert(dct3_read32(c,100)==0x12345678);
    dct3_write16(c,DCT3_RAM_SIZE-1,0xabcdu);
    assert(c->ram[DCT3_RAM_SIZE-2]==0xab && c->ram[DCT3_RAM_SIZE-1]==0xcd);
    dct3_set_io_hooks(c,NULL,0x20000,0x20100,io_read,io_write);
    dct3_set_io_range2(c,0x200000,0x400000);
    dct3_write32(c,0x200000,0x12345678);
    cpu->memory.store32(cpu,0x200000,(int32_t)0xaabbccdd,&cycles);
    assert(stores==1 && dct3_read32(c,0x200000)==0x12345678);
    assert(cpu->memory.load32(cpu,0x200000,&cycles)==0x03254769 && reads==1);
    cpu->memory.store32(cpu,0x20000,(int32_t)0xaabbccdd,&cycles);
    assert(stores==2 && dct3_read32(c,0x20000)==0xaabbccdd);
    dct3_core_destroy(c);
    DCT3Core *a=dct3_core_create(), *b=dct3_core_create(); assert(a && b);
    uint8_t cache[16384]; uint32_t tags[64];
    dct3_set_code_cache(b,cache,tags,0x3d0000);
    // Thumb MOV/ADD/store/load/compare/conditional and unconditional branch.
    const uint16_t code[]={0x2000,0x2101,0x2207,0x3001,0x1840,0x6018,0x681c,0x2c0a,0xd1f9,0x46c0,0xe7f7};
    for(unsigned i=0;i<sizeof code/sizeof code[0];i++) {
        dct3_write16(a,0x200000+2*i,code[i]);
        dct3_write16(b,0x200000+2*i,code[i]);
    }
    dct3_core_force_pc_cpsr(a,0x200000,0x3f,0);
    dct3_core_force_pc_cpsr(b,0x200000,0x3f,0);
    a->cpu.gprs[3]=b->cpu.gprs[3]=0x100;
    a->cpu.irqh.processEvents=b->cpu.irqh.processEvents=test_events;
    a->cpu.nextEvent=b->cpu.nextEvent=0;
    compare_execution(a,b,0); compare_execution(a,b,1);
    compare_execution(a,b,8); compare_execution(a,b,32); compare_execution(a,b,4096);
    // ARM MOV/ADD/CMP/branch, then BX into Thumb within the same batch.
    const uint32_t arm[]={0xe3a00000,0xe2800001,0xe3500007,0x1afffffc,0xe12fff15};
    for(unsigned i=0;i<5;i++) {
        dct3_write32(a,0x200100+4*i,arm[i]); dct3_write32(b,0x200100+4*i,arm[i]);
    }
    dct3_core_force_pc_cpsr(a,0x200100,0x1f,0);
    dct3_core_force_pc_cpsr(b,0x200100,0x1f,0);
    a->cpu.gprs[5]=b->cpu.gprs[5]=0x200001;
    compare_execution(a,b,128);
    // Exact state and cycle equivalence for accelerated idle polling.
    const uint16_t idle[]={0x7830,0x2800,0xd001,0xe7fb,0x46c0,0x46c0};
    for(unsigned i=0;i<6;i++) {
        dct3_write16(a,0x200200+2*i,idle[i]); dct3_write16(b,0x200200+2*i,idle[i]);
    }
    dct3_core_force_pc_cpsr(a,0x200200,0x3f,0);
    dct3_core_force_pc_cpsr(b,0x200200,0x3f,0);
    a->cpu.gprs[6]=b->cpu.gprs[6]=0x200;
    a->ram[0x200]=b->ram[0x200]=7;
    a->cpu.nextEvent=b->cpu.nextEvent=a->cpu.cycles+5000;
    for(unsigned limit=24;limit<512;limit+=7) {
        a->cpu.nextEvent=b->cpu.nextEvent=a->cpu.cycles+5000;
        unsigned skipped=dct3_skip_byte_poll(b,limit);
        assert(skipped>=8);
        for(unsigned i=0;i<skipped;i++) dct3_step(a);
        compare_execution(a,b,0);
    }
    b->ram[0x200]=0; assert(dct3_skip_byte_poll(b,100)==0);
    b->ram[0x200]=7;
    b->cpu.gprs[6]=0x20000; assert(dct3_skip_byte_poll(b,100)==0);
    for(unsigned phase=0;phase<4;phase++) {
        dct3_core_force_pc_cpsr(a,0x200200,0x3f,0);
        dct3_core_force_pc_cpsr(b,0x200200,0x3f,0);
        a->cpu.gprs[6]=b->cpu.gprs[6]=0x200;
        a->ram[0x200]=b->ram[0x200]=7;
        a->cpu.nextEvent=b->cpu.nextEvent=a->cpu.cycles+5000;
        compare_execution(a,b,(int)phase);
        unsigned skipped=dct3_skip_byte_poll(b,192); assert(skipped>=8);
        for(unsigned i=0;i<skipped;i++) dct3_step(a);
        compare_execution(a,b,0);
    }
    dct3_core_destroy(a); dct3_core_destroy(b);
    puts("PASS: 10000 memory cases; SRAM code cache; batched ARM/Thumb state, cycles, events and mode switches");
    return 0;
}
'''


def main():
    os.environ["PATH"] = str(GCC.parent) + os.pathsep + os.environ["PATH"]
    core = ROOT / "components/dct3"
    with tempfile.TemporaryDirectory(prefix="dct3-memory-") as directory:
        work = Path(directory)
        source = work / "verify.c"
        source.write_text(TEST)
        for mode in [[], ["-DDCT3_ESP32", "-DDCT3_CODE_CACHE"]]:
            program = work / "verify.exe"
            subprocess.run([str(GCC), "-std=c11", "-O3",
                            "-DDCT3_RAM_SIZE=0x00400000u", *mode,
                            "-I", str(core), "-I", str(core / "mgba-arm/include"),
                            str(source), str(core / "core/dct3_core.c"),
                            str(core / "mgba-arm/src/arm/arm.c"),
                            str(core / "mgba-arm/src/arm/isa-arm.c"),
                            str(core / "mgba-arm/src/arm/isa-thumb.c"),
                            "-o", str(program)], check=True)
            subprocess.run([str(program)], check=True)


if __name__ == "__main__":
    main()
