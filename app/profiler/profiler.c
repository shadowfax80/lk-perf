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

#include <arch/arch_interrupts.h>
#include <arch/arch_ops.h>
#include <arch/arm.h>
#include <kernel/mp.h>
#include <kernel/thread.h>
#include <lib/console.h>
#include <lk/compiler.h>
#include <lk/reg.h>
#include <platform/interrupts.h>
#include <platform/time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if BCM2711
#include <dev/interrupt/arm_gic.h>
#include <platform/bcm28xx.h>
// Not declared in arm_gic.h despite being a real, global (non-static)
// function (dev/interrupt/arm_gic/arm_gic.c) -- the enum types it
// takes are public, just not this prototype.
status_t gic_configure_interrupt(unsigned int vector,
                                  enum interrupt_trigger_mode tm,
                                  enum interrupt_polarity pol);
#endif

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
static inline uint32_t pmu_read_pmselr(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 5" : "=r"(v)); return v;
}
static inline void pmu_write_pmcntenclr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 2" :: "r"(v));
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
static inline void pmu_write_pmxevcntr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c13, 2" :: "r"(v));
}
static inline void pmu_write_pmintenset(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c14, 1" :: "r"(v));
}
static inline void pmu_write_pmintenclr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c14, 2" :: "r"(v));
}
// Write-1-to-clear, per the ARM architecture: writing back the same
// bits read from PMOVSR clears exactly those overflow flags.
static inline void pmu_write_pmovsr(uint32_t v) {
    __asm__ volatile("mcr p15, 0, %0, c9, c12, 3" :: "r"(v));
}

// Reload event counter 0 (the PMU-sampling counter) from interrupt context. PMSELR is
// a single selector shared by every PMXEVCNTR/PMXEVTYPER access: writing it here without
// restoring it would redirect the next counter access of whatever code this interrupt
// preempted (or, if that code had selected another counter, reload the wrong one).
static inline void profiler_pmu_reload_counter0(uint32_t reload) {
    uint32_t sel = pmu_read_pmselr();
    pmu_write_pmselr(0);
    pmu_write_pmxevcntr(reload);
    pmu_write_pmselr(sel);
}

#if BCM2711
// M5 `profiler pmustart`: BCM2711's PMU interrupt is NOT a PPI like
// the timer (dev/timer/arm_generic) -- it's 4 separate per-core SPIs,
// confirmed from the real Raspberry Pi kernel's own device tree source
// (bcm2711.dtsi): `interrupts = <GIC_SPI 16 ...>, <GIC_SPI 17 ...>,
// <GIC_SPI 18 ...>, <GIC_SPI 19 ...>; interrupt-affinity = <&cpu0>,
// <&cpu1>, <&cpu2>, <&cpu3>;`. GIC_SPI n in DTS notation is real GIC
// interrupt ID n+32, so these are IDs 48-51, one per core.
//
// dev/interrupt/arm_gic/gic_v2.c's own init (`arm_gic_init_hw`) routes
// every SPI to cpu0 only by default (`GICD_ITARGETSR(i/4) = 0x01010101`
// for all of them) -- without reprogramming this, all 4 PMU interrupts
// would land on core 0 regardless of which core's counter actually
// overflowed. IDs 48-51 all share one ITARGETSR register (4 IDs per
// register, byte-per-ID target bitmask), so this is one write:
// byte0(ID48)=0x01(cpu0), byte1(ID49)=0x02(cpu1), byte2(ID50)=0x04(cpu2),
// byte3(ID51)=0x08(cpu3) => 0x08040201.
//
// Computed via this platform's own static "bcm2711 low peripherals +
// gic" mmu_initial_mappings entry (platform/bcm28xx.h's
// BCM_GIC_BASE_VIRT), not arm_gic's own internal (separately,
// dynamically mapped) GICD virtual address -- both are valid device-
// memory aliases of the exact same physical GICD_ITARGETSR register,
// so which one this uses doesn't matter for correctness, and this
// avoids needing arm_gic's private (non-public-header) internal state
// at all.
#define PROFILER_PMU_SPI_BASE 48  // GIC_SPI 16 == real GIC ID 48 (16+32)
#define PROFILER_PMU_GICD_VIRT (BCM_GIC_BASE_VIRT + 0x1000)

static void profiler_pmu_route_and_unmask(void) {
    volatile uint32_t *itargetsr =
        (volatile uint32_t *)(PROFILER_PMU_GICD_VIRT + 0x800 +
                               (PROFILER_PMU_SPI_BASE / 4) * 4);
    *itargetsr = 0x08040201;
    for (int i = 0; i < 4; i++) {
        // gic_v2.c's own init (arm_gicv2_init) unconditionally
        // configures every SPI as edge-triggered ("Initialize all the
        // SPIs to edge triggered") -- but the real DTB marks this
        // interrupt IRQ_TYPE_LEVEL_HIGH, and the PMU signal genuinely
        // is level (asserted until the software clears PMOVSR), not a
        // pulse. Left at the GIC's edge default, this silently never
        // fires (confirmed: no crash, but zero samples, on the first
        // real hardware attempt at this). Reconfigure explicitly
        // rather than assume the GIC's default matches the DTB.
        gic_configure_interrupt(PROFILER_PMU_SPI_BASE + i,
                                 IRQ_TRIGGER_MODE_LEVEL, IRQ_POLARITY_ACTIVE_HIGH);
        unmask_interrupt(PROFILER_PMU_SPI_BASE + i);
    }
}

