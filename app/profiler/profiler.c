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
 *
 * M5 (real Pi 4B hardware): three more fields per sample -- SPSR (full
 * register, not just the Thumb bit already extracted below: the host
 * needs it to know the interrupted mode, and to correctly mask the ARM
 * interworking ISA bit the way scripts/dwarf_unwind.py's
 * strip_isa_bit() already does for other addresses), the interrupted
 * thread's own SP, and its thread_t* as a de-facto thread ID (LK has
 * no small numeric TID, just a pointer and a name[32]).
 *
 * The interrupted SP needs no new assembly capture, unlike FP: LK's
 * standard IRQ entry (arch/arm/arm/exceptions.S's `save` macro)
 * captures `frame->usp`/`ulr` via `stmia sp,{r13,r14}^`, but that
 * instruction only captures the USR-mode banked SP/LR -- this
 * project's LK threads never switch to USR/SYS mode at all
 * (arch/arm/arm/thread.c's arch_context_switch does a raw cooperative
 * stack-pointer swap, no CPSR mode field anywhere), so usp/ulr are
 * dead registers here, not the real interrupted SP. The real one is
 * exactly `frame + 1`: one past the end of the `struct arm_iframe`
 * that `save` already pushes, confirmed by hand-tracing the exact
 * push/align sequence in exceptions.S (see docs/RPI4_BRINGUP.md).
 *
 * Each sample also captures PROFILER_STACK_CAPTURE_BYTES of raw stack
 * memory starting at that same SP -- offline DWARF-CFI unwinding needs
 * the actual stack bytes at sample time to walk past the first frame
 * (dwarf_unwind.py's read_memory callback), and a live target can't
 * preserve them until the host reads them later, so they're captured
 * into the ring buffer right alongside the registers. 128 bytes is
 * enough for several frames of this project's own small ARM32 call
 * chains, not whole thread stacks -- see PROFILER_STACK_CAPTURE_BYTES's
 * own comment for the sizing rationale. `scripts/pi4_pc_histogram.py`
 * feeds this to DwarfCFIUnwinder for a real multi-frame view instead
 * of the single-frame (leaf-function) one Stage 1 of the original
 * QEMU-era design was limited to.
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
//
// M5: BCM2711 (Pi 4B) uses a different vector for the exact same timer
// -- GIC ID 30, not 27 -- per overlay/lk/0004-bcm28xx-add-rpi4.patch's
// own redefinition of INTERRUPT_ARM_LOCAL_CNTPNSIRQ (which
// platform_early_init() actually registers the tick against). Without
// this, profiler_on_tick()'s vector check would never match on real
// hardware and the profiler would silently capture zero samples --
// found by checking the actual registered vector before the first
// real hardware test, not by a failed test.
#if BCM2711
#define PROFILER_TIMER_IRQ 30
#else
#define PROFILER_TIMER_IRQ 27
#endif

#define PROFILER_BUF_SIZE 4096

// M5: bytes of stack memory captured per sample, starting at the
// interrupted SP (see the file-level comment's `frame + 1` derivation).
// DWARF-CFI unwinding needs to read a handful of words per frame (the
// callee-saved registers a frame actually spilled) above the CFA --
// 128 bytes covers several frames of typical small ARM32 call chains
// (this project's own workload_outer->mid->inner is 3 levels deep with
// small frames) without capturing whole thread stacks. Per-CPU,
// per-sample: 4 * PROFILER_BUF_SIZE * 128 = 2MB total, comfortably
// inside this board's real (DTB-derived, M4) memory, and a full-buffer
// dump at the calibrated 3,000,000 baud stays well under a minute.
#define PROFILER_STACK_CAPTURE_BYTES 128

// SPSR T-bit (Thumb state) -- CPSR/SPSR bit 5, per the ARM architecture
// reference manual. Picks which of profiler_fp_r7/profiler_fp_r11 was the
// interrupted code's actual frame-pointer register.
#define PROFILER_SPSR_T_BIT (1u << 5)

// M5 `profiler stat`: raw ARMv7 PMU (Performance Monitors) coprocessor
// accessors. Stage 5's own "pmu" command above already confirmed this
// coprocessor faults on the QEMU target (no secure-monitor boot stage
// to clear the NSACR trap); real hardware/firmware normally does clear
// it, but that's untested here until `stat` actually runs -- these are
// deliberately separate, minimal functions (not a struct/abstraction)
// so a UART-probe bisection (this project's own established debugging
// discipline, see docs/RPI4_BRINGUP.md's M1-M4 history) can pin down
// exactly which register access is the first to fault, if any.
// PMCCNTR's own accessor already exists and is proven safe on real ARM
// hardware elsewhere (arch_cycle_count(), arch/arm/include/arch/arch_ops.h,
// used throughout bolt-aarch32's bolt_bench) -- reimplemented here
// rather than depending on that header, to keep every PMU register
// this command touches in one place.
static inline uint32_t pmu_read_pmceid0(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 6" : "=r"(v)); return v;
}
static inline uint32_t pmu_read_pmceid1(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 7" : "=r"(v)); return v;
}
static inline uint32_t pmu_read_pmcr(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 0" : "=r"(v)); return v;
}
static inline void pmu_write_pmcr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 0" :: "r"(v));
}
static inline void pmu_write_pmcntenset(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 1" :: "r"(v));
}
static inline void pmu_write_pmselr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 5" :: "r"(v));
}
static inline uint32_t pmu_read_pmccntr(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c13, 0" : "=r"(v)); return v;
}
static inline void pmu_write_pmxevtyper(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 1" :: "r"(v));
}
static inline uint32_t pmu_read_pmxevcntr(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c13, 2" : "=r"(v)); return v;
}

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
// M5: the interrupted thread's real SP (frame + 1, see the file-level
// comment above) and full SPSR, plus its thread_t* as a de-facto TID.
uint32_t profiler_sp_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_spsr_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_tid_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
// M5: raw stack bytes starting at profiler_sp_buf[cpu][idx], for
// offline DWARF-CFI unwinding's read_memory callback. No bounds check
// against the thread's actual stack top -- thread stacks in this
// project are always allocated well over PROFILER_STACK_CAPTURE_BYTES
// (4096B minimum, see app/profiler.c's own thread_create calls), and a
// sample landing within 128 bytes of a real overflow is already a
// separate, worse problem than this capture.
uint8_t profiler_stack_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE][PROFILER_STACK_CAPTURE_BYTES];
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
    // frame + 1: one past the end of the pushed struct arm_iframe --
    // the interrupted thread's own real SP, not frame->usp (that's the
    // USR-mode bank, dead here; see the file-level comment above).
    profiler_sp_buf[cpu][idx] = (uint32_t)(frame + 1);
    profiler_spsr_buf[cpu][idx] = frame->spsr;
    profiler_tid_buf[cpu][idx] = (uint32_t)(uintptr_t)get_current_thread();
    memcpy(profiler_stack_buf[cpu][idx], (const void *)(frame + 1),
           PROFILER_STACK_CAPTURE_BYTES);
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
        printf("usage: profiler <start|stop|status|clear|dump|bench|nest|smp|stat|pmu|fpcheck>\n");
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
        memset(profiler_sp_buf, 0, sizeof(profiler_sp_buf));
        memset(profiler_spsr_buf, 0, sizeof(profiler_spsr_buf));
        memset(profiler_tid_buf, 0, sizeof(profiler_tid_buf));
        memset(profiler_stack_buf, 0, sizeof(profiler_stack_buf));
        memset(profiler_ts_buf, 0, sizeof(profiler_ts_buf));
        printf("profiler: buffer cleared\n");
    } else if (!strcmp(sub, "dump")) {
        // M5: text dump, one tagged line per sample, oldest-to-newest
        // per core -- simple and robust over the console's existing
        // printf path rather than a new binary protocol, and cheap
        // enough at the calibrated 3,000,000 baud (see
        // docs/RPI4_BRINGUP.md). scripts/pi4_pc_histogram.py parses
        // the "SAMPLE " lines out of a captured log; the trailing
        // `stack=<hex>` field is PROFILER_STACK_CAPTURE_BYTES raw
        // bytes starting at `sp`, which the host feeds to
        // dwarf_unwind.py's read_memory callback for real multi-frame
        // unwinding instead of the leaf-only view earlier M5 work had.
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            uint32_t count = profiler_total[c] < PROFILER_BUF_SIZE ?
                             profiler_total[c] : PROFILER_BUF_SIZE;
            uint32_t start = profiler_total[c] < PROFILER_BUF_SIZE ?
                             0 : profiler_head[c];
            for (uint32_t n = 0; n < count; n++) {
                uint32_t idx = (start + n) % PROFILER_BUF_SIZE;
                printf("SAMPLE cpu=%d pc=%08x lr=%08x fp=%08x sp=%08x "
                       "spsr=%08x tid=%08x ts=%016llx stack=",
                       c, profiler_pc_buf[c][idx], profiler_lr_buf[c][idx],
                       profiler_fp_buf[c][idx], profiler_sp_buf[c][idx],
                       profiler_spsr_buf[c][idx], profiler_tid_buf[c][idx],
                       (unsigned long long)profiler_ts_buf[c][idx]);
                for (int b = 0; b < PROFILER_STACK_CAPTURE_BYTES; b++) {
                    printf("%02x", profiler_stack_buf[c][idx][b]);
                }
                printf("\n");
            }
        }
        printf("SAMPLE done\n");
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
    } else if (!strcmp(sub, "stat")) {
        // M5 `perf stat`-equivalent: PMU counting mode, real hardware
        // only (see the "pmu" branch below for why this was never
        // attempted on the QEMU target). Printing before each new
        // coprocessor access on purpose -- if this crashes, the UART
        // log shows exactly which register access was the first to
        // fault, the same bisection discipline this project's own
        // M1-M4 hardware bring-up already used (see
        // docs/RPI4_BRINGUP.md).
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 5000000;

        printf("stat: reading PMCEID0/PMCEID1 ...\n");
        uint32_t pmceid0 = pmu_read_pmceid0();
        uint32_t pmceid1 = pmu_read_pmceid1();
        printf("stat: PMCEID0=0x%08x PMCEID1=0x%08x\n", pmceid0, pmceid1);

        // Event 0x03 = L1D_CACHE_REFILL (L1 data cache miss), an
        // architectural ARMv7 PMUv2 event -- PMCEID0 bit N marks
        // whether event N (0-31) is implemented on this core.
        #define PROFILER_PMU_EVENT_L1D_CACHE_REFILL 0x03
        bool have_l1d_refill = (pmceid0 & (1u << PROFILER_PMU_EVENT_L1D_CACHE_REFILL)) != 0;
        printf("stat: L1D_CACHE_REFILL (event 0x03) %s\n",
               have_l1d_refill ? "implemented" : "NOT implemented -- counting it anyway, expect garbage");

        printf("stat: reading PMCR ...\n");
        uint32_t pmcr = pmu_read_pmcr();
        printf("stat: PMCR=0x%08x (N=%u counters)\n", pmcr, (pmcr >> 11) & 0x1f);

        printf("stat: selecting event counter 0 for L1D_CACHE_REFILL ...\n");
        pmu_write_pmselr(0);
        pmu_write_pmxevtyper(PROFILER_PMU_EVENT_L1D_CACHE_REFILL);

        printf("stat: enabling cycle counter + event counter 0 ...\n");
        pmu_write_pmcntenset((1u << 31) | (1u << 0));

        printf("stat: resetting counters and starting (PMCR E|P|C) ...\n");
        pmu_write_pmcr(pmcr | (1u << 0) | (1u << 1) | (1u << 2));

        printf("stat: running workload (%u iters) ...\n", iters);
        profiler_workload_a(iters);

        printf("stat: reading PMCCNTR/PMXEVCNTR ...\n");
        uint32_t cycles = pmu_read_pmccntr();
        pmu_write_pmselr(0);
        uint32_t l1d_refills = pmu_read_pmxevcntr();

        printf("stat: stopping (PMCR E=0) ...\n");
        pmu_write_pmcr(pmcr);

        printf("stat: %u iters, %u cycles, %u L1D_CACHE_REFILL\n", iters, cycles, l1d_refills);
        if (cycles > 0 && iters > 0) {
            // uint64_t throughout: iters*1000 alone can already exceed
            // uint32_t range at the default 5,000,000 iters.
            uint64_t iters_per_cycle_milli = (uint64_t)iters * 1000 / cycles;
            uint64_t refills_per_iter_tenthousandth = (uint64_t)l1d_refills * 10000 / iters;
            printf("stat: %u.%03u iters/cycle, %u.%04u L1D_CACHE_REFILL/iter\n",
                   (uint32_t)(iters_per_cycle_milli / 1000),
                   (uint32_t)(iters_per_cycle_milli % 1000),
                   (uint32_t)(refills_per_iter_tenthousandth / 10000),
                   (uint32_t)(refills_per_iter_tenthousandth % 10000));
        }
        printf("stat: done\n");
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
