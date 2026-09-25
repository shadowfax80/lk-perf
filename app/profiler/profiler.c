/*
 * Bare-metal statistical sampling profiler for LK, AArch32.
 *
 * Stage 3: frame-pointer offline unwind. Adds an FP capture (r7 in Thumb
 * state, r11 in ARM state, picked per-sample from the interrupted SPSR's
 * T-bit) to Stage 2's PC+LR. The FP register itself is NOT saved by the
 * generic IRQ entry path (arch/arm/arm/exceptions.S's `save` macro only
 * touches r0-r3/r12/lr/sp) so overlay/lk/0002-capture-interrupted-fp.patch
 * stashes r7/r11 into two always-present core-LK globals right at IRQ
 * entry, before any C code can repurpose them. The host walks the FP
 * chain offline from a QMP memory dump: this build is compiled with
 * -fno-omit-frame-pointer (project/profiler.mk), and empirically (via
 * objdump, not assumed) GCC's Thumb prologue for that flag is
 * `strd r7,lr,[sp,#-8]!` / `add r7,sp,#0` -- so at any live fp,
 * [fp+0]=caller's saved fp and [fp+4]=the *stack-saved* return address,
 * which is reliable where Stage 2's live-register LR read was not (see
 * that stage's own comment: GCC happily reuses the live lr register as
 * scratch mid-function; it can't touch the copy it already pushed).
 *
 * The sample source is dev/interrupt/arm_gic/gic_v2.c's profiler_on_tick()
 * hook (overlay/lk/0001-add-profiler-tick-hook.patch) -- the only place the
 * interrupted register state (struct arm_iframe) is still in scope, since
 * register_int_handler()'s callback signature is (void *arg) only. This
 * file does NOT register its own timer; it piggybacks on whichever IRQ
 * fires the hook (LK's own scheduler tick, IRQ 27 on qemu-virt-arm/
 * cortex-a15, confirmed from this project family's own boot logs), so it
 * adds no timer-programming code and cannot double-program hardware timer
 * registers already owned by kernel/timer.c.
 */

#include <arch/arm.h>
#include <lib/console.h>
#include <lk/compiler.h>
#include <lk/reg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// IRQ 27 = non-secure physical timer PPI on qemu-virt-arm/cortex-a15 --
// this is LK's own scheduler-tick IRQ (kernel/timer.c, dev/timer/
// arm_generic/arm_generic_timer.c), confirmed directly from this project
// family's boot logs across every prior LK/QEMU session. Piggybacking on
// it means Stage 1's sampling rate is whatever kernel/timer.c configures
// (10ms / 100Hz at the time of writing) -- fine for proving the pipeline;
// an independent, configurable-rate timer is a later refinement, not a
// Stage 1 requirement.
#define PROFILER_TIMER_IRQ 27

#define PROFILER_BUF_SIZE 4096

// SPSR T-bit (Thumb state) -- CPSR/SPSR bit 5, per the ARM architecture
// reference manual. Picks which of profiler_fp_r7/profiler_fp_r11 was the
// interrupted code's actual frame-pointer register.
#define PROFILER_SPSR_T_BIT (1u << 5)

// Defined in arch/arm/arm/exceptions.S by
// overlay/lk/0002-capture-interrupted-fp.patch -- always present in core
// LK (not app/profiler-owned), so linking without app/profiler still
// works; these just sit unread.
extern uint32_t profiler_fp_r7;
extern uint32_t profiler_fp_r11;

// Plain external linkage, no custom linker section: the host finds these
// by ordinary ELF symbol lookup (arm-none-eabi-nm), the same way earlier
// project work found bolt_bench's counters. Parallel arrays (not a struct
// array) so the host can nm-locate and size each column independently
// without computing struct-layout/padding offsets itself.
uint32_t profiler_pc_buf[PROFILER_BUF_SIZE];
uint32_t profiler_lr_buf[PROFILER_BUF_SIZE];
uint32_t profiler_fp_buf[PROFILER_BUF_SIZE];
volatile uint32_t profiler_head;
volatile uint32_t profiler_total;
volatile uint32_t profiler_enabled;

// Called from dev/interrupt/arm_gic/gic_v2.c on every IRQ (weak default is
// a no-op when this file isn't linked in). Keep this minimal and
// allocation-free -- it runs in interrupt context on every timer tick,
// the same atomicity discipline as this project family's earlier
// counter-bump-stub lessons.
void profiler_on_tick(struct arm_iframe *frame, unsigned int vector) {
    if (vector != PROFILER_TIMER_IRQ || !profiler_enabled) {
        return;
    }

    bool thumb = (frame->spsr & PROFILER_SPSR_T_BIT) != 0;

    uint32_t idx = profiler_head;
    profiler_pc_buf[idx] = frame->pc;
    profiler_lr_buf[idx] = frame->lr;
    profiler_fp_buf[idx] = thumb ? profiler_fp_r7 : profiler_fp_r11;
    profiler_head = (idx + 1) % PROFILER_BUF_SIZE;
    profiler_total++;
}