// M5 `profiler pmustart`/`pmustop`: independent of profiler_enabled --
// both timer-tick and PMU-event sampling are genuine, separately
// selectable modes (per docs/RPI4_BRINGUP.md), sharing the same ring
// buffers. profiler_pmu_reload is the "0xFFFFFFFF - count + 1" preset
// value, reapplied to PMXEVCNTR every time it overflows so sampling
// continues at the same event-count interval.
//
// Per-CPU (fixed, review finding #1): a single shared flag let a
// pmustop on one core silence overflow-acknowledgment for a DIFFERENT
// core that was still armed, hanging it in a level-triggered interrupt
// storm -- see profiler_pmu_arm_this_cpu/profiler_pmu_disarm_this_cpu's
// own comment below for the full failure mode. Declared here (ahead of
// profiler_pc_buf and friends further down) since the arm/disarm
// helpers right below need them already visible.
volatile uint32_t profiler_pmu_enabled[SMP_MAX_CPUS];
volatile uint32_t profiler_pmu_reload;
// K6: overflows that never produced a sample because one IRQ-masked span
// covered more than a whole period (see the compensated reload below).
volatile uint32_t profiler_pmu_missed[SMP_MAX_CPUS];

// Review finding #3: pmustart only rejected count == 0, so a small reload
// (e.g. "pmustart 0x11 100") could make the counter overflow again before
// the interrupt handler -- GIC vector dispatch, several PMU/GIC register
// accesses, a 128-byte stack memcpy -- finishes, leaving a core doing
// nothing but re-entering its own overflow handler. Not a precisely
// measured threshold, just a conservative floor comfortably above that
// handler's known-nontrivial cost; Linux's perf_event_max_sample_rate
// exists for the identical reason.
#define PROFILER_PMU_MIN_COUNT 10000u

// Review finding #1: a real, observed hang, not a hypothetical one.
// pmustart/pmustop each only touched whichever core happened to run the
// shell command -- its own banked PMU registers plus a single SHARED
// profiler_pmu_enabled flag. If pmustart ran on cpu2 and a later pmustop
// ran on cpu0, pmustop cleared the shared flag and cpu0's own registers,
// but cpu2's counter kept counting: profiler_on_tick() then saw
// profiler_pmu_enabled already false for cpu2's next real overflow and
// returned without ever clearing PMOVSR. Since this SPI is
// level-triggered (profiler_pmu_route_and_unmask above), an
// unacknowledged overflow re-raises itself the instant the handler
// returns, forever -- cpu2 never leaves interrupt context again.
//
// Fixed by making the enabled flag per-core (so each core's own overflow
// is judged against its OWN arm/disarm state, never another core's) and
// by broadcasting arm/disarm to every core via mp_sync_exec(), so
// pmustart and pmustop can never run on different cores and leave one
// of them out of sync. profiler_on_tick() also now unconditionally
// acknowledges any real PMU-overflow vector for its own core even when
// that core's flag is false, as defense in depth for the narrow window
// while a core is transitioning between armed and disarmed.
struct profiler_pmu_arm_ctx {
    uint32_t event;
};

static void profiler_pmu_arm_this_cpu(void *context) {
    struct profiler_pmu_arm_ctx *ctx = (struct profiler_pmu_arm_ctx *)context;
    uint cpu = arch_curr_cpu_num();
    // Runs from an IPI on every core: keep the preempted code's PMSELR selection.
    uint32_t sel = pmu_read_pmselr();
    pmu_write_pmselr(0);
    pmu_write_pmxevtyper(ctx->event);
    pmu_write_pmxevcntr(profiler_pmu_reload);
    pmu_write_pmselr(sel);
    pmu_write_pmcntenset(1u << 0);
    // Set this core's flag BEFORE enabling its overflow interrupt, not
    // after -- otherwise a real overflow landing in between finds
    // profiler_on_tick() still reading a stale "disabled" flag on this
    // exact core.
    profiler_pmu_enabled[cpu] = 1;
    pmu_write_pmintenset(1u << 0);
    uint32_t pmcr = pmu_read_pmcr();
    pmu_write_pmcr(pmcr | (1u << 0));
}

static void profiler_pmu_disarm_this_cpu(void *context) {
    (void)context;
    uint cpu = arch_curr_cpu_num();
    pmu_write_pmintenclr(1u << 0);
    uint32_t pmcr = pmu_read_pmcr();
    pmu_write_pmcr(pmcr & ~(1u << 0));
    profiler_pmu_enabled[cpu] = 0;
    // Clear any overflow that raced in right before pmintenclr took
    // effect, so it isn't left pending on this core.
    pmu_write_pmovsr(1u << 0);
}
#endif

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
// K6: why a sample may sit at an unmask point instead of where the time
// went. The trigger is held pending while the core runs with CPSR.I set
// and is taken right after the unmask, so the sample PC is the unmask
// point. `lat` measures how late the interrupt was taken: CNTPCT ticks
// past the timer deadline (src 't') or PMU events counted past the
// overflow (src 'p'). `msite`/`mgap` name the masked region that closed
// most recently on this core (arch/arm/arm/irqmask.c, chained through
// IRQ handlers taken at the same unmask) and how many CNTPCT ticks
// before this sample it ended; a large `lat` with a small `mgap` means
// the sample was delayed by that region.
uint8_t profiler_src_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_lat_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_msite_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
uint32_t profiler_mgap_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
volatile uint32_t profiler_head[SMP_MAX_CPUS];
volatile uint32_t profiler_total[SMP_MAX_CPUS];
volatile uint32_t profiler_enabled;
// profiler_pmu_enabled[]/profiler_pmu_reload declared earlier in this
// file (ahead of the arm/disarm helpers that need them) -- see that
// declaration's own comment.

