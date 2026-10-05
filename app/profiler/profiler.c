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
 * K10 (Pi 4B): timer-mode sampling no longer uses LK's tick. LK re-arms
 * that tick relative to when it is handled, so a tick delayed by masked
 * code shifts every later one and the samples phase-lock onto the masking
 * pattern. The profiler now owns the per-core virtual timer (CNTV, which
 * LK does not use here) and keeps its own sampling grid; see
 * profiler_timer_next().
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
#include <kernel/event.h>
#include <kernel/mp.h>
#include <kernel/mutex.h>
#include <kernel/thread.h>
#include <lib/console.h>
#include <lk/compiler.h>
#include <lk/reg.h>
#include <platform/interrupts.h>
#include <platform/time.h>
#include <stdarg.h>
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
// K10: the profiler's own per-core sampling timer, the virtual timer (GIC
// PPI 27 on BCM2711; LK's tick stays on the Non-secure physical timer, 30).
#define PROFILER_TIMER_IRQ 27
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
static inline uint32_t pmu_read_pmcntenset(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 1" : "=r"(v)); return v;
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
static inline uint32_t pmu_read_pmovsr(void) {
    uint32_t v; __asm__ volatile("mrc p15, 0, %0, c9, c12, 3" : "=r"(v)); return v;
}
// K7: ARMv8 AArch32 gives PMCCNTR a 64-bit view (MRRC/MCRR), which with
// PMCR.LC set also overflows at 64 bits.
static inline uint64_t pmu_read_pmccntr64(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 0, %0, %1, c9" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}
static inline void pmu_write_pmccntr64(uint64_t v) {
    __asm__ volatile("mcrr p15, 0, %0, %1, c9" :: "r"((uint32_t)v), "r"((uint32_t)(v >> 32)));
}
#define PMU_PMCR_E (1u << 0)
#define PMU_PMCR_D (1u << 3)
#define PMU_PMCR_LC (1u << 6)
#define PMU_CYCLE_BIT (1u << 31)

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

// K12 (route 3): pseudo-NMI sampling. GIC priorities are Non-secure view
// values: the GIC-400 keeps 5 priority bits and a Non-secure write of v
// stores (v >> 1) | 0x80, so the Non-secure range has 16 levels in steps of
// 0x10, and an interrupt is signalled only if its priority is below the
// priority mask. PMU SPIs get the highest Non-secure priority; everything
// else gets ARM_NMI_PMR_MASKED, which the raised mask (also
// ARM_NMI_PMR_MASKED) blocks and the open mask (0xff) admits.
#define PROFILER_NMI_PRIO_PMU 0x00u
#define PROFILER_NMI_PRIO_NORMAL ARM_NMI_PMR_MASKED
#define PROFILER_GICD_IPRIORITY(irq) \
    ((volatile uint8_t *)(PROFILER_PMU_GICD_VIRT + 0x400 + (irq)))
#define PROFILER_GICC_PMR ((volatile uint32_t *)(BCM_GIC_BASE_VIRT + 0x2000 + 0x4))
#define PROFILER_GICD_ISENABLER(n) \
    ((volatile uint32_t *)(PROFILER_PMU_GICD_VIRT + 0x100 + 4 * (n)))
// The Non-secure view of this GIC-400 keeps 4 priority-mask bits: an open
// mask written as 0xff reads back as 0xf0.
#define PROFILER_NMI_PMR_BITS 0xf0u

// A priority has to read back only for interrupts that can reach LK.
// Unimplemented IDs (GIC-400 PPIs 16-24) and Secure-only ones read as zero
// and ignore writes; they are also never enabled in the Non-secure view.
static bool profiler_nmi_prio_ok(unsigned int irq, uint8_t want) {
    if (*PROFILER_GICD_IPRIORITY(irq) == want)
        return true;
    return !(*PROFILER_GICD_ISENABLER(irq / 32) & (1u << (irq % 32)));
}

// K10: the sampling timer gets the sampling priority too, so timer-mode
// samples also reach masked thread code in this mode.
static uint8_t profiler_nmi_prio_for(unsigned int irq) {
    return ((irq >= PROFILER_PMU_SPI_BASE && irq < PROFILER_PMU_SPI_BASE + 4) ||
            irq == PROFILER_TIMER_IRQ)
               ? PROFILER_NMI_PRIO_PMU : PROFILER_NMI_PRIO_NORMAL;
}

struct profiler_nmi_ctx {
    volatile uint32_t ok[SMP_MAX_CPUS];
    volatile uint32_t bad_irq[SMP_MAX_CPUS];   // first banked IRQ that did not read back, or 32
    volatile uint32_t bad_prio[SMP_MAX_CPUS];
    volatile uint32_t pmr[SMP_MAX_CPUS];       // PMR read back after opening
};

// Runs in an IPI on every core (CPSR.I set, core not masked by priority).
static void profiler_nmi_on_this_cpu(void *context) {
    struct profiler_nmi_ctx *ctx = (struct profiler_nmi_ctx *)context;
    uint cpu = arch_curr_cpu_num();
    bool ok = true;
    ctx->bad_irq[cpu] = 32;
    // SGIs and PPIs (0-31) have banked priorities: program them per core.
    for (unsigned int irq = 0; irq < 32; irq++) {
        *PROFILER_GICD_IPRIORITY(irq) = profiler_nmi_prio_for(irq);
        if (!profiler_nmi_prio_ok(irq, profiler_nmi_prio_for(irq)) && ctx->bad_irq[cpu] == 32) {
            ctx->bad_irq[cpu] = irq;
            ctx->bad_prio[cpu] = *PROFILER_GICD_IPRIORITY(irq);
            ok = false;
        }
    }
    *PROFILER_GICC_PMR = ARM_NMI_PMR_OPEN;
    ctx->pmr[cpu] = *PROFILER_GICC_PMR & 0xff;
    ok = ok && ctx->pmr[cpu] == (ARM_NMI_PMR_OPEN & PROFILER_NMI_PMR_BITS);
    ctx->ok[cpu] = ok;
    if (ok) {
        arm_nmi_masked[cpu] = 0;
        arm_nmi_mode[cpu] = 1;
    }
}

static void profiler_nmi_off_this_cpu(void *context) {
    (void)context;
    uint cpu = arch_curr_cpu_num();
    arm_nmi_mode[cpu] = 0;
    arm_nmi_masked[cpu] = 0;
    *PROFILER_GICC_PMR = ARM_NMI_PMR_OPEN;
}

static bool profiler_nmi_enable(void) {
    // Shared SPIs once; read back so a priority write the GIC ignored
    // (e.g. a Group 0 interrupt) refuses the mode instead of silently
    // leaving it unmaskable.
    for (unsigned int irq = 32; irq < MAX_INT; irq++)
        *PROFILER_GICD_IPRIORITY(irq) = profiler_nmi_prio_for(irq);
    for (unsigned int irq = 32; irq < MAX_INT; irq++) {
        if (!profiler_nmi_prio_ok(irq, profiler_nmi_prio_for(irq))) {
            printf("nmi: priority of IRQ %u reads back 0x%02x, expected 0x%02x -- not enabled\n",
                   irq, *PROFILER_GICD_IPRIORITY(irq), profiler_nmi_prio_for(irq));
            return false;
        }
    }
    arm_nmi_pmr = PROFILER_GICC_PMR;
    arm_nmi_any = 1;
    struct profiler_nmi_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_nmi_on_this_cpu, &ctx);
    bool all = true;
    for (int c = 0; c < SMP_MAX_CPUS; c++)
        all = all && ctx.ok[c];
    if (!all) {
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_nmi_off_this_cpu, NULL);
        arm_nmi_any = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++)
            printf("nmi: cpu%d banked IRQ %u reads 0x%02x, PMR reads 0x%02x\n", c,
                   ctx.bad_irq[c], ctx.bad_prio[c], ctx.pmr[c]);
        printf("nmi: per-core priority/PMR setup did not read back -- not enabled\n");
        return false;
    }
    return true;
}

// K7: `profiler stat` counting state. Up to six event counters (five when
// `pmustart` owns counter 0) plus the cycle counter, on every core at once,
// so a command that migrates or runs threads on several cores is counted in
// full. Event counters are 32 bits; their overflow interrupt (the same
// per-core SPIs as PMU sampling) extends them to 64. The cycle counter uses
// its native 64-bit mode.
#define PROFILER_STAT_MAX 6
struct profiler_stat_cfg {
    uint32_t n;          // event counters in use
    uint32_t first;      // first event counter index (1 if sampling owns 0)
    uint32_t event[PROFILER_STAT_MAX];
};
static struct profiler_stat_cfg profiler_stat_cfg;
volatile uint32_t profiler_stat_mask[SMP_MAX_CPUS];   // counters `stat` owns now
static uint32_t profiler_stat_ovf[SMP_MAX_CPUS][PROFILER_STAT_MAX];
static uint64_t profiler_stat_val[SMP_MAX_CPUS][PROFILER_STAT_MAX];
static uint64_t profiler_stat_cyc[SMP_MAX_CPUS];
static uint32_t profiler_stat_pmcr[SMP_MAX_CPUS];
// K15: counters (and the cycle counter) that were enabled before `stat`;
// other code -- bolt-aarch32's bolt_bench timing -- relies on the cycle
// counter staying enabled, so `stat` gives back exactly this state.
static uint32_t profiler_stat_cnten[SMP_MAX_CPUS];
static volatile uint32_t profiler_stat_busy;

// From the PMU interrupt, or with the counters frozen: count the overflows
// of `stat`'s counters among `ovs` and clear exactly those flags.
static void profiler_stat_overflow(uint cpu, uint32_t ovs) {
    uint32_t mine = ovs & profiler_stat_mask[cpu];
    if (!mine)
        return;
    for (uint32_t i = 0; i < profiler_stat_cfg.n; i++)
        if (mine & (1u << (profiler_stat_cfg.first + i)))
            profiler_stat_ovf[cpu][i]++;
    pmu_write_pmovsr(mine);
}