// Minimal, self-contained synthetic workload purely to give Stage 1
// something with real, distinguishable hot PCs to sample -- proves the
// pipeline captures genuine variation, not a stuck/constant PC. `volatile`
// accumulators and NO_INLINE stop the compiler from folding these away.
static volatile uint32_t profiler_sink;

__NO_INLINE static void profiler_workload_a(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += i * 2654435761u;  // Knuth multiplicative hash, arbitrary busy work
    }
    profiler_sink = acc;
}

__NO_INLINE static void profiler_workload_b(uint32_t iters) {
    uint32_t acc = 1;
    for (uint32_t i = 0; i < iters; i++) {
        acc = (acc << 1) ^ (acc >> 31) ^ i;
    }
    profiler_sink = acc;
}

// Three-level call chain purely to give Stage 3's offline FP-chain
// unwinder something with real depth to reconstruct -- workload_a/b above
// are both leaves, useless for testing call-stack recovery.
__NO_INLINE static void profiler_workload_inner(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += (i ^ 0x9e3779b9u) * 2246822519u;  // xxhash-ish mix, arbitrary busy work
    }
    profiler_sink = acc;
}

__NO_INLINE static void profiler_workload_mid(uint32_t iters) {
    profiler_workload_inner(iters);
}

__NO_INLINE static void profiler_workload_outer(uint32_t iters) {
    profiler_workload_mid(iters);
}

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: profiler <start|stop|status|clear|bench|nest|fpcheck>\n");
        return -1;
    }

    const char *sub = argv[1].str;

    if (!strcmp(sub, "start")) {
        profiler_enabled = 1;
        printf("profiler: sampling started (irq %d, buffer %d entries)\n",
               PROFILER_TIMER_IRQ, PROFILER_BUF_SIZE);
    } else if (!strcmp(sub, "stop")) {
        profiler_enabled = 0;
        printf("profiler: sampling stopped (%u samples total, %u since last clear)\n",
               profiler_total, profiler_total);
    } else if (!strcmp(sub, "clear")) {
        profiler_head = 0;
        profiler_total = 0;
        memset(profiler_pc_buf, 0, sizeof(profiler_pc_buf));
        memset(profiler_lr_buf, 0, sizeof(profiler_lr_buf));
        memset(profiler_fp_buf, 0, sizeof(profiler_fp_buf));
        printf("profiler: buffer cleared\n");
    } else if (!strcmp(sub, "bench")) {
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 20000000;
        printf("profiler: running synthetic workload (%u iters/function) ...\n", iters);
        for (int rep = 0; rep < 4; rep++) {
            profiler_workload_a(iters);
            profiler_workload_b(iters);
        }
        printf("profiler: bench done (sink=%u, ignore -- just prevents dead-code elim)\n",
               profiler_sink);
    } else if (!strcmp(sub, "nest")) {
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 20000000;
        printf("profiler: running nested-call workload (%u iters) ...\n", iters);
        profiler_workload_outer(iters);
        printf("profiler: nest done (sink=%u, ignore -- just prevents dead-code elim)\n",
               profiler_sink);
    } else if (!strcmp(sub, "fpcheck")) {
        // Diagnostic: dereference the ring buffer's OWN last-recorded fp
        // directly on target, no QMP involved. Deliberately NOT
        // profiler_fp_r7/r11 directly -- those are continuously
        // overwritten on every tick regardless of profiler_enabled, so by
        // the time a shell command reads them they reflect whatever last
        // interrupted the *idle* shell loop, not the sample actually
        // stored in profiler_fp_buf[] while the workload ran.
        if (profiler_total == 0) {
            printf("fpcheck: no samples yet\n");
        } else {
            uint32_t idx = (profiler_head + PROFILER_BUF_SIZE - 1) % PROFILER_BUF_SIZE;
            uint32_t pc = profiler_pc_buf[idx];
            uint32_t fp = profiler_fp_buf[idx];
            printf("fpcheck: last sample idx=%u pc=0x%08x fp=0x%08x\n", idx, pc, fp);
            if (fp >= 0x1000) {
                volatile uint32_t *p = (volatile uint32_t *)fp;
                printf("*(fp+0)=0x%08x *(fp+4)=0x%08x\n", p[0], p[1]);
            }
        }
    } else if (!strcmp(sub, "status")) {
        printf("profiler: enabled=%u total_samples=%u head=%u capacity=%d%s\n",
               profiler_enabled, profiler_total, profiler_head, PROFILER_BUF_SIZE,
               profiler_total >= PROFILER_BUF_SIZE ? " (WRAPPED)" : "");
    } else {
        printf("unknown subcommand '%s'\n", sub);
        return -1;
    }
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("profiler", "bare-metal sampling profiler (start|stop|status|clear)", &cmd_profiler)
STATIC_COMMAND_END(profiler);