// K6: architected generic-timer registers (AArch32 CP15). CNTPCT is the
// system count; CNTP_CVAL is the physical timer's compare value, i.e. the
// deadline the tick fired for (LK's tick uses the CNTP timer on this board).
static inline uint64_t profiler_cntpct(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t profiler_cntp_cval(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 2, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint32_t profiler_cntfrq(void) {
    uint32_t v;
    __asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(v));
    return v;
}

static inline uint32_t profiler_sat32(uint64_t v) {
    return v > 0xffffffffu ? 0xffffffffu : (uint32_t)v;
}

// Called from dev/interrupt/arm_gic/gic_v2.c on every IRQ (weak default is
// a no-op when this file isn't linked in). Keep this minimal and
// allocation-free -- it runs in interrupt context on every timer tick,
// the same atomicity discipline as this project family's earlier
// counter-bump-stub lessons. Runs concurrently on every core (PPI, no
// cross-core serialization) -- touches only this core's own array slots,
// so no locking is needed despite running on N cores at once.
void profiler_on_tick(struct arm_iframe *frame, unsigned int vector) {
    uint cpu = arch_curr_cpu_num();
    bool is_timer_tick = (vector == PROFILER_TIMER_IRQ) && profiler_enabled;
#if BCM2711
    // M5: the PMU's 4 per-core SPIs (see profiler_pmu_route_and_unmask's
    // own comment) are routed one-to-one with cores, so a real overflow
    // on this exact core always arrives as its own specific vector --
    // no need to check *which* of the 4 fired, just that one did.
    // is_pmu_vector alone (vector match, no enabled check) is also used
    // below to unconditionally acknowledge the interrupt even when this
    // core isn't currently "enabled" -- see the early-return branch.
    bool is_pmu_vector = vector >= PROFILER_PMU_SPI_BASE && vector < PROFILER_PMU_SPI_BASE + 4;
    bool is_pmu_overflow = is_pmu_vector && profiler_pmu_enabled[cpu];
#else
    bool is_pmu_vector = false;
    bool is_pmu_overflow = false;
#endif
    if (!is_timer_tick && !is_pmu_overflow) {
#if BCM2711
        // Review finding #1: a real PMU-overflow vector for THIS core
        // must always be acknowledged, even when profiler_pmu_enabled[cpu]
        // is false -- it's level-triggered, so an unacknowledged one
        // re-raises itself the instant this handler returns and hangs
        // the core in a pure interrupt storm. See
        // profiler_pmu_arm_this_cpu/profiler_pmu_disarm_this_cpu's own
        // comment for the exact scenario this fixes, confirmed on real
        // hardware.
        if (is_pmu_vector) {
            pmu_write_pmovsr(1u << 0);
            profiler_pmu_reload_counter0(profiler_pmu_reload);
        }
#endif
        return;
    }

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

    // K6: interrupt delay and the most recent masked region on this core.
    uint64_t now_count = profiler_cntpct();
    uint32_t lat;
    uint8_t src;
#if BCM2711
    if (is_pmu_overflow) {
        // The counter wrapped to 0 at overflow and kept counting, so its
        // value now is the number of events since the overflow.
        uint32_t sel = pmu_read_pmselr();
        pmu_write_pmselr(0);
        lat = pmu_read_pmxevcntr();
        pmu_write_pmselr(sel);
        src = 'p';
    } else
#endif
    {
        uint64_t deadline = profiler_cntp_cval();
        lat = now_count >= deadline ? profiler_sat32(now_count - deadline) : 0;
        src = 't';
    }
    const struct arm_irqmask_cpu *mc = &arm_irqmask_cpu[cpu];
    // The cause chains through IRQ handlers taken back to back at the
    // same unmask (arch/arm/arm/irqmask.c), so `msite` names the masked
    // region that held this sample back, not the handler that ran just
    // before it.
    bool attributed = arm_irqmask_on && mc->cause_end != 0 && now_count >= mc->cause_end;
    profiler_src_buf[cpu][idx] = src;
    profiler_lat_buf[cpu][idx] = lat;
    profiler_msite_buf[cpu][idx] = attributed ? mc->cause_site : 0;
    profiler_mgap_buf[cpu][idx] = attributed ? profiler_sat32(now_count - mc->cause_end)
                                             : 0xffffffffu;
    profiler_head[cpu] = (idx + 1) % PROFILER_BUF_SIZE;
    profiler_total[cpu]++;

#if BCM2711
    if (is_pmu_overflow) {
        pmu_write_pmovsr(1u << 0);           // write-1-to-clear counter0's overflow flag
        // K6: keep the sampling grid fixed in event time. Reloading the full
        // period here would restart it from whenever this (possibly delayed)
        // interrupt was taken, so after an IRQ-masked span every later
        // overflow shifts with it and the samples phase-lock onto the
        // masking pattern. Credit the `lat` events already counted towards
        // the next period instead; whole periods inside one masked span
        // are lost overflows, counted rather than silently dropped.
        uint32_t period = 0u - profiler_pmu_reload;
        uint32_t into_next = lat;
        if (period && lat >= period) {
            profiler_pmu_missed[cpu] += lat / period;
            into_next = lat % period;
        }
        profiler_pmu_reload_counter0(profiler_pmu_reload + into_next);  // next window
    }
#endif
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

// K6 ground-truth workload: alternate equal spins with IRQs masked and
// unmasked, so half of the time is masked by construction. Masked-time
// accounting should report about 50% on the core running it, at
// profiler_masked_spin's site, and samples triggered during the masked
// half should be taken late and attributed to that site.
__NO_INLINE static void profiler_spin_ticks(uint64_t span) {
    uint64_t t0 = profiler_cntpct();
    while (profiler_cntpct() - t0 < span) {
    }
}

__NO_INLINE static void profiler_masked_spin(uint64_t span) {
    arch_disable_ints();
    profiler_spin_ticks(span);
    arch_enable_ints();
}

__NO_INLINE static void profiler_unmasked_spin(uint64_t span) {
    profiler_spin_ticks(span);
}

static void profiler_masktest(uint32_t loops, uint32_t us) {
    uint64_t span = ((uint64_t)us * profiler_cntfrq()) / 1000000u;
    for (uint32_t i = 0; i < loops; i++) {
        profiler_masked_spin(span);
        profiler_unmasked_spin(span);
    }
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

// M5 edge-case validation (`profiler edgetest`): deliberately targets
// specific things the sampling/unwinding pipeline hasn't been tested
// against on real hardware yet.
//
// 1. A genuinely deep, non-tail-call chain. workload_outer/mid above
//    are BOTH tail calls ("return child(x);" with nothing after --
//    confirmed via disassembly to compile to a plain `b.w`, no `bl`,
//    no frame at all), so they've never actually tested multi-level
//    unwinding through real stack frames on this target -- every
//    multi-frame result so far reached its depth some other way
//    (through profiler_smp_worker's own real `bl`, or by luck).
//    Every level here does work AFTER its child call returns, so
//    none of them can be tail-call-eliminated: each one's own return
//    address into ITS caller must survive across the `bl` it makes,
//    forcing a real spilled LR and a real frame at every level.
//    8 levels deep, expecting to likely exceed
//    PROFILER_STACK_CAPTURE_BYTES (128) by the outer few levels and
//    see the unwinder degrade gracefully -- on real hardware it
//    turned out each of these small frames only needs ~8-16 bytes, so
//    the full 8-level chain (deep_8 -> ... -> deep_1 -> cmd_profiler)
//    fit inside the window and unwound completely every time, no
//    truncation. A real, useful negative result: 128 bytes covers
//    meaningfully deep real (non-recursive) chains in this codebase,
//    not just the shallow ones tested before this. See
//    profiler_recurse below for where the window *does* end up being
//    the real limit, on a workload deep enough to actually reach it.
__NO_INLINE static uint32_t profiler_deep_8(uint32_t x) {
    volatile uint32_t v = x * 3u + 1u;
    return v;
}
__NO_INLINE static uint32_t profiler_deep_7(uint32_t x) { return profiler_deep_8(x) + 7; }
__NO_INLINE static uint32_t profiler_deep_6(uint32_t x) { return profiler_deep_7(x) + 6; }
__NO_INLINE static uint32_t profiler_deep_5(uint32_t x) { return profiler_deep_6(x) + 5; }
__NO_INLINE static uint32_t profiler_deep_4(uint32_t x) { return profiler_deep_5(x) + 4; }
__NO_INLINE static uint32_t profiler_deep_3(uint32_t x) { return profiler_deep_4(x) + 3; }
__NO_INLINE static uint32_t profiler_deep_2(uint32_t x) { return profiler_deep_3(x) + 2; }
__NO_INLINE static uint32_t profiler_deep_1(uint32_t x) { return profiler_deep_2(x) + 1; }

static void profiler_workload_deepchain(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += profiler_deep_1(i);
    }
    profiler_sink = acc;
}

// 2. Real (non-tail) recursion: the SAME instruction address recurs at
// every depth, on purpose. This found a real bug on first real
// hardware use, not a hypothetical one: DwarfCFIUnwinder.unwind()'s
// original cycle guard checked pc alone (`if cur_pc in chain: break`),
// which can't tell genuine recursion (same pc, a *different* cfa/SP
// at every depth -- each recursive call has its own stack frame) from
// an actual CFI-driven infinite loop (same pc *and* the same cfa,
// meaning the walk truly isn't making progress) -- it stopped every
// one of this function's real 20-level-deep unwinds after just 1
// frame. Fixed by keying the guard on (pc, cfa) instead; verified
// against the exact scenario in scripts/test_dwarf_unwind.py, and
// confirmed on real hardware afterward: this now correctly reaches
// depth 15-18 before running out of PROFILER_STACK_CAPTURE_BYTES
// (128) -- the *capture window*, not the algorithm, is what limits
// how deep a real recursive unwind can go here.
__NO_INLINE static uint32_t profiler_recurse(uint32_t depth, uint32_t acc) {
    if (depth == 0) {
        return acc;
    }
    uint32_t r = profiler_recurse(depth - 1, acc + depth);
    return r ^ depth;  // work after the recursive call -> real frame, not a tail call
}

static void profiler_workload_recurse(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += profiler_recurse(20, i);
    }
    profiler_sink = acc;
}

// 3. Force a real frame-pointer (r7, the Thumb FP convention)
// call chain -- everything else in this project compiles at -O2 with
// frame pointers omitted (confirmed repeatedly: fp=0 or garbage in
// most captured samples so far), so the r7-based CFA path
// scripts/dwarf_unwind.py's unwinder supports has only ever been
// exercised by the hand-written thumb_edge.S test, never by real
// compiled code on real hardware.
__attribute__((optimize("no-omit-frame-pointer")))
__NO_INLINE static uint32_t profiler_fp_forced_inner(uint32_t x) {
    volatile uint32_t v = x * 7u;
    return v + 1;
}
__attribute__((optimize("no-omit-frame-pointer")))
__NO_INLINE static uint32_t profiler_fp_forced_outer(uint32_t x) {
    uint32_t r = profiler_fp_forced_inner(x);
    return r + 2;
}

static void profiler_workload_fpforce(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += profiler_fp_forced_outer(i);
    }
    profiler_sink = acc;
}