// Runs in an IPI on every core. The counters start last, after all setup.
static void profiler_stat_arm_this_cpu(void *context) {
    (void)context;
    uint cpu = arch_curr_cpu_num();
    const struct profiler_stat_cfg *c = &profiler_stat_cfg;
    uint32_t mask = 0;
    uint32_t sel = pmu_read_pmselr();
    profiler_stat_cnten[cpu] = pmu_read_pmcntenset();
    for (uint32_t i = 0; i < c->n; i++) {
        uint32_t ctr = c->first + i;
        pmu_write_pmcntenclr(1u << ctr);
        pmu_write_pmselr(ctr);
        pmu_write_pmxevtyper(c->event[i]);   // no filtering: EL0 and EL1, both states
        pmu_write_pmxevcntr(0);
        mask |= 1u << ctr;
        profiler_stat_ovf[cpu][i] = 0;
    }
    pmu_write_pmselr(sel);
    pmu_write_pmovsr(mask | PMU_CYCLE_BIT);
    profiler_stat_mask[cpu] = mask;
    pmu_write_pmintenset(mask);
    uint32_t pmcr = pmu_read_pmcr();
    profiler_stat_pmcr[cpu] = pmcr;
    pmu_write_pmcntenclr(PMU_CYCLE_BIT);
    pmu_write_pmccntr64(0);
    pmu_write_pmcr((pmcr | PMU_PMCR_E | PMU_PMCR_LC) & ~PMU_PMCR_D);
    pmu_write_pmcntenset(mask | PMU_CYCLE_BIT);
}

