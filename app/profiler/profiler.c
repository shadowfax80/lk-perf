/*
 * Bare-metal statistical sampling profiler for LK, AArch32.
 *
 * Stage 2: PC+LR histogram. A periodic timer IRQ samples the interrupted
 * PC and LR into parallel ring buffers; the host symbolizes both and
 * reports self time plus a best-effort immediate-caller breakdown. LR is
 * free to capture (already in the exception frame) but is not ground
 * truth -- AAPCS lets a function reuse LR as scratch once it has made its
 * own calls, so treat this as a cheap, imperfect upgrade over Stage 1's
 * flat histogram, not a real call stack. Empirically confirmed, not just
 * theoretical: profiler_workload_inner (a leaf, no calls at all) gets
 * compiled at -O2 as `str.w lr,[sp,#-4]!` / `movw/movt lr,#0x9e3779b9` --
 * GCC spills the real return address to the stack and reuses the lr
 * *register* as a scratch temp for the loop body's whole duration purely
 * because the loop needs two live 32-bit immediates and register
 * pressure is high. Sampling mid-loop reads that immediate, not a
 * caller. Stage 3 (frame-pointer offline unwind) is what gets an actual
 * call chain.
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

// Plain external linkage, no custom linker section: the host finds these
// by ordinary ELF symbol lookup (arm-none-eabi-nm), the same way earlier
// project work found bolt_bench's counters. Parallel arrays (not a struct
// array) so the host can nm-locate and size each column independently
// without computing struct-layout/padding offsets itself.
uint32_t profiler_pc_buf[PROFILER_BUF_SIZE];
uint32_t profiler_lr_buf[PROFILER_BUF_SIZE];
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

    uint32_t idx = profiler_head;
    profiler_pc_buf[idx] = frame->pc;
    profiler_lr_buf[idx] = frame->lr;
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
        printf("usage: profiler <start|stop|status|clear|bench|nest>\n");
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