// 4. A genuine ARM-mode function called from this project's otherwise
// all-Thumb code (`-mthumb` is the default build flag here) -- real
// BL/BX interworking, mode switch included. Every other ISA-bit test
// so far has been the offline, hand-built scripts/test_dwarf_unwind.py
// scenario; this is the first on real hardware, with a real sample
// landing inside genuinely ARM-mode code.
__attribute__((target("arm")))
__NO_INLINE static uint32_t profiler_arm_mode_func(uint32_t x) {
    volatile uint32_t v = x * 5u + 2u;
    return v;
}

static void profiler_workload_armmode(uint32_t iters) {
    uint32_t acc = 0;
    for (uint32_t i = 0; i < iters; i++) {
        acc += profiler_arm_mode_func(i);
    }
    profiler_sink = acc;
}

// `profiler dump`'s per-sample integrity check (see that command's own
// comment for why): FNV-1a-style, folding each binary field into a
// running multiply-xor state -- cheap on-target (no lookup table, one
// multiply per field) and trivial to reproduce exactly on the host in
// Python. Deliberately over the sample's real binary values, not the
// printed hex text, so it catches corruption of the printed text
// itself without caring how that text was formatted.
static inline uint32_t profiler_fnv(uint32_t c, uint32_t v) {
    return (c ^ v) * 16777619u;
}

