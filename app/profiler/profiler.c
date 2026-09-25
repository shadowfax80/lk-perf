/*
 * Bare-metal statistical sampling profiler for LK, AArch32.
 *
 * Stage 1: PC-only histogram. A periodic timer IRQ samples the interrupted
 * PC into a ring buffer; the host reads the buffer out over QMP and
 * symbolizes it into a flat self-time histogram. See docs/DESIGN.md for
 * why this is a flat histogram, not a flame graph -- a bare PC carries no
 * calling-context information, so stages 2/3 (PC+LR, then frame-pointer
 * offline unwind) are what get you an actual call stack.
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
// project work found bolt_bench's counters.
uint32_t profiler_pc_buf[PROFILER_BUF_SIZE];
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

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: profiler <start|stop|status|clear|bench>\n");
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