// Runs in an IPI on every core: freeze first, then read, then give the
// counters back (PMCR as it was, so PMU sampling keeps running if armed).
static void profiler_stat_read_this_cpu(void *context) {
    (void)context;
    uint cpu = arch_curr_cpu_num();
    const struct profiler_stat_cfg *c = &profiler_stat_cfg;
    uint32_t mask = profiler_stat_mask[cpu];
    pmu_write_pmcntenclr(mask | PMU_CYCLE_BIT);
    profiler_stat_cyc[cpu] = pmu_read_pmccntr64();
    profiler_stat_overflow(cpu, pmu_read_pmovsr());   // overflow not yet taken as an IRQ
    uint32_t sel = pmu_read_pmselr();
    for (uint32_t i = 0; i < c->n; i++) {
        pmu_write_pmselr(c->first + i);
        profiler_stat_val[cpu][i] = ((uint64_t)profiler_stat_ovf[cpu][i] << 32) | pmu_read_pmxevcntr();
    }
    pmu_write_pmselr(sel);
    pmu_write_pmintenclr(mask);
    profiler_stat_mask[cpu] = 0;
    pmu_write_pmcr(profiler_stat_pmcr[cpu]);
    // K15: re-enable what was enabled before (the read above disabled it)
    pmu_write_pmcntenset(profiler_stat_cnten[cpu] & (mask | PMU_CYCLE_BIT));
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
// K1: how many of those bytes were really copied. The copy stops at the top
// of the stack the interrupted SP is on, so a sample from a thread with less
// than PROFILER_STACK_CAPTURE_BYTES above its SP no longer reads past its
// stack allocation; the rest of the slot is zero. 0 means SP was not on a
// known stack (e.g. mid context switch) and nothing was copied.
uint8_t profiler_slen_buf[SMP_MAX_CPUS][PROFILER_BUF_SIZE];
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

// K10: sampling periods whose whole length passed before the previous
// sample could be taken (the core was masked throughout), per core.
volatile uint32_t profiler_timer_missed[SMP_MAX_CPUS];

#if BCM2711
// K10: timer-mode sampling on the per-core virtual timer. Time is cut into
// periods of a fixed length, counted from when sampling was armed, and each
// period gets exactly one deadline at a random offset inside it (stratified
// random sampling). The grid never moves: a sample delayed by masked code
// does not shift the later deadlines, so the sample share of any code
// equals its share of time for every workload, periodic or not; a fixed
// offset would still alias with a workload whose period divides the
// sampling period. A deadline that falls inside masked code is taken at the
// unmask, as before, with its delay in `lat`.
static uint32_t profiler_timer_period;                  // CNTVCT ticks
static uint64_t profiler_timer_stratum[SMP_MAX_CPUS];   // start of the current period
static uint32_t profiler_timer_rng[SMP_MAX_CPUS];

static inline uint64_t profiler_cntvct(void) {
    uint32_t lo, hi;
    __asm__ volatile("isb; mrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t profiler_cntv_cval(void) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p15, 3, %0, %1, c14" : "=r"(lo), "=r"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static inline void profiler_cntv_ctl(uint32_t ctl) {
    __asm__ volatile("mcr p15, 0, %0, c14, c3, 1; isb" : : "r"(ctl) : "memory");
}

// A new compare value takes the timer output low at once (the condition is
// evaluated continuously), so the level interrupt is gone before the EOI.
static inline void profiler_cntv_arm(uint64_t cval) {
    __asm__ volatile("mcrr p15, 3, %0, %1, c14" : : "r"((uint32_t)cval),
                     "r"((uint32_t)(cval >> 32)) : "memory");
    profiler_cntv_ctl(1);   // enabled, not masked
}

static uint32_t profiler_timer_offset(uint cpu) {
    uint32_t x = profiler_timer_rng[cpu];   // xorshift32
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    profiler_timer_rng[cpu] = x;
    return x % profiler_timer_period;
}

// Runs in an IPI on every core.
static void profiler_timer_arm_this_cpu(void *context) {
    (void)context;
    uint cpu = arch_curr_cpu_num();
    uint64_t now = profiler_cntvct();
    uint32_t seed = (uint32_t)now ^ (0x9e3779b9u * (cpu + 1));
    profiler_timer_rng[cpu] = seed ? seed : 1;
    profiler_timer_stratum[cpu] = now;
    profiler_cntv_arm(now + profiler_timer_offset(cpu));
    unmask_interrupt(PROFILER_TIMER_IRQ);   // PPI: enables it on this core only
}

static void profiler_timer_disarm_this_cpu(void *context) {
    (void)context;
    profiler_cntv_ctl(0);
    mask_interrupt(PROFILER_TIMER_IRQ);
}

// Called for each timer sample, `now` being when it was taken: move to the
// next period and arm its deadline. Periods that ended while this core was
// masked get no sample and are counted.
static void profiler_timer_next(uint cpu, uint64_t now) {
    uint64_t period = profiler_timer_period;
    uint64_t next = profiler_timer_stratum[cpu] + period;
    if (now >= next + period) {
        uint64_t whole = (now - next) / period;
        profiler_timer_missed[cpu] += (uint32_t)whole;
        next += whole * period;
    }
    profiler_timer_stratum[cpu] = next;
    profiler_cntv_arm(next + profiler_timer_offset(cpu));
}
#endif

// K1: bytes that can be read upward from the interrupted SP without leaving
// its stack. Created threads record their stack (thread_t::stack/stack_size);
// idle threads and the bootstrap thread record none and run on the per-core
// boot stacks (abort_stack, ARCH_DEFAULT_STACK_SIZE per core, core n's top at
// abort_stack + (n + 1) * ARCH_DEFAULT_STACK_SIZE, see arch/arm/arm/start.S).
// An SP outside the stack that is expected for the current thread -- for
// example in the window of a context switch where the current-thread pointer
// already names the next thread -- gets 0: no bytes are copied.
extern uint8_t abort_stack[ARCH_DEFAULT_STACK_SIZE * SMP_MAX_CPUS];

static uint32_t profiler_stack_room(uint cpu, uintptr_t sp, const thread_t *t) {
    uintptr_t lo, hi;
    if (t && t->stack && t->stack_size) {
        lo = (uintptr_t)t->stack;
        hi = lo + t->stack_size;
    } else {
        lo = (uintptr_t)abort_stack + (uintptr_t)cpu * ARCH_DEFAULT_STACK_SIZE;
        hi = lo + ARCH_DEFAULT_STACK_SIZE;
    }
    if (sp < lo || sp > hi)
        return 0;
    uintptr_t room = hi - sp;
    return room < PROFILER_STACK_CAPTURE_BYTES ? (uint32_t)room : PROFILER_STACK_CAPTURE_BYTES;
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
    // K7: the same interrupt also signals `stat`'s counter overflows, so
    // only counter 0's flag means a sample is due.
    uint32_t ovs = is_pmu_vector ? pmu_read_pmovsr() : 0;
    if (is_pmu_vector)
        profiler_stat_overflow(cpu, ovs);
    bool is_pmu_overflow = is_pmu_vector && profiler_pmu_enabled[cpu] && (ovs & 1u);
#else
    bool is_pmu_vector = false;
    bool is_pmu_overflow = false;
#endif
    if (!is_timer_tick && !is_pmu_overflow) {
#if BCM2711
        // K10: the sampling timer is level-triggered too; one that fires
        // after `stop` (before the disarm IPI reached this core) is
        // switched off here instead of re-raising itself.
        if (vector == PROFILER_TIMER_IRQ)
            profiler_cntv_ctl(0);
        // Review finding #1: a real PMU-overflow vector for THIS core
        // must always be acknowledged, even when profiler_pmu_enabled[cpu]
        // is false -- it's level-triggered, so an unacknowledged one
        // re-raises itself the instant this handler returns and hangs
        // the core in a pure interrupt storm. See
        // profiler_pmu_arm_this_cpu/profiler_pmu_disarm_this_cpu's own
        // comment for the exact scenario this fixes, confirmed on real
        // hardware.
        if (is_pmu_vector && (ovs & 1u) && !(profiler_stat_mask[cpu] & 1u)) {
            pmu_write_pmovsr(1u << 0);
            profiler_pmu_reload_counter0(profiler_pmu_reload);
        }
#endif
        return;
    }

#if BCM2711
    // K10: no deadline has passed (the interrupt was seen again before the
    // re-arm took the timer output low): nothing to sample.
    if (is_timer_tick && !is_pmu_overflow && profiler_cntvct() < profiler_cntv_cval())
        return;
#endif

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
    thread_t *cur = get_current_thread();
    profiler_tid_buf[cpu][idx] = (uint32_t)(uintptr_t)cur;
    uint32_t room = profiler_stack_room(cpu, (uintptr_t)(frame + 1), cur);
    memcpy(profiler_stack_buf[cpu][idx], (const void *)(frame + 1), room);
    memset(profiler_stack_buf[cpu][idx] + room, 0, PROFILER_STACK_CAPTURE_BYTES - room);
    profiler_slen_buf[cpu][idx] = (uint8_t)room;
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
#if BCM2711
        uint64_t vnow = profiler_cntvct();
        uint64_t deadline = profiler_cntv_cval();
        lat = vnow >= deadline ? profiler_sat32(vnow - deadline) : 0;
        profiler_timer_next(cpu, vnow);
#else
        uint64_t deadline = profiler_cntp_cval();
        lat = now_count >= deadline ? profiler_sat32(now_count - deadline) : 0;
#endif
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

// K3 ground-truth workload: time spent in LK's hand-written memcpy and
// memset (assembly without CFI), each reached from its own C caller. A
// report should attribute memcpy samples to profiler_copy_a and memset
// samples to profiler_fill_b. The length comes from a volatile so the
// compiler cannot replace the calls with inline code.
static uint8_t profiler_mem_src[4096], profiler_mem_dst[4096];
static volatile uint32_t profiler_mem_len = sizeof(profiler_mem_dst);

__NO_INLINE static void profiler_copy_a(uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++)
        memcpy(profiler_mem_dst, profiler_mem_src, profiler_mem_len);
}

__NO_INLINE static void profiler_fill_b(uint32_t iters) {
    for (uint32_t i = 0; i < iters; i++)
        memset(profiler_mem_dst, (int)i, profiler_mem_len);
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
                                          uint32_t lat, uint32_t msite, uint32_t mgap,
                                          uint32_t slen) {
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
    c = profiler_fnv(c, slen);   // K1
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

// K2: image identity. The read-only part of the image -- code, unwind index,
// rodata and LK's constant tables (lk_init, commands, apps) -- spans
// [_start, __rodata_end) and is never written at run time, so an FNV-1a hash
// of those bytes identifies the build. The host recomputes it from the ELF it
// is given and refuses a mismatched ELF.
extern const uint8_t _start[];
extern const uint8_t __rodata_end[];

// K2: capture session. A session starts at `profiler clear` (or at boot) and
// may be dumped several times; every dump says which session it belongs to,
// which sampling modes and PMU configuration produced it, and how many
// samples each core took, kept, overwrote and lost, so a log holding several
// dumps or a damaged transfer can be interpreted without operator notes.
#define PROFILER_DUMP_FORMAT 3u   // 3: K10 timer period and missed periods
#define PROFILER_MODE_TIMER (1u << 0)
#define PROFILER_MODE_PMU (1u << 1)
static uint64_t profiler_run_id;       // CNTPCT when the session started
static uint32_t profiler_dump_no;      // dumps of this session so far
static uint32_t profiler_modes_used;   // PROFILER_MODE_* armed in this session
static uint32_t profiler_pmu_event_used;
static uint32_t profiler_pmu_period_used;
static uint32_t profiler_pmu_config_mixed;  // a second, different PMU config
static uint32_t profiler_timer_period_us;   // K10: timer-mode period (0: none)
static uint32_t profiler_timer_mixed;       // a second, different timer period

static void profiler_session_start(void) {
    profiler_run_id = profiler_cntpct();
    profiler_dump_no = 0;
    profiler_modes_used = 0;
    profiler_pmu_event_used = 0;
    profiler_pmu_period_used = 0;
    profiler_pmu_config_mixed = 0;
    profiler_timer_period_us = 0;
    profiler_timer_mixed = 0;
}

// K11 feasibility: what this core says about PC sampling through the
// external debug interface (EDPCSR), from CP14 registers that are always
// readable at PL1: whether PC sampling is implemented, whether non-invasive
// debug is allowed in each security state, and where the debug ROM table
// is, if the CPU can reach its own debug registers through memory.
#define PROFILER_MRC14(op1, crn, crm, op2) ({ uint32_t v; \
    __asm__ volatile("mrc p14, " #op1 ", %0, " #crn ", " #crm ", " #op2 : "=r"(v)); v; })

static void profiler_dbginfo(void) {
    uint32_t midr, lo, hi;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 0" : "=r"(midr));
    uint32_t didr = PROFILER_MRC14(0, c0, c0, 0);
    uint32_t devid = PROFILER_MRC14(0, c7, c2, 7);
    uint32_t devid1 = PROFILER_MRC14(0, c7, c1, 7);
    uint32_t auth = PROFILER_MRC14(0, c7, c14, 6);
    uint32_t oslsr = PROFILER_MRC14(0, c1, c1, 4);
    uint32_t prcr = PROFILER_MRC14(0, c1, c4, 4);
    uint32_t dscr = PROFILER_MRC14(0, c0, c1, 0);
    __asm__ volatile("mrrc p14, 0, %0, %1, c1" : "=r"(lo), "=r"(hi));
    uint64_t drar = ((uint64_t)hi << 32) | lo;
    printf("dbginfo: cpu%u MIDR=%08x DBGDIDR=%08x DBGDEVID=%08x DBGDEVID1=%08x\n",
           arch_curr_cpu_num(), midr, didr, devid, devid1);
    printf("dbginfo: PC sampling (DBGDEVID.PCSample)=%u: %s; PCSR offset (DBGDEVID1)=%u\n",
           devid & 0xf,
           (devid & 0xf) == 0 ? "not implemented" :
           (devid & 0xf) == 3 ? "EDPCSR+EDCIDSR+EDVIDSR" : "EDPCSR (partial)",
           devid1 & 0xf);
    printf("dbginfo: DBGAUTHSTATUS=%08x: non-invasive NS %s, S %s; invasive NS %s, S %s\n", auth,
           (auth & 0xc) == 0xc ? "enabled" : (auth & 0xc) == 0x8 ? "disabled" : "n/i",
           (auth & 0xc0) == 0xc0 ? "enabled" : (auth & 0xc0) == 0x80 ? "disabled" : "n/i",
           (auth & 0x3) == 0x3 ? "enabled" : (auth & 0x3) == 0x2 ? "disabled" : "n/i",
           (auth & 0x30) == 0x30 ? "enabled" : (auth & 0x30) == 0x20 ? "disabled" : "n/i");
    printf("dbginfo: DBGOSLSR=%08x (OS lock %s) DBGPRCR=%08x DBGDSCRint=%08x\n", oslsr,
           (oslsr & 0x2) ? "locked" : "unlocked", prcr, dscr);
    printf("dbginfo: DBGDRAR=%016llx: ROM table %s at %016llx\n", (unsigned long long)drar,
           (drar & 3) == 3 ? "valid" : (drar & 3) == 0 ? "NOT VALID (no memory-mapped route)"
                                                        : "reserved",
           (unsigned long long)(drar & ~0xfffull));
}

#if BCM2711
// K11 feasibility: walk the CoreSight ROM table DBGDRAR names and report
// each component; for the cores' debug blocks, read the PC-sample
// registers from this core. Only the BCM2711 low-peripheral window
// (0xfc000000-0xffffffff, mapped as device memory) is reachable.
#define PROFILER_DBG_MAX 16
static uint32_t profiler_dbg_core_pa[PROFILER_DBG_MAX];
static uint32_t profiler_dbg_core_aff[PROFILER_DBG_MAX];
static uint32_t profiler_dbg_cores;

static volatile uint32_t *profiler_dbg_reg(uint32_t pa, uint32_t off) {
    return (volatile uint32_t *)(BCM2711_LOW_PERIPH_VIRT + (pa - BCM2711_LOW_PERIPH_PHYS) + off);
}

static bool profiler_dbg_reachable(uint32_t pa) {
    return pa >= BCM2711_LOW_PERIPH_PHYS && pa <= 0xfffff000u;
}

static void profiler_dbg_walk(uint32_t rom, int depth, bool print) {
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t e = *profiler_dbg_reg(rom, 4 * i);
        if (e == 0)
            break;
        uint32_t pa = rom + (e & 0xfffff000u);
        if (!(e & 1)) {
            if (print)
                printf("dbgrom: %*s[%u] %08x not present\n", depth * 2, "", i, e);
            continue;
        }
        if (!profiler_dbg_reachable(pa)) {
            if (print)
                printf("dbgrom: %*s[%u] %08x -> %08x outside the mapped window\n", depth * 2, "",
                       i, e, pa);
            continue;
        }
        uint32_t cidr1 = *profiler_dbg_reg(pa, 0xff4);
        uint32_t cls = (cidr1 >> 4) & 0xf;
        uint32_t part = (*profiler_dbg_reg(pa, 0xfe0) & 0xff) |
                        ((*profiler_dbg_reg(pa, 0xfe4) & 0xf) << 8);
        uint32_t devarch = *profiler_dbg_reg(pa, 0xfbc);
        uint32_t devtype = *profiler_dbg_reg(pa, 0xfcc) & 0xff;
        uint32_t aff = *profiler_dbg_reg(pa, 0xfa8);
        if (print)
            printf("dbgrom: %*s[%u] %08x class %x part %03x devarch %08x devtype %02x aff %08x\n",
                   depth * 2, "", i, pa, cls, part, devarch, devtype, aff);
        if (cls == 1 && depth < 2)
            profiler_dbg_walk(pa, depth + 1, print);
        if (cls == 9 && devtype == 0x15 && profiler_dbg_cores < PROFILER_DBG_MAX) {
            profiler_dbg_core_pa[profiler_dbg_cores] = pa;
            profiler_dbg_core_aff[profiler_dbg_cores] = aff;
            profiler_dbg_cores++;
        }
    }
}

static bool profiler_dbg_find(bool print) {
    uint32_t lo, hi;
    __asm__ volatile("mrrc p14, 0, %0, %1, c1" : "=r"(lo), "=r"(hi));
    uint32_t rom = lo & ~0xfffu;
    profiler_dbg_cores = 0;
    if ((lo & 3) != 3 || hi != 0 || !profiler_dbg_reachable(rom)) {
        printf("dbgrom: no reachable ROM table (DBGDRAR %08x%08x)\n", hi, lo);
        return false;
    }
    if (print)
        printf("dbgrom: ROM table %08x, CIDR %08x %08x\n", rom, *profiler_dbg_reg(rom, 0xff0),
               *profiler_dbg_reg(rom, 0xff4));
    profiler_dbg_walk(rom, 0, print);
    return true;
}

static void profiler_dbg_unlock_os_this_cpu(void *context) {
    (void)context;
    __asm__ volatile("mcr p14, 0, %0, c1, c0, 4; isb" : : "r"(0) : "memory");   // DBGOSLAR
}

static void profiler_dbgpcsr(void) {
    if (!profiler_dbg_find(false) || !profiler_dbg_cores) {
        printf("dbgpcsr: no core debug blocks found\n");
        return;
    }
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_dbg_unlock_os_this_cpu, NULL);
    printf("dbgpcsr: OS lock cleared on all cores; reading from cpu%u\n", arch_curr_cpu_num());
    for (uint32_t c = 0; c < profiler_dbg_cores; c++) {
        uint32_t pa = profiler_dbg_core_pa[c];
        printf("dbgpcsr: %08x aff %08x EDDEVID %08x EDPRSR %08x EDAUTHSTATUS %08x EDLSR %08x\n",
               pa, profiler_dbg_core_aff[c], *profiler_dbg_reg(pa, 0xfc8),
               *profiler_dbg_reg(pa, 0x314), *profiler_dbg_reg(pa, 0xfb8),
               *profiler_dbg_reg(pa, 0xfb4));
        for (int k = 0; k < 4; k++) {
            uint32_t pcl = *profiler_dbg_reg(pa, 0x0a0);   // EDPCSRlo: takes the sample
            uint32_t cid = *profiler_dbg_reg(pa, 0x0a4);
            uint32_t vid = *profiler_dbg_reg(pa, 0x0a8);
            uint32_t pch = *profiler_dbg_reg(pa, 0x0ac);
            printf("dbgpcsr:   EDPCSR %08x%08x EDCIDSR %08x EDVIDSR %08x\n", pch, pcl, cid, vid);
        }
    }
}
#endif

// The USB-serial link loses data mostly in the first few KB after the line
// has been idle (records 0-11 of a dump were the most often lost). Padding
// lines, which the host ignores, take that hit instead of the dump.
static void profiler_link_warmup(void) {
    static const char pad[] = "PAD ................................................................"
                              "...............................................................\n";
    for (int i = 0; i < 32; i++)
        printf("%s", pad);
}

static uint32_t profiler_image_hash(void) {
    uint32_t h = 0x811c9dc5u;
    for (const uint8_t *p = _start; p < __rodata_end; p++)
        h = (h ^ *p) * 16777619u;
    return h;
}

#if BCM2711
// K7: names for the common ARMv8 common events (others print as hex).
static const char *profiler_event_name(uint32_t ev) {
    switch (ev) {
    case 0x01: return "L1I_CACHE_REFILL";
    case 0x02: return "L1I_TLB_REFILL";
    case 0x03: return "L1D_CACHE_REFILL";
    case 0x04: return "L1D_CACHE";
    case 0x05: return "L1D_TLB_REFILL";
    case 0x08: return "INST_RETIRED";
    case 0x09: return "EXC_TAKEN";
    case 0x0a: return "EXC_RETURN";
    case 0x10: return "BR_MIS_PRED";
    case 0x11: return "CPU_CYCLES";
    case 0x12: return "BR_PRED";
    case 0x13: return "MEM_ACCESS";
    case 0x14: return "L1I_CACHE";
    case 0x15: return "L1D_CACHE_WB";
    case 0x16: return "L2D_CACHE";
    case 0x17: return "L2D_CACHE_REFILL";
    case 0x18: return "L2D_CACHE_WB";
    case 0x19: return "BUS_ACCESS";
    case 0x1b: return "INST_SPEC";
    case 0x1d: return "BUS_CYCLES";
    default: return NULL;
    }
}

// Default event set, most useful first: with sampling armed only the
// first five fit.
static const uint32_t profiler_stat_default[PROFILER_STAT_MAX] = {
    0x08, 0x03, 0x04, 0x10, 0x12, 0x17,
};

static int profiler_stat_find(uint32_t ev) {
    for (uint32_t i = 0; i < profiler_stat_cfg.n; i++)
        if (profiler_stat_cfg.event[i] == ev)
            return (int)i;
    return -1;
}

// Output lines are assembled first and printed with one printf each. The
// first K7 build wrote each table line in many fragments and the host lost
// large parts of them; whole-line writes made the tables almost clean. Not
// a general fix for link loss: dump records lose the same ~0.2% either way
// (K9 measurement).
struct profiler_line {
    char buf[200];
    size_t len;
};

static void profiler_line_add(struct profiler_line *l, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l->buf + l->len, sizeof(l->buf) - l->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        l->len = l->len + (size_t)n < sizeof(l->buf) ? l->len + (size_t)n : sizeof(l->buf) - 1;
}

// num/den with three decimals, integer only (no FPU on the target).
static void profiler_line_ratio(struct profiler_line *l, const char *label, uint64_t num,
                                uint64_t den, uint32_t scale) {
    if (!den)
        return;
    uint64_t milli = (num * scale * 1000u) / den;
    profiler_line_add(l, " %s %llu.%03llu", label, (unsigned long long)(milli / 1000),
                      (unsigned long long)(milli % 1000));
}

static void profiler_stat_derived(const char *who, uint64_t cyc, const uint64_t *v) {
    int inst = profiler_stat_find(0x08), l1 = profiler_stat_find(0x04),
        l1r = profiler_stat_find(0x03), br = profiler_stat_find(0x12),
        brm = profiler_stat_find(0x10), l2r = profiler_stat_find(0x17);
    struct profiler_line l = { .len = 0 };
    profiler_line_add(&l, "stat: %-4s", who);
    if (inst >= 0)
        profiler_line_ratio(&l, "IPC", v[inst], cyc, 1);
    if (l1 >= 0 && l1r >= 0)
        profiler_line_ratio(&l, "L1D-miss%", v[l1r], v[l1], 100);
    if (br >= 0 && brm >= 0)
        profiler_line_ratio(&l, "br-mispred%", v[brm], v[br], 100);
    if (inst >= 0 && l2r >= 0)
        profiler_line_ratio(&l, "L2D-refill/kinst", v[l2r], v[inst], 1000);
    printf("%s\n", l.buf);
}

// `profiler stat [-e ev,...] [command ...]` / `profiler stat [iters]`.
static int profiler_stat(int argc, const console_cmd_args *argv) {
    int a = 2;
    uint32_t events[PROFILER_STAT_MAX];
    uint32_t nev = 0;
    bool user_events = false;
    if (argc > a && !strcmp(argv[a].str, "-e")) {
        if (argc <= a + 1) {
            printf("usage: profiler stat [-e ev,ev,...] [command ...] | profiler stat [iters]\n");
            return -1;
        }
        const char *p = argv[a + 1].str;
        while (*p) {
            char *end;
            uint32_t ev = (uint32_t)strtoul(p, &end, 0);
            if (end == p || nev == PROFILER_STAT_MAX || ev > 0x3ff) {
                printf("stat: bad event list '%s' (at most %d events, 0x0..0x3ff, comma separated)\n",
                       argv[a + 1].str, PROFILER_STAT_MAX);
                return -1;
            }
            events[nev++] = ev;
            p = *end == ',' ? end + 1 : end;
            if (*end && *end != ',') {
                printf("stat: bad event list '%s'\n", argv[a + 1].str);
                return -1;
            }
        }
        user_events = true;
        a += 2;
    }
    if (!user_events) {
        for (nev = 0; nev < PROFILER_STAT_MAX; nev++)
            events[nev] = profiler_stat_default[nev];
    }

    // Counter 0 belongs to PMU sampling while it is armed on any core.
    bool sampling = false;
    for (int c = 0; c < SMP_MAX_CPUS; c++)
        sampling = sampling || profiler_pmu_enabled[c];
    uint32_t avail = ((pmu_read_pmcr() >> 11) & 0x1f) - (sampling ? 1 : 0);
    if (avail > PROFILER_STAT_MAX)
        avail = PROFILER_STAT_MAX;
    if (nev > avail) {
        if (user_events) {
            printf("stat: %u events requested, %u counters free%s\n", nev, avail,
                   sampling ? " (counter 0 is PMU sampling's)" : "");
            return -1;
        }
        nev = avail;
    }
    uint32_t ceid[2] = { pmu_read_pmceid0(), pmu_read_pmceid1() };
    for (uint32_t i = 0; i < nev; i++)
        if (events[i] < 64 && !(ceid[events[i] / 32] & (1u << (events[i] % 32))))
            printf("stat: warning: event 0x%x is not marked implemented (PMCEID) -- counting anyway\n",
                   events[i]);

    // The command line to run, or the built-in workload.
    bool builtin = argc <= a || (argv[a].str[0] >= '0' && argv[a].str[0] <= '9');
    uint32_t iters = builtin && argc > a ? (uint32_t)argv[a].u : 5000000;
    char cmd[256];
    size_t len = 0;
    cmd[0] = 0;
    for (int i = a; !builtin && i < argc; i++) {
        size_t l = strlen(argv[i].str);
        if (len + l + 2 > sizeof(cmd)) {
            printf("stat: command line too long\n");
            return -1;
        }
        if (len)
            cmd[len++] = ' ';
        memcpy(cmd + len, argv[i].str, l + 1);
        len += l;
    }
    if (!builtin && !strncmp(cmd, "profiler stat", 13)) {
        printf("stat: cannot count itself\n");
        return -1;
    }

    profiler_stat_busy = 1;
    profiler_stat_cfg.n = nev;
    profiler_stat_cfg.first = sampling ? 1 : 0;
    for (uint32_t i = 0; i < nev; i++)
        profiler_stat_cfg.event[i] = events[i];
    profiler_pmu_route_and_unmask();
    printf("stat: counting %s on all %d cores ...\n", builtin ? "the built-in workload" : cmd,
           SMP_MAX_CPUS);

    // Nothing prints between arm and read except the command itself (K4).
    uint64_t t0 = profiler_cntpct();
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_stat_arm_this_cpu, NULL);
    int rc = 0;
    if (builtin)
        profiler_workload_a(iters);
    else
        rc = console_run_script_locked(NULL, cmd);
    mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_stat_read_this_cpu, NULL);
    uint64_t t1 = profiler_cntpct();
    profiler_stat_busy = 0;

    uint64_t us = ((t1 - t0) * 1000000u) / profiler_cntfrq();
    if (builtin)
        printf("stat: built-in workload, %u iters, %llu.%03llu ms\n", iters,
               (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
    else
        printf("stat: '%s' returned %d, %llu.%03llu ms\n", cmd, rc,
               (unsigned long long)(us / 1000), (unsigned long long)(us % 1000));
    struct profiler_line l = { .len = 0 };
    profiler_line_add(&l, "stat: %-4s %16s", "cpu", "cycles");
    for (uint32_t i = 0; i < nev; i++) {
        const char *name = profiler_event_name(events[i]);
        if (name)
            profiler_line_add(&l, " %16s", name);
        else
            profiler_line_add(&l, "        event%#05x", events[i]);
    }
    printf("%s\n", l.buf);
    uint64_t sum_cyc = 0, sum[PROFILER_STAT_MAX] = { 0 };
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        l.len = 0;
        profiler_line_add(&l, "stat: cpu%d %16llu", c, (unsigned long long)profiler_stat_cyc[c]);
        sum_cyc += profiler_stat_cyc[c];
        for (uint32_t i = 0; i < nev; i++) {
            profiler_line_add(&l, " %16llu", (unsigned long long)profiler_stat_val[c][i]);
            sum[i] += profiler_stat_val[c][i];
        }
        printf("%s\n", l.buf);
    }
    l.len = 0;
    profiler_line_add(&l, "stat: %-4s %16llu", "all", (unsigned long long)sum_cyc);
    for (uint32_t i = 0; i < nev; i++)
        profiler_line_add(&l, " %16llu", (unsigned long long)sum[i]);
    printf("%s\n", l.buf);
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        char who[8];
        snprintf(who, sizeof(who), "cpu%d", c);
        profiler_stat_derived(who, profiler_stat_cyc[c], profiler_stat_val[c]);
    }
    profiler_stat_derived("all", sum_cyc, sum);
    printf("stat: done\n");
    return rc;
}
#endif

// K8: scheduler events. LK's kernel/thread.c calls lkperf_sched_switch()
// right before every context switch and lkperf_sched_wakeup() wherever a
// thread becomes ready again (overlay 0015), both with thread_lock held and
// interrupts disabled. Each core appends to its own ring (oldest entries
// overwritten, counted); the host merges the rings by CNTPCT and derives
// run, blocked and sleep intervals, wakeup latency, and a Perfetto
// timeline. A low-priority thread adds the measured ARM clock and the
// firmware's throttling flags from the VideoCore mailbox (on the Pi, LK
// itself never changes the clock).
#define PROFILER_SCHED_BUF 8192
#define PROFILER_SCHED_SWITCH 1u
#define PROFILER_SCHED_WAKE 2u
#define PROFILER_SCHED_FREQ 3u
#define PROFILER_SCHED_NAME 4u    // a: thread, b: priority, c: flags, name in the event
#define PROFILER_SCHED_NAMES 256
struct profiler_sched_ev {
    uint64_t ts;      // CNTPCT
    uint32_t a;       // switch: old thread; wake: woken thread; freq: measured ARM Hz
    uint32_t b;       // switch: new thread; wake: current (waker) thread; freq: set ARM Hz
    uint32_t c;       // switch: wait queue the old thread blocks on; wake: wait queue; freq: throttled flags
    int32_t err;      // wake: wait result handed to the thread (ERR_TIMED_OUT = timeout)
    uint32_t type_state;   // type | state << 8 (switch: state the old thread leaves in; wake: from)
    char name[32];         // NAME events only
};
static struct profiler_sched_ev profiler_sched_buf[SMP_MAX_CPUS][PROFILER_SCHED_BUF];
static volatile uint32_t profiler_sched_total[SMP_MAX_CPUS];
static volatile uint32_t profiler_sched_on;
static uint64_t profiler_sched_start, profiler_sched_stop;
// Names of threads seen at a switch, so threads that exited before the dump
// still have one. Written only under thread_lock (the hooks hold it).
struct profiler_sched_name {
    uint32_t tid;
    int32_t prio;
    uint32_t flags;
    char name[32];
};
static struct profiler_sched_name profiler_sched_names[PROFILER_SCHED_NAMES];

static void profiler_sched_put(uint32_t type, uint32_t state, uint32_t a, uint32_t b,
                               uint32_t c, int32_t err) {
    uint cpu = arch_curr_cpu_num();
    uint32_t n = profiler_sched_total[cpu];
    struct profiler_sched_ev *e = &profiler_sched_buf[cpu][n % PROFILER_SCHED_BUF];
    e->ts = profiler_cntpct();
    e->a = a;
    e->b = b;
    e->c = c;
    e->err = err;
    e->type_state = type | (state << 8);
    profiler_sched_total[cpu] = n + 1;
}

// A thread_t address is reused once its thread exits, so names travel in
// the event stream: a NAME event whenever a thread is seen with a name or
// priority the cache does not hold for that address. The host starts a new
// thread identity after each exit and takes its name from these events.
static void profiler_sched_name_seen(const thread_t *t) {
    struct profiler_sched_name *n =
        &profiler_sched_names[((uint32_t)(uintptr_t)t >> 4) % PROFILER_SCHED_NAMES];
    if (n->tid == (uint32_t)(uintptr_t)t && n->prio == t->priority &&
        !strncmp(n->name, t->name, sizeof(n->name) - 1))
        return;
    n->tid = (uint32_t)(uintptr_t)t;
    n->prio = t->priority;
    n->flags = t->flags;
    memcpy(n->name, t->name, sizeof(n->name));
    n->name[sizeof(n->name) - 1] = 0;
    uint cpu = arch_curr_cpu_num();
    uint32_t i = profiler_sched_total[cpu];
    profiler_sched_put(PROFILER_SCHED_NAME, 0, n->tid, (uint32_t)n->prio, n->flags, 0);
    memcpy(profiler_sched_buf[cpu][i % PROFILER_SCHED_BUF].name, n->name, sizeof(n->name));
}

void lkperf_sched_switch(thread_t *oldthread, thread_t *newthread) {
    if (!profiler_sched_on)
        return;
    // NAME events go before the switch, so the host knows both threads.
    profiler_sched_name_seen(oldthread);
    profiler_sched_name_seen(newthread);
    uint32_t wq = oldthread->state == THREAD_BLOCKED
                      ? (uint32_t)(uintptr_t)oldthread->blocking_wait_queue : 0;
    profiler_sched_put(PROFILER_SCHED_SWITCH, oldthread->state, (uint32_t)(uintptr_t)oldthread,
                       (uint32_t)(uintptr_t)newthread, wq, 0);
}

void lkperf_sched_wakeup(thread_t *t, enum thread_state from, void *wq, status_t err) {
    if (!profiler_sched_on)
        return;
    profiler_sched_put(PROFILER_SCHED_WAKE, from, (uint32_t)(uintptr_t)t,
                       (uint32_t)(uintptr_t)get_current_thread(), (uint32_t)(uintptr_t)wq, err);
}

#if BCM2711
// VideoCore property mailbox, polled (same mechanism as overlay 0011's UART
// clock request). One request at a time.
static mutex_t profiler_mbox_lock = MUTEX_INITIAL_VALUE(profiler_mbox_lock);

static uint32_t profiler_mbox_get(uint32_t tag, uint32_t id) {
    static volatile uint32_t __ALIGNED(16) mbox[8];
    mutex_acquire(&profiler_mbox_lock);
    mbox[0] = sizeof(mbox);
    mbox[1] = 0;
    mbox[2] = tag;
    mbox[3] = 8;
    mbox[4] = 0;
    mbox[5] = id;
    mbox[6] = 0;
    mbox[7] = 0;   // end tag
    arch_clean_cache_range((addr_t)mbox, sizeof(mbox));
    uint32_t bus = ((uint32_t)(uintptr_t)mbox & 0x3fffffff) + BCM_SDRAM_BUS_ADDR_BASE;
    while (*REG32(ARM0_MAILBOX_STATUS) & (1u << 31)) {}
    *REG32(ARM0_MAILBOX_WRITE) = (bus & ~0xfu) | 8u;
    for (;;) {
        while (*REG32(ARM0_MAILBOX_STATUS) & (1u << 30)) {}
        if ((*REG32(ARM0_MAILBOX_READ) & 0xfu) == 8u)
            break;
    }
    arch_invalidate_cache_range((addr_t)mbox, sizeof(mbox));
    // GET_THROTTLED answers in the first value word, clock tags in the second.
    uint32_t v = tag == 0x00030046 ? mbox[5] : mbox[6];
    mutex_release(&profiler_mbox_lock);
    return v;
}

#define PROFILER_MBOX_CLOCK_RATE 0x00030002u
#define PROFILER_MBOX_CLOCK_MEASURED 0x00030047u
#define PROFILER_MBOX_THROTTLED 0x00030046u
#define PROFILER_MBOX_CLOCK_ARM 3u

static void profiler_freq_read(uint32_t *measured, uint32_t *set, uint32_t *throttled) {
    *measured = profiler_mbox_get(PROFILER_MBOX_CLOCK_MEASURED, PROFILER_MBOX_CLOCK_ARM);
    *set = profiler_mbox_get(PROFILER_MBOX_CLOCK_RATE, PROFILER_MBOX_CLOCK_ARM);
    *throttled = profiler_mbox_get(PROFILER_MBOX_THROTTLED, 0);
}

static void profiler_freq_record(void) {
    uint32_t m, st, th;
    profiler_freq_read(&m, &st, &th);
    arch_interrupt_saved_state_t is = arch_interrupt_save();   // stay on this core's ring
    profiler_sched_put(PROFILER_SCHED_FREQ, 0, m, st, th, 0);
    arch_interrupt_restore(is);
}

static volatile uint32_t profiler_freq_ms;
static volatile uint32_t profiler_freq_running;

static int profiler_freq_thread(void *arg) {
    (void)arg;
    while (profiler_sched_on && profiler_freq_ms) {
        profiler_freq_record();
        thread_sleep(profiler_freq_ms);
    }
    profiler_freq_running = 0;
    return 0;
}
#endif

static void profiler_sched_enable(uint32_t freq_ms) {
    if (!profiler_run_id)
        profiler_session_start();
    profiler_sched_on = 0;
    for (int c = 0; c < SMP_MAX_CPUS; c++)
        profiler_sched_total[c] = 0;
    THREAD_LOCK(lock_state);   // the hooks write the name cache under thread_lock
    memset(profiler_sched_names, 0, sizeof(profiler_sched_names));
    THREAD_UNLOCK(lock_state);
    profiler_sched_start = profiler_cntpct();
    profiler_sched_stop = 0;
    profiler_sched_on = 1;
#if BCM2711
    profiler_freq_ms = freq_ms;
    if (freq_ms && !profiler_freq_running) {
        profiler_freq_running = 1;
        thread_t *t = thread_create("profiler_freq", profiler_freq_thread, NULL, LOW_PRIORITY,
                                    DEFAULT_STACK_SIZE);
        if (t)
            thread_detach_and_resume(t);
        else
            profiler_freq_running = 0;
    }
#endif
}

static void profiler_sched_disable(void) {
    if (profiler_sched_on) {
#if BCM2711
        profiler_freq_record();   // closing frequency sample
#endif
        profiler_sched_on = 0;
        profiler_sched_stop = profiler_cntpct();
    }
}

static void profiler_sched_status(void) {
    printf("sched: recording %s, frequency thread %s (%u ms)\n", profiler_sched_on ? "on" : "off",
#if BCM2711
           profiler_freq_running ? "running" : "stopped", profiler_freq_ms);
#else
           "n/a", 0u);
#endif
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        uint32_t n = profiler_sched_total[c];
        printf("sched: cpu%d events=%u retained=%u overwritten=%u\n", c, n,
               n < PROFILER_SCHED_BUF ? n : PROFILER_SCHED_BUF,
               n > PROFILER_SCHED_BUF ? n - PROFILER_SCHED_BUF : 0);
    }
}