// K6 records append src/lat/msite/mgap after the stack bytes, so the
// pre-K6 prefix of the checksum is unchanged and old logs still verify.
static uint32_t profiler_sample_checksum(uint32_t cpu, uint32_t seq, uint32_t pc, uint32_t lr,
                                          uint32_t fp, uint32_t sp, uint32_t spsr, uint32_t tid,
                                          uint64_t ts, const uint8_t *stack, uint32_t src,
                                          uint32_t lat, uint32_t msite, uint32_t mgap) {
    uint32_t c = 0x811c9dc5u;
    c ^= cpu; c *= 16777619u;
    c ^= seq; c *= 16777619u;
    c ^= pc; c *= 16777619u;
    c ^= lr; c *= 16777619u;
    c ^= fp; c *= 16777619u;
    c ^= sp; c *= 16777619u;
    c ^= spsr; c *= 16777619u;
    c ^= tid; c *= 16777619u;
    c ^= (uint32_t)(ts & 0xffffffffu); c *= 16777619u;
    c ^= (uint32_t)(ts >> 32); c *= 16777619u;
    for (int i = 0; i < PROFILER_STACK_CAPTURE_BYTES; i++) {
        c ^= stack[i]; c *= 16777619u;
    }
    c = profiler_fnv(c, src);
    c = profiler_fnv(c, lat);
    c = profiler_fnv(c, msite);
    c = profiler_fnv(c, mgap);
    return c;
}

// K6: start a fresh accounting window on every core, then enable it.
static void profiler_mask_reset_this_cpu(void *context) {
    arm_irqmask_reset_this_cpu(*(const uint64_t *)context);
}

static void profiler_mask_sync_this_cpu(void *context) {
    (void)context;
}

static void profiler_mask_on(void) {
    arm_irqmask_on = 0;
    uint64_t start = profiler_cntpct();
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_mask_reset_this_cpu, &start);
    arm_irqmask_on = 1;
}

static uint32_t profiler_permille(uint64_t part, uint64_t whole) {
    return whole ? (uint32_t)((part * 1000u) / whole) : 0;
}

static uint32_t profiler_ticks_to_ns(uint64_t ticks) {
    uint32_t f = profiler_cntfrq();
    return f ? profiler_sat32((ticks * 1000000000ull) / f) : 0;
}

