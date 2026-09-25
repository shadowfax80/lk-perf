/*
 * Bare-metal statistical sampling profiler for LK, AArch32.
 *
 * Stage 0 (this file): prove the build/boot/shell pipeline only -- no
 * sampling logic yet. See docs/DESIGN.md for the staged plan this follows:
 *   0. this skeleton (pipeline proof)
 *   1. PC-only histogram (periodic timer -> ring buffer -> QMP dump)
 *   2. PC+LR (one extra level of context, no unwind logic)
 *   3. Frame-pointer-chain offline unwind (-fno-omit-frame-pointer build +
 *      host-side walker) for real multi-level flame graphs
 *   4. SMP: per-CPU timer/buffer, CNTPCT-tagged samples for cross-core merge
 */

#include <lib/console.h>
#include <stdio.h>

static int cmd_profiler(int argc, const console_cmd_args *argv) {
    printf("profiler: stage 0 skeleton -- no sampling implemented yet\n");
    return 0;
}

STATIC_COMMAND_START
STATIC_COMMAND("profiler", "bare-metal sampling profiler (stage 0)", &cmd_profiler)
STATIC_COMMAND_END(profiler);