// Thread names: live threads, then threads seen at a switch that have exited
// since (the host keeps the first intact name it sees for a tid).
static void profiler_sched_print_threads(void) {
    static struct profiler_sched_name live[PROFILER_SCHED_NAMES];
    uint32_t nlive = 0;
    THREAD_LOCK(state);
    thread_t *t;
    list_for_every_entry(&thread_list, t, thread_t, thread_list_node) {
        if (nlive == PROFILER_SCHED_NAMES)
            break;
        live[nlive].tid = (uint32_t)(uintptr_t)t;
        live[nlive].prio = t->priority;
        live[nlive].flags = t->flags;
        memcpy(live[nlive].name, t->name, sizeof(live[nlive].name));
        live[nlive].name[sizeof(live[nlive].name) - 1] = 0;
        nlive++;
    }
    THREAD_UNLOCK(state);
    for (uint32_t pass = 0; pass < 2; pass++) {
        const struct profiler_sched_name *tab = pass ? profiler_sched_names : live;
        uint32_t cnt = pass ? PROFILER_SCHED_NAMES : nlive;
        for (uint32_t i = 0; i < cnt; i++) {
            const struct profiler_sched_name *n = &tab[i];
            if (!n->tid)
                continue;
            uint32_t h = profiler_fnv(profiler_fnv(profiler_fnv(0x811c9dc5u, n->tid),
                                                   (uint32_t)n->prio), n->flags);
            char name[32];
            size_t len = 0;
            for (; len < sizeof(name) - 1 && n->name[len]; len++) {
                char ch = n->name[len];
                name[len] = (ch > ' ' && ch < 127) ? ch : '_';
                h = profiler_fnv(h, (uint8_t)name[len]);
            }
            name[len] = 0;
            printf("THREAD tid=%08x prio=%08x flags=%08x name=%s crc=%08x\n", n->tid,
                   (uint32_t)n->prio, n->flags, len ? name : "-", h);
        }
    }
}

