/*
 * Bare-metal statistical sampling profiler for LK, AArch32.
 *
 * Stage 5 note: PMU-event-triggered sampling (see docs/DESIGN.md) is
 * confirmed real-hardware-only on this target, not just anticipated --
 * see the "pmu" branch of cmd_profiler below for the two independently
 * verified reasons (no PMU IRQ route in this QEMU target's device tree;
 * PMU coprocessor access itself faults without a secure-monitor boot
 * stage). Stages 1-4 below are unaffected and remain the working
 * pipeline.
 *
 * Stage 4: SMP. Everything from stages 1-3 (PC, LR, FP-chain unwind) now
 * runs correctly with multiple cores concurrently sampling: per-CPU ring
 * buffers (no locking in the ISR -- a lock inside a sampling interrupt
 * is exactly the kind of perturbation that would corrupt the
 * measurement), and per-sample CNTPCT-derived timestamps for cross-core
 * ordering. Confirmed necessary, not just anticipated: the GIC tick IRQ
 * is a PPI (banked per-core), so with -smp N every core independently
 * enters profiler_on_tick(), and profiler_fp_r7/r11 (Stage 3's capture
 * globals) would otherwise race between cores -- fixed by making the
 * exceptions.S capture itself per-CPU (see
 * overlay/lk/0003-percpu-fp-capture.patch), indexed by the *exact* same
 * MPIDR-masking instruction arch_curr_cpu_num() compiles to
 * (`bic r3, r3, #0xff000000`, read from this build's own
 * lk.elf.debug.lst, not re-derived from the architecture manual by
 * hand) so the assembly-computed index can never disagree with the
 * C-computed one used for the other per-CPU arrays below.
 *
 * Timestamp uses current_time_hires() (CNTPCT-backed, microsecond
 * lk_bigtime_t), not PMCCNTR: PMCCNTR is a per-core PMU cycle counter,
 * free-running independently since each core's own reset, with no
 * architectural guarantee of cross-core phase alignment -- using it to
 * interleave samples from different cores would silently produce a
 * wrong merged ordering. CNTPCT is architected as a single, SoC-wide
 * coherent time base, visible identically to every core.
 *
 * Stage 3's FP-chain layout ([fp+0]=caller's saved fp, [fp+4]=saved
 * return address) and its .text-range plausibility gate (see
 * scripts/pc_histogram.py) are unchanged and per-core-independent: one
 * shared binary means identical frame-pointer conventions on every core,
 * and each thread's stack is separate memory regardless of which core
 * runs it.
 *
 * The FP register itself is NOT saved by the generic IRQ entry path
 * (arch/arm/arm/exceptions.S's `save` macro only touches r0-r3/r12/lr/sp)
 * so overlay/lk/0002-capture-interrupted-fp.patch (now per-CPU per
 * 0003) stashes r7/r11 into always-present core-LK globals right at IRQ
 * entry, before any C code can repurpose them.
 *
 * The sample source is dev/interrupt/arm_gic/gic_v2.c's profiler_on_tick()
 * hook (overlay/lk/0001-add-profiler-tick-hook.patch) -- the only place the
 * interrupted register state (struct arm_iframe) is still in scope, since
 * register_int_handler()'s callback signature is (void *arg) only. This
 * file does NOT register its own timer; it piggybacks on whichever IRQ
 * fires the hook (LK's own scheduler tick, IRQ 27 on qemu-virt-arm/
 * cortex-a15, confirmed a genuine per-core PPI from this build's own SMP
 * boot log: "Generic timer register irq 27 on cpu 0" repeated per core),
 * so it adds no timer-programming code and cannot double-program
 * hardware timer registers already owned by kernel/timer.c.
 */

#include <arch/arch_ops.h>
#include <arch/arm.h>
#include <kernel/thread.h>
#include <lib/console.h>
#include <lk/compiler.h>
#include <lk/reg.h>
#include <platform/time.h>
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
// overlay/lk/0002-capture-interrupted-fp.patch +
// overlay/lk/0003-percpu-fp-capture.patch (per-CPU as of Stage 4) --
// always present in core LK (not app/profiler-owned), so linking without
// app/profiler still works; these just sit unread.
extern uint32_t profiler_fp_r7[SMP_MAX_CPUS];
extern uint32_t profiler_fp_r11[SMP_MAX_CPUS];