// Human-readable summary; the dump carries the same data for the host.
static void profiler_mask_print(void) {
    uint64_t now = profiler_cntpct();
    printf("mask: accounting %s, CNTFRQ=%u Hz\n", arm_irqmask_on ? "on" : "off",
           profiler_cntfrq());
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        const struct arm_irqmask_cpu *m = &arm_irqmask_cpu[c];
        uint64_t window = m->window_start && now > m->window_start ? now - m->window_start : 0;
        uint32_t pm = profiler_permille(m->masked_ticks, window);
        uint32_t ipm = profiler_permille(m->irq_ticks, window);
        printf("mask: cpu%d masked %u.%u%% (irq handlers %u.%u%%) in %u regions, "
               "longest %u ns at %08x, unrecorded sites %u\n",
               c, pm / 10, pm % 10, ipm / 10, ipm % 10, m->regions,
               profiler_ticks_to_ns(m->max_ticks), m->max_site, m->dropped_regions);
        bool shown[ARM_IRQMASK_SITES] = { false };
        for (int rank = 0; rank < 5; rank++) {
            int best = -1;
            for (int i = 0; i < (int)ARM_IRQMASK_SITES; i++) {
                if (!shown[i] && m->sites[i].count &&
                    (best < 0 || m->sites[i].ticks > m->sites[best].ticks))
                    best = i;
            }
            if (best < 0)
                break;
            shown[best] = true;
            const struct arm_irqmask_site *e = &m->sites[best];
            uint32_t spm = profiler_permille(e->ticks, window);
            printf("mask:   site %08x  %u.%u%%  %u regions  longest %u ns\n", e->site,
                   spm / 10, spm % 10, e->count, profiler_ticks_to_ns(e->max_ticks));
        }
    }
}

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: profiler <start|stop|status|clear|dump|bench|nest|smp|edgetest|stat|pmustart|pmustop|pmu|maskon|maskoff|mask|masktest>\n");
        return -1;
    }

    const char *sub = argv[1].str;

    if (!strcmp(sub, "maskon")) {
        profiler_mask_on();
        printf("mask: IRQ-masked time accounting on (new window on all %d cores)\n",
               SMP_MAX_CPUS);
    } else if (!strcmp(sub, "maskoff")) {
        arm_irqmask_on = 0;
        printf("mask: IRQ-masked time accounting off (totals kept)\n");
    } else if (!strcmp(sub, "mask")) {
        profiler_mask_print();
    } else if (!strcmp(sub, "masktest")) {
        uint32_t loops = argc >= 3 ? (uint32_t)argv[2].u : 2000;
        uint32_t us = argc >= 4 ? (uint32_t)argv[3].u : 500;
        printf("masktest: %u x (%u us masked + %u us unmasked) on cpu%u ...\n", loops, us, us,
               arch_curr_cpu_num());
        profiler_masktest(loops, us);
        printf("masktest: done\n");
    } else if (!strcmp(sub, "start")) {
        if (!arm_irqmask_on) {
            profiler_mask_on();
            printf("mask: IRQ-masked time accounting started with sampling\n");
        }
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
        memset(profiler_src_buf, 0, sizeof(profiler_src_buf));
        memset(profiler_lat_buf, 0, sizeof(profiler_lat_buf));
        memset(profiler_msite_buf, 0, sizeof(profiler_msite_buf));
        memset(profiler_mgap_buf, 0, sizeof(profiler_mgap_buf));
        for (int c = 0; c < SMP_MAX_CPUS; c++)
            profiler_pmu_missed[c] = 0;
        if (arm_irqmask_on)
            profiler_mask_on();
        printf("profiler: buffer cleared%s\n",
               arm_irqmask_on ? " (masked-time window restarted)" : "");
    } else if (!strcmp(sub, "dump")) {
        // M5: text dump, one tagged line per sample, oldest-to-newest
        // per core -- simple and robust over the console's existing
        // printf path rather than a new binary protocol, and cheap
        // enough at the calibrated baud (see docs/RPI4_BRINGUP.md).
        // scripts/pi4_pc_histogram.py parses the "SAMPLE " lines out of
        // a captured log; the trailing `stack=<hex>` field is
        // PROFILER_STACK_CAPTURE_BYTES raw bytes starting at `sp`,
        // which the host feeds to dwarf_unwind.py's read_memory
        // callback for real multi-frame unwinding instead of the
        // leaf-only view earlier M5 work had.
        //
        // `seq`/`crc` (added 2026-09-29): a real, sustained UART
        // transfer of a large dump was found on real hardware to
        // occasionally corrupt a byte with an exact 64-byte period
        // (a USB Full-Speed bulk packet boundary artifact in the
        // host's serial adapter/driver, not something this project's
        // own UART timing controls) -- present at both 3,000,000 and
        // 6,000,000 baud, with no settling delay able to fix it since
        // it recurs throughout a long continuous burst, not just at
        // the baud-switch transient. Most such corruption already
        // breaks hex-parseability (a NUL byte isn't a valid hex
        // digit), which the host's regex already silently drops --
        // but silently, with no way to know a sample went missing,
        // and no defense at all against a bit-flip that lands on
        // another valid hex digit. `seq` is a plain per-dump counter:
        // a gap in the sequence means the host lost or malformed a
        // line entirely. `crc` is a cheap FNV-1a-style hash over the
        // sample's actual binary fields (not the printed text) --
        // the host recomputes it from what it parsed and drops any
        // sample where they disagree, the same "count it, don't trust
        // it, don't abort the whole report" resilience already used
        // for a failed unwind (review finding #12).
        // K6: end the masked-time window before printing, so the dump's
        // own console output is not counted. Once accounting is off, an
        // IPI to every core completes only after each core's in-flight
        // (masked) update has finished, so the snapshot is consistent.
        uint64_t now = profiler_cntpct();
        uint32_t mask_was_on = arm_irqmask_on;
        arm_irqmask_on = 0;
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_mask_sync_this_cpu, NULL);
        static struct arm_irqmask_cpu mask_snap[SMP_MAX_CPUS];
        memcpy(mask_snap, arm_irqmask_cpu, sizeof(mask_snap));

        uint32_t seq = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            uint32_t count = profiler_total[c] < PROFILER_BUF_SIZE ?
                             profiler_total[c] : PROFILER_BUF_SIZE;
            uint32_t start = profiler_total[c] < PROFILER_BUF_SIZE ?
                             0 : profiler_head[c];
            for (uint32_t n = 0; n < count; n++) {
                uint32_t idx = (start + n) % PROFILER_BUF_SIZE;
                uint32_t pc = profiler_pc_buf[c][idx];
                uint32_t lr = profiler_lr_buf[c][idx];
                uint32_t fp = profiler_fp_buf[c][idx];
                uint32_t sp = profiler_sp_buf[c][idx];
                uint32_t spsr = profiler_spsr_buf[c][idx];
                uint32_t tid = profiler_tid_buf[c][idx];
                uint64_t ts = profiler_ts_buf[c][idx];
                const uint8_t *stack = profiler_stack_buf[c][idx];
                uint32_t src = profiler_src_buf[c][idx];
                uint32_t lat = profiler_lat_buf[c][idx];
                uint32_t msite = profiler_msite_buf[c][idx];
                uint32_t mgap = profiler_mgap_buf[c][idx];
                uint32_t crc = profiler_sample_checksum((uint32_t)c, seq, pc, lr, fp, sp,
                                                         spsr, tid, ts, stack, src, lat,
                                                         msite, mgap);
                printf("SAMPLE seq=%08x cpu=%d pc=%08x lr=%08x fp=%08x sp=%08x "
                       "spsr=%08x tid=%08x ts=%016llx stack=",
                       seq, c, pc, lr, fp, sp, spsr, tid, (unsigned long long)ts);
                for (int b = 0; b < PROFILER_STACK_CAPTURE_BYTES; b++) {
                    printf("%02x", stack[b]);
                }
                printf(" src=%c lat=%08x msite=%08x mgap=%08x crc=%08x\n",
                       src ? (char)src : '?', lat, msite, mgap, crc);
                seq++;
            }
        }
        // K6: masked-time accounting as frozen at the start of the dump
        // (the dump ends the window), checksummed like the samples. All
        // counts are CNTPCT ticks; MASKINFO gives the tick rate.
        uint32_t freq = profiler_cntfrq();
        uint32_t ic = profiler_fnv(profiler_fnv(profiler_fnv(profiler_fnv(
            0x811c9dc5u, freq), mask_was_on), (uint32_t)now), (uint32_t)(now >> 32));
        printf("MASKINFO cntfrq=%08x on=%u now=%016llx crc=%08x\n", freq, mask_was_on,
               (unsigned long long)now, ic);
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            const struct arm_irqmask_cpu *m = &mask_snap[c];
            uint32_t k = 0x811c9dc5u;
            k = profiler_fnv(k, (uint32_t)c);
            k = profiler_fnv(k, (uint32_t)m->window_start);
            k = profiler_fnv(k, (uint32_t)(m->window_start >> 32));
            k = profiler_fnv(k, (uint32_t)m->masked_ticks);
            k = profiler_fnv(k, (uint32_t)(m->masked_ticks >> 32));
            k = profiler_fnv(k, m->regions);
            k = profiler_fnv(k, (uint32_t)m->irq_ticks);
            k = profiler_fnv(k, (uint32_t)(m->irq_ticks >> 32));
            k = profiler_fnv(k, m->irq_regions);
            k = profiler_fnv(k, m->max_ticks);
            k = profiler_fnv(k, m->max_site);
            k = profiler_fnv(k, m->dropped_regions);
            k = profiler_fnv(k, (uint32_t)m->dropped_ticks);
            k = profiler_fnv(k, (uint32_t)(m->dropped_ticks >> 32));
            printf("MASKCPU cpu=%d start=%016llx masked=%016llx regions=%08x irq=%016llx "
                   "irqregions=%08x max=%08x maxsite=%08x dropped=%08x droppedticks=%016llx "
                   "crc=%08x\n", c, (unsigned long long)m->window_start,
                   (unsigned long long)m->masked_ticks, m->regions,
                   (unsigned long long)m->irq_ticks, m->irq_regions, m->max_ticks, m->max_site,
                   m->dropped_regions, (unsigned long long)m->dropped_ticks, k);
            for (int i = 0; i < (int)ARM_IRQMASK_SITES; i++) {
                const struct arm_irqmask_site *e = &m->sites[i];
                if (!e->count)
                    continue;
                uint32_t sk = 0x811c9dc5u;
                sk = profiler_fnv(sk, (uint32_t)c);
                sk = profiler_fnv(sk, e->site);
                sk = profiler_fnv(sk, e->count);
                sk = profiler_fnv(sk, (uint32_t)e->ticks);
                sk = profiler_fnv(sk, (uint32_t)(e->ticks >> 32));
                sk = profiler_fnv(sk, e->max_ticks);
                printf("MASKSITE cpu=%d site=%08x count=%08x ticks=%016llx max=%08x crc=%08x\n",
                       c, e->site, e->count, (unsigned long long)e->ticks, e->max_ticks, sk);
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
    } else if (!strcmp(sub, "edgetest")) {
        // M5 edge-case validation -- see the four profiler_workload_*
        // functions' own comments above for exactly what each one
        // targets. Runs all four back to back so one profiler
        // start/edgetest/stop/dump captures samples across all of
        // them; iters is per-workload, kept modest since deepchain and
        // recurse both do real per-call work (unlike bench/nest's pure
        // arithmetic loops).
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 2000000;
        printf("profiler: edgetest deepchain (%u iters) ...\n", iters);
        profiler_workload_deepchain(iters);
        printf("profiler: edgetest recurse (%u iters) ...\n", iters);
        profiler_workload_recurse(iters);
        printf("profiler: edgetest fpforce (%u iters) ...\n", iters);
        profiler_workload_fpforce(iters);
        printf("profiler: edgetest armmode (%u iters) ...\n", iters);
        profiler_workload_armmode(iters);
        printf("profiler: edgetest done (sink=%u, ignore -- just prevents dead-code elim)\n",
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

        // Event counter 1, not 0: counter 0 belongs to `pmustart` sampling, which
        // may be armed right now. Likewise no PMCR.P (it would reset counter 0 too):
        // zero counter 1 directly.
        printf("stat: selecting event counter 1 for L1D_CACHE_REFILL ...\n");
        pmu_write_pmselr(1);
        pmu_write_pmxevtyper(PROFILER_PMU_EVENT_L1D_CACHE_REFILL);
        pmu_write_pmxevcntr(0);

        printf("stat: enabling cycle counter + event counter 1 ...\n");
        pmu_write_pmcntenset((1u << 31) | (1u << 1));

        printf("stat: resetting the cycle counter and starting (PMCR E|C) ...\n");
        pmu_write_pmcr(pmcr | (1u << 0) | (1u << 2));

        printf("stat: running workload (%u iters) ...\n", iters);
        profiler_workload_a(iters);

        printf("stat: reading PMCCNTR/PMXEVCNTR ...\n");
        uint32_t cycles = pmu_read_pmccntr();
        pmu_write_pmselr(1);
        uint32_t l1d_refills = pmu_read_pmxevcntr();

        // Stop only counter 1; PMCR goes back to what it was (enabled if sampling is).
        printf("stat: stopping (counter 1 off, PMCR restored) ...\n");
        pmu_write_pmcntenclr(1u << 1);
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
    } else if (!strcmp(sub, "pmustart")) {
#if BCM2711
        // M5 PMU-event-driven sampling, real second sampling mode
        // alongside timer-tick (`start`/`stop`), not instead of it --
        // both are genuine, selectable options sharing the same ring
        // buffers. Runs on every core (fixed, review finding #1): PMU
        // control registers are per-core (banked in hardware, unlike
        // the shared distributor state profiler_pmu_route_and_unmask
        // sets up once), so arming/disarming broadcasts via
        // mp_sync_exec() instead of only touching whichever core ran
        // this command -- see profiler_pmu_arm_this_cpu's own comment.
        if (argc < 4) {
            printf("usage: profiler pmustart <event> <count>  "
                   "(both accept a 0x prefix for hex)\n");
            return -1;
        }
        // Review finding #8: argv[].u parses plain decimal only, but
        // this command's own usage/docs always showed hex event IDs --
        // "pmustart 11" silently programmed decimal 11 (0x0B) instead
        // of the intended CPU_CYCLES (0x11), with no warning since the
        // A72 implements both. strtoul's base-0 auto-detects a leading
        // "0x" and falls back to decimal otherwise.
        uint32_t event = (uint32_t)strtoul(argv[2].str, NULL, 0);
        uint32_t count = (uint32_t)strtoul(argv[3].str, NULL, 0);
        if (count < PROFILER_PMU_MIN_COUNT) {
            printf("pmustart: count must be >= %u -- see "
                   "PROFILER_PMU_MIN_COUNT's own comment for why\n",
                   PROFILER_PMU_MIN_COUNT);
            return -1;
        }

        uint32_t pmceid0 = pmu_read_pmceid0();
        if (event < 32 && !(pmceid0 & (1u << event))) {
            printf("pmustart: warning: event 0x%x not marked implemented in "
                   "PMCEID0=0x%08x -- trying anyway\n", event, pmceid0);
        }

        printf("pmustart: routing PMU SPIs 48-51 to cpu0-3 and unmasking ...\n");
        profiler_pmu_route_and_unmask();

        profiler_pmu_reload = 0xFFFFFFFFu - count + 1u;
        printf("pmustart: arming event counter 0 on all %d cores for event "
               "0x%x, reload every %u ...\n", SMP_MAX_CPUS, event, count);
        struct profiler_pmu_arm_ctx ctx = { .event = event };
        if (!arm_irqmask_on) {
            profiler_mask_on();
            printf("mask: IRQ-masked time accounting started with sampling\n");
        }
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_pmu_arm_this_cpu, &ctx);

        printf("profiler: PMU-event sampling started on all %d cores "
               "(event 0x%x, every %u occurrences)\n", SMP_MAX_CPUS, event, count);
#else
        printf("pmustart: needs real hardware (BCM2711) -- see the 'pmu' command\n");
#endif
    } else if (!strcmp(sub, "pmustop")) {
#if BCM2711
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_pmu_disarm_this_cpu, NULL);
        printf("profiler: PMU-event sampling stopped on all %d cores\n", SMP_MAX_CPUS);