// Checksummed text dump, like the sample dump. Recording stops first, so
// nothing is overwritten while it prints.
static void profiler_sched_dump(void) {
    profiler_sched_disable();
    profiler_link_warmup();
    uint32_t freq = profiler_cntfrq();
    uint32_t image_lo = (uint32_t)(uintptr_t)_start;
    uint32_t image_hi = (uint32_t)(uintptr_t)__rodata_end;
    uint32_t build = profiler_image_hash();
    uint32_t w[] = { 1u, (uint32_t)profiler_run_id, (uint32_t)(profiler_run_id >> 32), freq,
                     SMP_MAX_CPUS, PROFILER_SCHED_BUF, image_lo, image_hi, build,
                     (uint32_t)profiler_sched_start, (uint32_t)(profiler_sched_start >> 32),
                     (uint32_t)profiler_sched_stop, (uint32_t)(profiler_sched_stop >> 32) };
    uint32_t k = 0x811c9dc5u;
    for (unsigned i = 0; i < sizeof(w) / sizeof(w[0]); i++)
        k = profiler_fnv(k, w[i]);
    printf("SCHEDBEGIN fmt=00000001 run=%016llx cntfrq=%08x cpus=%08x buf=%08x "
           "image=%08x-%08x build=%08x start=%016llx stop=%016llx crc=%08x\n",
           (unsigned long long)profiler_run_id, freq, SMP_MAX_CPUS, PROFILER_SCHED_BUF, image_lo,
           image_hi, build, (unsigned long long)profiler_sched_start,
           (unsigned long long)profiler_sched_stop, k);
    uint32_t sent = 0;
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        uint32_t n = profiler_sched_total[c];
        uint32_t kept = n < PROFILER_SCHED_BUF ? n : PROFILER_SCHED_BUF;
        k = profiler_fnv(profiler_fnv(profiler_fnv(0x811c9dc5u, (uint32_t)c), n), kept);
        printf("SCHEDCPU cpu=%d total=%08x retained=%08x crc=%08x\n", c, n, kept, k);
    }
    profiler_sched_print_threads();
    uint32_t seq = 0;
    for (int c = 0; c < SMP_MAX_CPUS; c++) {
        uint32_t n = profiler_sched_total[c];
        uint32_t kept = n < PROFILER_SCHED_BUF ? n : PROFILER_SCHED_BUF;
        for (uint32_t i = n - kept; i < n; i++) {
            const struct profiler_sched_ev *e = &profiler_sched_buf[c][i % PROFILER_SCHED_BUF];
            uint32_t ew[] = { seq, (uint32_t)c, (uint32_t)e->ts, (uint32_t)(e->ts >> 32),
                              e->type_state, e->a, e->b, e->c, (uint32_t)e->err };
            uint32_t h = 0x811c9dc5u;
            for (unsigned j = 0; j < sizeof(ew) / sizeof(ew[0]); j++)
                h = profiler_fnv(h, ew[j]);
            char name[32] = "";
            if ((e->type_state & 0xff) == PROFILER_SCHED_NAME) {
                size_t len = 0;
                for (; len < sizeof(name) - 1 && e->name[len]; len++) {
                    char ch = e->name[len];
                    name[len] = (ch > ' ' && ch < 127) ? ch : '_';
                    h = profiler_fnv(h, (uint8_t)name[len]);
                }
                name[len] = 0;
            }
            printf("SCHED seq=%08x cpu=%d ts=%016llx ev=%08x a=%08x b=%08x c=%08x err=%08x "
                   "%s%s%scrc=%08x\n", seq, c, (unsigned long long)e->ts, e->type_state, e->a,
                   e->b, e->c, (uint32_t)e->err, name[0] ? "name=" : "", name,
                   name[0] ? " " : "", h);
            seq++;
            sent++;
        }
    }
    profiler_sched_print_threads();   // again: the host keeps the first intact copy
    k = profiler_fnv(profiler_fnv(profiler_fnv(0x811c9dc5u, (uint32_t)profiler_run_id),
                                  (uint32_t)(profiler_run_id >> 32)), sent);
    printf("SCHEDEND run=%016llx events=%08x crc=%08x\n", (unsigned long long)profiler_run_id,
           sent, k);
}