// Plain external linkage, no custom linker section: the host finds these
// by ordinary ELF symbol lookup (arm-none-eabi-nm), the same way earlier
// project work found bolt_bench's counters. Parallel arrays (not a struct
// array) so the host can nm-locate and size each column independently
// without computing struct-layout/padding offsets itself. Per-CPU as of
// Stage 4: [cpu][index], one independent ring per core, no cross-core
// locking (an ISR is exactly the wrong place to contend a lock).
uint32_t profiler_pc_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_lr_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_fp_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint64_t profiler_ts_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
volatile uint32_t profiler_head[SMP_MAX_CPUS];
volatile uint32_t profiler_total[SMP_MAX_CPUS];
volatile uint32_t profiler_enabled;

// Called from dev/interrupt/arm_gic/gic_v2.c on every IRQ (weak default is
// a no-op when this file isn't linked in). Keep this minimal and
// allocation-free -- it runs in interrupt context on every timer tick,
// the same atomicity discipline as this project family's earlier
// counter-bump-stub lessons. Runs concurrently on every core (PPI, no
// cross-core serialization) -- touches only this core's own array slots,
// so no locking is needed despite running on N cores at once.
void profiler_on_tick(struct arm_iframe *frame, unsigned int vector) {
    if (vector != PROFILER_TIMER_IRQ || !profiler_enabled) {
        return;
    }

    uint cpu = arch_curr_cpu_num();
    bool thumb = (frame->spsr & PROFILER_SPSR_T_BIT) != 0;

    uint32_t idx = profiler_head[cpu];
    profiler_pc_buf[cpu][idx] = frame->pc;
    profiler_lr_buf[cpu][idx] = frame->lr;
    profiler_fp_buf[cpu][idx] = thumb ? profiler_fp_r7[cpu] : profiler_fp_r11[cpu];
    profiler_ts_buf[cpu][idx] = current_time_hires();
    profiler_head[cpu] = (idx + 1) % PROFILER_BUF_SIZE;
    profiler_total[cpu]++;
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

// Stage 4: run the same nested workload concurrently on SMP_MAX_CPUS
// worker threads so the SMP scheduler has a reason to actually spread
// work across every core -- proves per-CPU sampling captures activity
// on more than just whichever core happens to run the shell thread.
static int profiler_smp_worker(void *arg) {
    uint32_t iters = (uint32_t)(uintptr_t)arg;
    profiler_workload_outer(iters);
    return 0;
}

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: profiler <start|stop|status|clear|bench|nest|smp|pmu|fpcheck>\n");
        return -1;
    }

    const char *sub = argv[1].str;

    if (!strcmp(sub, "start")) {
        profiler_enabled = 1;
        printf("profiler: sampling started (irq %d, %d cores, buffer %d entries/core)\n",
               PROFILER_TIMER_IRQ, SMP_MAX_CPUS, PROFILER_BUF_SIZE);
    } else if (!strcmp(sub, "stop")) {
        profiler_enabled = 0;
        uint32_t total = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) total += profiler_total[c];
        printf("profiler: sampling stopped (%u samples total across %d cores)\n",
               total, SMP_MAX_CPUS);
    } else if (!strcmp(sub, "clear")) {
        memset(profiler_head, 0, sizeof(profiler_head));
        memset(profiler_total, 0, sizeof(profiler_total));
        memset(profiler_pc_buf, 0, sizeof(profiler_pc_buf));
        memset(profiler_lr_buf, 0, sizeof(profiler_lr_buf));
        memset(profiler_fp_buf, 0, sizeof(profiler_fp_buf));
        memset(profiler_ts_buf, 0, sizeof(profiler_ts_buf));
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
    } else if (!strcmp(sub, "smp")) {
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 20000000;
        printf("profiler: running nested workload on %d worker threads ...\n", SMP_MAX_CPUS);
        thread_t *workers[SMP_MAX_CPUS];
        for (int i = 0; i < SMP_MAX_CPUS; i++) {
            workers[i] = thread_create("profiler_smp_worker", profiler_smp_worker,
                                        (void *)(uintptr_t)iters, DEFAULT_PRIORITY, 4096);
            thread_resume(workers[i]);
        }
        for (int i = 0; i < SMP_MAX_CPUS; i++) {
            thread_join(workers[i], NULL, INFINITE_TIME);
        }
        printf("profiler: smp done (sink=%u, ignore -- just prevents dead-code elim)\n",
               profiler_sink);
    } else if (!strcmp(sub, "pmu")) {
        // Stage 5 (PMU-overflow-triggered sampling) is real-hardware-only
        // on this project -- confirmed two independent ways, not assumed:
        //
        // 1. No PMU interrupt route exists to arm at all. Dumping this
        //    exact QEMU invocation's own generated device tree
        //    (`qemu-system-arm -machine virt -cpu cortex-a15 -machine
        //    dumpdtb=...`) shows the `pmu {};` node present but empty --
        //    no `compatible`, no `interrupts` property.
        // 2. PMU coprocessor register access itself is unsafe here, not
        //    just the interrupt path: even the single already-public,
        //    already-proven LK accessor arch_cycle_count()
        //    (arch/arm/include/arch/arch_ops.h, `mrc p15,0,%0,c9,c13,0`
        //    = PMCCNTR, used throughout bolt-aarch32's bolt_bench)
        //    reliably faults with "undefined abort" the moment it
        //    executes on this bare-metal image -- verified directly,
        //    not inferred. Real hardware/firmware normally clears the
        //    NSACR PMU-access trap during secure-world boot before
        //    handing off to the kernel; this minimal image has no
        //    secure-monitor stage to do that. An earlier, more ambitious
        //    version of this command (PMCR/PMCEID/PMSELR/PMXEVTYPER
        //    register probing) crashed the same way and was removed
        //    rather than left in a state that panics the target.
        printf("pmu: Stage 5 needs real hardware -- see this command's own\n");
        printf("pmu: source comment for the two independent, verified reasons\n");
        printf("pmu: (no PMU IRQ route in this QEMU target's device tree, and\n");
        printf("pmu: PMU coprocessor access itself faults without a secure-\n");
        printf("pmu: monitor boot stage to clear the NSACR trap).\n");
    } else if (!strcmp(sub, "fpcheck")) {
        // Diagnostic: dereference each core's ring buffer's OWN
        // last-recorded fp directly on target, no QMP involved.
        // Deliberately NOT profiler_fp_r7/r11 directly -- those are
        // continuously overwritten on every tick regardless of
        // profiler_enabled, so by the time a shell command reads them
        // they reflect whatever last interrupted the *idle* shell loop
        // on *that* core, not the sample actually stored in
        // profiler_fp_buf[] while the workload ran.
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            if (profiler_total[c] == 0) {
                printf("fpcheck: cpu%d no samples yet\n", c);
                continue;
            }
            uint32_t idx = (profiler_head[c] + PROFILER_BUF_SIZE - 1) % PROFILER_BUF_SIZE;
            uint32_t pc = profiler_pc_buf[c][idx];
            uint32_t fp = profiler_fp_buf[c][idx];
            printf("fpcheck: cpu%d last sample idx=%u pc=0x%08x fp=0x%08x\n", c, idx, pc, fp);
            if (fp >= 0x1000) {
                volatile uint32_t *p = (volatile uint32_t *)fp;
                printf("cpu%d *(fp+0)=0x%08x *(fp+4)=0x%08x\n", c, p[0], p[1]);
            }
        }
    } else if (!strcmp(sub, "status")) {
        uint32_t total = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            total += profiler_total[c];
            printf("profiler: cpu%d total_samples=%u head=%u%s\n", c, profiler_total[c],
                   profiler_head[c], profiler_total[c] >= PROFILER_BUF_SIZE ? " (WRAPPED)" : "");
        }
        printf("profiler: enabled=%u total_samples(all cpus)=%u capacity=%d/core\n",
               profiler_enabled, total, PROFILER_BUF_SIZE);
    } else {
        printf("unknown subcommand '%s'\n", sub);
        return -1;
    }
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("profiler", "bare-metal sampling profiler (start|stop|status|clear)", &cmd_profiler)
STATIC_COMMAND_END(profiler);
