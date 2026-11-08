// ESP32 build: the allocator-shadow / telemetry instruments are host debugging
// aids and are permanently off on the device (dct3_getenv() returns NULL). Their
// static rings need ~430 KiB of internal DRAM, which does not fit next to the
// application. This file provides the same API as no-ops so the shared harness
// links unchanged; the implementations remain in heap_shadow.c / telemetry.c for
// host builds.

#include "harness/harness.h"

int heap_shadow_armed(void) { return 0; }
int heap_shadow_step(Mad2* m, struct ARMCore* cpu, uint32_t pc, int64_t step) {
    (void)m; (void)cpu; (void)pc; (void)step;
    return 0;
}
void heap_shadow_dump(Mad2* m) { (void)m; }
void heap_shadow_arm(int shadow, int guard) { (void)shadow; (void)guard; }
void heap_shadow_set_sample(long every) { (void)every; }
double heap_shadow_used_d(Mad2* m) { (void)m; return 0.0; }
double heap_shadow_live_d(void) { return 0.0; }
const char* heap_shadow_text(Mad2* m, unsigned* len) {
    (void)m;
    if (len) *len = 0;
    return "";
}

int telemetry_armed(void) { return 0; }
int telemetry_step(Mad2* m, struct ARMCore* cpu, uint32_t pc, int64_t step) {
    (void)m; (void)cpu; (void)pc; (void)step;
    return 0;
}
void telemetry_post_branch(Mad2* m, struct ARMCore* cpu, int64_t step) {
    (void)m; (void)cpu; (void)step;
}
void telemetry_event_irq(Mad2* m, int irq_n, int is_fiq, int64_t step) {
    (void)m; (void)irq_n; (void)is_fiq; (void)step;
}
int telemetry_backtrace(uint32_t* out, int max) { (void)out; (void)max; return 0; }
void telemetry_dump(Mad2* m) { (void)m; }
void telemetry_arm_branchring(int on) { (void)on; }
void telemetry_arm_invariant(int on) { (void)on; }
void telemetry_arm_eventlog(int on) { (void)on; }
void telemetry_set_lastwriter(uint32_t addr, uint32_t len) { (void)addr; (void)len; }
double telemetry_branch_count(void) { return 0.0; }