// K8 ground truth. A waker sleeps 2 ms, works 200 us and signals an event; a
// waiter blocks on that event and works 500 us per wakeup. Two lockers take
// turns on a mutex, holding it 300 us and sleeping 1 ms between turns. So,
// per iteration: one waiter wakeup from `profiler_st_event`, one waker
// wakeup from sleep, and some blocking on `profiler_st_mutex`.
static event_t profiler_st_event;
static mutex_t profiler_st_mutex;
static volatile uint32_t profiler_st_iters;
// Independent check of the trace: CNTPCT around every thread_sleep() call,
// [0] for the waker's 2 ms sleeps, [1] for the lockers' 1 ms sleeps.
static uint64_t profiler_st_slept[2];
static uint32_t profiler_st_sleeps[2];
static spin_lock_t profiler_st_lock = SPIN_LOCK_INITIAL_VALUE;

static void profiler_st_sleep(uint32_t ms, int which) {
    uint64_t t0 = profiler_cntpct();
    thread_sleep(ms);
    uint64_t d = profiler_cntpct() - t0;
    arch_interrupt_saved_state_t st = spin_lock_irqsave(&profiler_st_lock);
    profiler_st_slept[which] += d;
    profiler_st_sleeps[which]++;
    spin_unlock_irqrestore(&profiler_st_lock, st);
}