#else
        printf("pmustop: needs real hardware (BCM2711)\n");
#endif
    } else if (!strcmp(sub, "pmu")) {
        // Stage 5 (PMU-overflow-triggered sampling) is real-hardware-only
        // on this project -- confirmed two independent ways on the
        // QEMU target, not assumed:
        //
        // 1. No PMU interrupt route exists to arm at all there. Dumping
        //    that exact QEMU invocation's own generated device tree
        //    (`qemu-system-arm -machine virt -cpu cortex-a15 -machine
        //    dumpdtb=...`) shows the `pmu {};` node present but empty --
        //    no `compatible`, no `interrupts` property.
        // 2. PMU coprocessor register access itself was unsafe there,
        //    not just the interrupt path: even the single already-
        //    public, already-proven LK accessor arch_cycle_count()
        //    (arch/arm/include/arch/arch_ops.h, `mrc p15,0,%0,c9,c13,0`
        //    = PMCCNTR, used throughout bolt-aarch32's bolt_bench)
        //    reliably faulted with "undefined abort" the moment it
        //    executed on that bare-metal image -- verified directly,
        //    not inferred. QEMU's minimal image has no secure-monitor
        //    boot stage to clear the NSACR PMU-access trap.
        //
        // M5: on THIS real hardware, PMU access works cleanly -- see
        // `profiler stat` above, confirmed on real Pi 4B silicon: reads
        // PMCEID0/1, programs an event counter, runs a workload, reads
        // real counts back, no fault anywhere. The Pi's own firmware
        // does clear that trap during its real secure-world boot, as
        // hypothesized (not previously confirmed) when this comment
        // was first written for the QEMU target. Only the PMU
        // *interrupt* path (event-overflow-triggered sampling, as
        // opposed to plain counting) remains unverified on real
        // hardware -- that's the actual PMU-event-driven-sampling
        // milestone, still ahead.
        printf("pmu: counting mode confirmed working on real hardware --\n");
        printf("pmu: see `profiler stat`. What's still unverified here is\n");
        printf("pmu: the PMU *interrupt* path (event-overflow-triggered\n");
        printf("pmu: sampling) -- see this command's own source comment.\n");
    } else if (!strcmp(sub, "status")) {
        uint32_t total = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            total += profiler_total[c];
            printf("profiler: cpu%d total_samples=%u head=%u%s pmu_missed=%u\n", c,
                   profiler_total[c], profiler_head[c],
                   profiler_total[c] >= PROFILER_BUF_SIZE ? " (WRAPPED)" : "",
                   profiler_pmu_missed[c]);
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
STATIC_COMMAND("profiler", "bare-metal sampling profiler (start|stop|status|clear|mask)", &cmd_profiler)
STATIC_COMMAND_END(profiler);