static void profiler_st_work(uint32_t us) {
    profiler_spin_ticks(((uint64_t)us * profiler_cntfrq()) / 1000000u);
}

static int profiler_st_waker(void *arg) {
    for (uint32_t i = 0; i < profiler_st_iters; i++) {
        profiler_st_sleep(2, 0);
        profiler_st_work(200);
        event_signal(&profiler_st_event, true);
    }
    return 0;
}

static int profiler_st_waiter(void *arg) {
    for (uint32_t i = 0; i < profiler_st_iters; i++) {
        event_wait(&profiler_st_event);
        profiler_st_work(500);
    }
    return 0;
}

static int profiler_st_locker(void *arg) {
    for (uint32_t i = 0; i < profiler_st_iters; i++) {
        mutex_acquire(&profiler_st_mutex);
        profiler_st_work(300);
        mutex_release(&profiler_st_mutex);
        profiler_st_sleep(1, 1);
    }
    return 0;
}

static void profiler_schedtest(uint32_t iters) {
    profiler_st_iters = iters;
    memset(profiler_st_slept, 0, sizeof(profiler_st_slept));
    memset(profiler_st_sleeps, 0, sizeof(profiler_st_sleeps));
    event_init(&profiler_st_event, false, EVENT_FLAG_AUTOUNSIGNAL);
    mutex_init(&profiler_st_mutex);
    thread_t *t[4];
    t[0] = thread_create("st_waiter", profiler_st_waiter, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    t[1] = thread_create("st_waker", profiler_st_waker, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    t[2] = thread_create("st_lock1", profiler_st_locker, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    t[3] = thread_create("st_lock2", profiler_st_locker, NULL, DEFAULT_PRIORITY, DEFAULT_STACK_SIZE);
    for (int i = 0; i < 4; i++)
        thread_resume(t[i]);
    for (int i = 0; i < 4; i++)
        thread_join(t[i], NULL, INFINITE_TIME);
    event_destroy(&profiler_st_event);
    mutex_destroy(&profiler_st_mutex);
    for (int i = 0; i < 2; i++) {
        uint64_t mean_us = profiler_st_sleeps[i]
            ? (profiler_st_slept[i] * 1000000u) / profiler_cntfrq() / profiler_st_sleeps[i] : 0;
        printf("schedtest: thread_sleep(%d) x %u: mean %llu us (measured with CNTPCT)\n",
               i ? 1 : 2, profiler_st_sleeps[i], (unsigned long long)mean_us);
    }
}

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    if (argc < 2) {
        printf("usage: profiler <start [period_us]|stop|stat [-e ev,...] [cmd...]|status|clear|dump|bench|nest|smp|edgetest|pmustart|pmustop|pmu|maskon|maskoff|mask|masktest|memtest|nmion|nmioff|buildid|dbginfo|dbgrom|dbgpcsr|sched [on [freq_ms]|off]|schedump|schedtest|freq>\n");
        return -1;
    }

    const char *sub = argv[1].str;

    if (!strcmp(sub, "sched")) {
        if (argc >= 3 && !strcmp(argv[2].str, "on")) {
            uint32_t ms = argc >= 4 ? (uint32_t)argv[3].u : 100;
            profiler_sched_enable(ms);
            printf("sched: recording context switches and wakeups on all %d cores "
                   "(%u events/core ring), ARM clock every %u ms\n", SMP_MAX_CPUS,
                   PROFILER_SCHED_BUF, ms);
        } else if (argc >= 3 && !strcmp(argv[2].str, "off")) {
            profiler_sched_disable();
            printf("sched: recording off\n");
        } else {
            profiler_sched_status();
        }
    } else if (!strcmp(sub, "schedump")) {
        profiler_sched_dump();
    } else if (!strcmp(sub, "schedtest")) {
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 200;
        printf("schedtest: %u iterations: waiter/waker on an event, two lockers on a mutex ...\n",
               iters);
        profiler_schedtest(iters);
        printf("schedtest: done\n");
#if BCM2711
    } else if (!strcmp(sub, "freq")) {
        uint32_t m, st, th;
        profiler_freq_read(&m, &st, &th);
        printf("freq: ARM clock measured %u Hz, set %u Hz, throttled flags %08x\n", m, st, th);
#endif
    } else if (!strcmp(sub, "dbginfo")) {
        profiler_dbginfo();
#if BCM2711
    } else if (!strcmp(sub, "dbgrom")) {
        profiler_dbg_find(true);
    } else if (!strcmp(sub, "dbgpcsr")) {
        profiler_dbgpcsr();
#endif
    } else if (!strcmp(sub, "buildid")) {
        uint64_t t0 = profiler_cntpct();
        uint32_t h = profiler_image_hash();
        uint64_t t1 = profiler_cntpct();
        printf("buildid: image %08x-%08x fnv1a=%08x (%u bytes, %u us)\n",
               (uint32_t)(uintptr_t)_start, (uint32_t)(uintptr_t)__rodata_end, h,
               (uint32_t)(__rodata_end - _start),
               (uint32_t)(((t1 - t0) * 1000000u) / profiler_cntfrq()));
    } else if (!strcmp(sub, "maskon")) {
        profiler_mask_on();
        printf("mask: IRQ-masked time accounting on (new window on all %d cores)\n",
               SMP_MAX_CPUS);
    } else if (!strcmp(sub, "maskoff")) {
        arm_irqmask_on = 0;
        printf("mask: IRQ-masked time accounting off (totals kept)\n");
    } else if (!strcmp(sub, "mask")) {
        profiler_mask_print();
    } else if (!strcmp(sub, "nmion")) {
#if BCM2711
        if (profiler_nmi_enable())
            printf("nmi: pseudo-NMI on all %d cores: masked code is masked by GIC priority, "
                   "PMU and timer sampling still reach it (IRQ handlers stay unsampled)\n",
                   SMP_MAX_CPUS);
#else
        printf("nmi: needs real hardware (BCM2711)\n");
#endif
    } else if (!strcmp(sub, "nmioff")) {
#if BCM2711
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_nmi_off_this_cpu, NULL);
        arm_nmi_any = 0;
        printf("nmi: off; masking uses CPSR.I again (GIC priorities left as set)\n");
#else
        printf("nmi: needs real hardware (BCM2711)\n");
#endif
    } else if (!strcmp(sub, "memtest")) {
        uint32_t iters = argc >= 3 ? (uint32_t)argv[2].u : 200000;
        printf("memtest: %u x memcpy (profiler_copy_a) and %u x memset (profiler_fill_b), "
               "%u bytes ...\n", iters, iters, profiler_mem_len);
        profiler_copy_a(iters);
        profiler_fill_b(iters);
        printf("memtest: done\n");
    } else if (!strcmp(sub, "masktest")) {
        uint32_t loops = argc >= 3 ? (uint32_t)argv[2].u : 2000;
        uint32_t us = argc >= 4 ? (uint32_t)argv[3].u : 500;
        printf("masktest: %u x (%u us masked + %u us unmasked) on cpu%u ...\n", loops, us, us,
               arch_curr_cpu_num());
        profiler_masktest(loops, us);
        printf("masktest: done\n");
    } else if (!strcmp(sub, "start")) {
#if BCM2711
        uint32_t us = argc >= 3 ? (uint32_t)argv[2].u : 10000;
        if (us < 50 || us > 1000000) {
            printf("usage: profiler start [period_us]  (50..1000000, default 10000)\n");
            return -1;
        }
        if (profiler_enabled) {
            profiler_enabled = 0;
            mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_timer_disarm_this_cpu, NULL);
        }
#endif
        if (!profiler_run_id)
            profiler_session_start();
#if BCM2711
        if ((profiler_modes_used & PROFILER_MODE_TIMER) && profiler_timer_period_us != us)
            profiler_timer_mixed = 1;
        profiler_timer_period_us = us;
#endif
        profiler_modes_used |= PROFILER_MODE_TIMER;
        if (!arm_irqmask_on) {
            profiler_mask_on();
            printf("mask: IRQ-masked time accounting started with sampling\n");
        }
        profiler_enabled = 1;
#if BCM2711
        profiler_timer_period = (uint32_t)(((uint64_t)us * profiler_cntfrq()) / 1000000u);
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_timer_arm_this_cpu, NULL);
        printf("profiler: sampling started (virtual timer irq %d, one sample at a random point "
               "of every %u us, %d cores, buffer %d entries/core)\n",
               PROFILER_TIMER_IRQ, us, SMP_MAX_CPUS, PROFILER_BUF_SIZE);
#else
        printf("profiler: sampling started (irq %d, %d cores, buffer %d entries/core)\n",
               PROFILER_TIMER_IRQ, SMP_MAX_CPUS, PROFILER_BUF_SIZE);
#endif
    } else if (!strcmp(sub, "stop")) {
        profiler_enabled = 0;
#if BCM2711
        mp_sync_exec(MP_IPI_TARGET_ALL, 0, profiler_timer_disarm_this_cpu, NULL);
#endif
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
        memset(profiler_slen_buf, 0, sizeof(profiler_slen_buf));
        profiler_session_start();
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            profiler_pmu_missed[c] = 0;
            profiler_timer_missed[c] = 0;
        }
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

        // K2: header. Per-core totals are read once here and the export below
        // uses exactly these values, so the header, the records and the footer
        // agree even if a sampler is still running (stop both first anyway).
        if (!profiler_run_id)
            profiler_session_start();
        profiler_link_warmup();
        uint32_t dump_no = ++profiler_dump_no;
        uint32_t snap_total[SMP_MAX_CPUS], snap_head[SMP_MAX_CPUS];
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            snap_total[c] = profiler_total[c];
            snap_head[c] = profiler_head[c];
        }
        uint32_t image_lo = (uint32_t)(uintptr_t)_start;
        uint32_t image_hi = (uint32_t)(uintptr_t)__rodata_end;
        uint32_t build = profiler_image_hash();
        uint32_t hk = 0x811c9dc5u;
        uint32_t hv[] = { PROFILER_DUMP_FORMAT, (uint32_t)profiler_run_id,
                          (uint32_t)(profiler_run_id >> 32), dump_no, image_lo, image_hi,
                          build, profiler_modes_used, profiler_pmu_event_used,
                          profiler_pmu_period_used, profiler_pmu_config_mixed, SMP_MAX_CPUS,
                          PROFILER_BUF_SIZE, profiler_timer_period_us, profiler_timer_mixed };
        for (unsigned i = 0; i < sizeof(hv) / sizeof(hv[0]); i++)
            hk = profiler_fnv(hk, hv[i]);
        printf("DUMPBEGIN fmt=%08x run=%016llx dump=%08x image=%08x-%08x build=%08x "
               "modes=%08x event=%08x period=%08x mixed=%08x cpus=%08x buf=%08x "
               "tperiod=%08x tmixed=%08x crc=%08x\n",
               PROFILER_DUMP_FORMAT, (unsigned long long)profiler_run_id, dump_no, image_lo,
               image_hi, build, profiler_modes_used, profiler_pmu_event_used,
               profiler_pmu_period_used, profiler_pmu_config_mixed, SMP_MAX_CPUS,
               PROFILER_BUF_SIZE, profiler_timer_period_us, profiler_timer_mixed, hk);
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            uint32_t kept = snap_total[c] < PROFILER_BUF_SIZE ? snap_total[c] : PROFILER_BUF_SIZE;
            uint32_t ck = 0x811c9dc5u;
            uint32_t cv[] = { (uint32_t)c, snap_total[c], kept, snap_total[c] - kept,
                              profiler_pmu_missed[c], profiler_timer_missed[c] };
            for (unsigned i = 0; i < sizeof(cv) / sizeof(cv[0]); i++)
                ck = profiler_fnv(ck, cv[i]);
            printf("DUMPCPU cpu=%d total=%08x retained=%08x overwritten=%08x pmumissed=%08x "
                   "tmissed=%08x crc=%08x\n", c, snap_total[c], kept, snap_total[c] - kept,
                   profiler_pmu_missed[c], profiler_timer_missed[c], ck);
        }

        uint32_t seq = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            uint32_t count = snap_total[c] < PROFILER_BUF_SIZE ?
                             snap_total[c] : PROFILER_BUF_SIZE;
            uint32_t start = snap_total[c] < PROFILER_BUF_SIZE ?
                             0 : snap_head[c];
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
                uint32_t slen = profiler_slen_buf[c][idx];
                uint32_t crc = profiler_sample_checksum((uint32_t)c, seq, pc, lr, fp, sp,
                                                         spsr, tid, ts, stack, src, lat,
                                                         msite, mgap, slen);
                // K9: the whole record in one write (it used to take 130
                // printf calls, one per stack byte).
                static const char hex[] = "0123456789abcdef";
                char stack_hex[2 * PROFILER_STACK_CAPTURE_BYTES + 1];
                for (int b = 0; b < PROFILER_STACK_CAPTURE_BYTES; b++) {
                    stack_hex[2 * b] = hex[stack[b] >> 4];
                    stack_hex[2 * b + 1] = hex[stack[b] & 0xf];
                }
                stack_hex[2 * PROFILER_STACK_CAPTURE_BYTES] = 0;
                printf("SAMPLE seq=%08x cpu=%d pc=%08x lr=%08x fp=%08x sp=%08x "
                       "spsr=%08x tid=%08x ts=%016llx stack=%s src=%c lat=%08x msite=%08x "
                       "mgap=%08x slen=%08x crc=%08x\n",
                       seq, c, pc, lr, fp, sp, spsr, tid, (unsigned long long)ts, stack_hex,
                       src ? (char)src : '?', lat, msite, mgap, slen, crc);
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
        // K12: cores masking by GIC priority at dump time (bit n = cpu n);
        // PMU samples reach their masked code, timer samples do not.
        uint32_t nmi_cpus = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++)
            nmi_cpus |= (arm_nmi_mode[c] ? 1u : 0u) << c;
        printf("MASKNMI cpus=%08x crc=%08x\n", nmi_cpus,
               profiler_fnv(0x811c9dc5u, nmi_cpus));
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
        // K2: footer -- the exact number of SAMPLE records this dump printed.
        uint32_t fk = profiler_fnv(profiler_fnv(profiler_fnv(profiler_fnv(0x811c9dc5u,
                      (uint32_t)profiler_run_id), (uint32_t)(profiler_run_id >> 32)),
                      dump_no), seq);
        printf("DUMPEND run=%016llx dump=%08x samples=%08x crc=%08x\n",
               (unsigned long long)profiler_run_id, dump_no, seq, fk);
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
        // K7: perf-stat-like counting of any console command (or the
        // built-in workload), on every core, up to six events plus cycles.
#if BCM2711
        return profiler_stat(argc, argv);
#else
        printf("stat: needs real hardware (BCM2711)\n");
#endif
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
        if (profiler_stat_busy) {
            printf("pmustart: refused while `profiler stat` is counting\n");
            return -1;
        }
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
        if (!profiler_run_id)
            profiler_session_start();
        if ((profiler_modes_used & PROFILER_MODE_PMU) &&
            (profiler_pmu_event_used != event || profiler_pmu_period_used != count))
            profiler_pmu_config_mixed = 1;
        profiler_modes_used |= PROFILER_MODE_PMU;
        profiler_pmu_event_used = event;
        profiler_pmu_period_used = count;
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
        // M5: on THIS real hardware, PMU access works cleanly: the Pi's
        // firmware clears that trap during its secure-world boot. Both
        // counting (`profiler stat`, K7) and overflow-interrupt sampling
        // (`pmustart`, M5/K6) are verified on the Pi; this command just
        // reports the PMU (K9: it used to say the interrupt path was
        // unverified).
        uint32_t pmcr = pmu_read_pmcr();
        printf("pmu: PMCR=%08x, %u event counters + cycle counter; PMCEID0=%08x PMCEID1=%08x "
               "PMCNTENSET=%08x (cpu%u)\n", pmcr, (pmcr >> 11) & 0x1f, pmu_read_pmceid0(),
               pmu_read_pmceid1(), pmu_read_pmcntenset(), arch_curr_cpu_num());
        printf("pmu: counting: `profiler stat [-e ev,...] <command>`; sampling: "
               "`profiler pmustart <event> <count>` / `pmustop`\n");
    } else if (!strcmp(sub, "status")) {
        uint32_t total = 0;
        for (int c = 0; c < SMP_MAX_CPUS; c++) {
            total += profiler_total[c];
            printf("profiler: cpu%d total_samples=%u head=%u%s pmu_missed=%u timer_missed=%u\n",
                   c, profiler_total[c], profiler_head[c],
                   profiler_total[c] >= PROFILER_BUF_SIZE ? " (WRAPPED)" : "",
                   profiler_pmu_missed[c], profiler_timer_missed[c]);
        }
        printf("profiler: enabled=%u total_samples(all cpus)=%u capacity=%d/core\n",
               profiler_enabled, total, PROFILER_BUF_SIZE);
        printf("profiler: masking mode %s (per core:", arm_nmi_any ? "pseudo-NMI" : "CPSR.I");
        for (int c = 0; c < SMP_MAX_CPUS; c++)
            printf(" %u", arm_nmi_mode[c]);
        printf(")\n");
    } else {
        printf("unknown subcommand '%s'\n", sub);
        return -1;
    }
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("profiler", "bare-metal sampling profiler (start|stop|status|clear|mask)", &cmd_profiler)
STATIC_COMMAND_END(profiler);
