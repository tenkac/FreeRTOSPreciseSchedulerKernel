# Project 1 — Timeline Scheduler for FreeRTOS (thin kernel patch)

A deterministic, time-triggered (cyclic-executive) scheduler integrated into
the FreeRTOS kernel through a deliberately minimal patch.

## Design philosophy: keep the kernel patch thin

The kernel is edited in exactly one place — a ~10-line guarded block in
`xTaskIncrementTick()` (plus one `#include`). The hook does **no scheduling
policy**: each tick it only advances a frame-relative counter and gives a
binary semaphore. *All* HRT/SRT/deadline/frame-reset decisions live in a
user-space engine task outside the kernel. This serves the spec's
"minimal intrusivity / easily portable" requirement.

```
   tick ISR                     user space
 ┌──────────────────┐        ┌────────────────────────────┐
 │ xTaskIncrementTick│  give  │ prvEngineTask (highest prio)│
 │   └ timeline_hook ├───────▶│  - frame wrap -> reset all  │
 │     (counter +    │  binary│  - HRT start -> spawn task  │
 │      1 semaphore) │  sem   │  - HRT end   -> terminate   │
 └──────────────────┘        │  - idle      -> run SRT chain│
                             └────────────────────────────┘
```

## Tree layout

```
app/
  main.c                      demo: 4 tasks, prints CPU stats, exits via QEMU semihosting
  startup.c, uart_io.c        Cortex-M3 vector table & UART driver
scheduler/
  timeline_engine.h / .c      engine: validation, scheduling, hooks, CPU stats
  trace.h / .c                tick-level trace -> UART + in-RAM ring buffer
kernel_patch/
  timeline_hook.h / .c        kernel-side hook (counter + semaphore give)
tests/
  test_main.c                 on-target C test suite (16 tests, PASS/FAIL output)
config/
  FreeRTOSConfig.h            adds configUSE_TIMELINE_SCHED 1
  linker_script.ld
Makefile
```

## Kernel integration (manual `tasks.c` edits)

Two edits to `FreeRTOS-Kernel/tasks.c` (V11.3.0):

1. Add after `#include <string.h>` (~line 32):
   ```c
   #if ( configUSE_TIMELINE_SCHED == 1 )
       #include "timeline_hook.h"
   #endif
   ```

2. After the FIRST `#endif /* configUSE_TICK_HOOK */` block (~line 4918) — the
   one in the scheduler-running branch where `xSwitchRequired` is in scope:
   ```c
   #if ( configUSE_TIMELINE_SCHED == 1 )
   {
       if( xTimelineTickHookFromISR( xTickCount ) != pdFALSE )
       {
           xSwitchRequired = pdTRUE;
       }
   }
   #endif
   ```

Plus `#define configUSE_TIMELINE_SCHED 1` in `FreeRTOSConfig.h`. When the macro
is 0, the kernel compiles byte-for-byte stock.

## Runtime model

Priorities are a *mechanism*, not the policy:

| Role        | Priority                    | Purpose                              |
|-------------|-----------------------------|--------------------------------------|
| Engine      | `configMAX_PRIORITIES - 1`  | wakes each tick on the semaphore     |
| HRT tasks   | `configMAX_PRIORITIES - 2`  | preempt SRT -> non-preemptive vs SRT |
| Supervisor  | `tskIDLE_PRIORITY + 2`      | demo only: frame counter, CPU report |
| SRT tasks   | `tskIDLE_PRIORITY + 1`      | best-effort in idle, preemptible     |

The engine blocks on the tick semaphore (≤1 tick release jitter). Per tick it
reads the frame offset the hook computed and:

- **frame wrap** → destroy & recreate all tasks (deterministic replay; spec §4)
- **offset == HRT start** → create the one-shot HRT task (preempts SRT)
- **offset == HRT end & not completed** → `vTaskDelete` (deadline overrun)
- **no HRT window open** → SRT tasks run in fixed `ulOrder`

### HRT preemption semantics

The brief calls HRT "non-preemptive". This implementation provides:

- Non-preemptive **w.r.t. SRT** — HRT priority > SRT priority, so SRT cannot
  interrupt HRT.
- Non-preemptive **w.r.t. other HRT** — configuration rejects overlapping HRT
  windows; at most one HRT is ever ready.
- HRT *is* briefly preempted by the SysTick ISR and the engine task (both at
  higher priority than HRT). The engine runs for a few µs/tick to enforce
  deadlines. This is required for the deterministic property — HRT bodies
  still run to completion within their declared window without yielding to
  any application task.

## Configuration interface

```c
TimelineSlot_t — per task:
    pcName, pxBody, pvArg, eClass (HARD_RT/SOFT_RT),
    ulStartOffset, ulEndOffset, ulSubframeId,   // HRT timing, frame-relative ticks
    ulOrder,                                     // SRT fixed order
    usStackWords

TimelineSpec_t — global:
    ulMajorFrameTicks, ulSubframeTicks, pxSlots, ulSlotCount, ucTraceEnabled
```

Entry points:
- `vConfigureScheduler(spec)` — brief-compatible name, returns `BaseType_t`
- `xTimelineEngineReload(spec)` — swap at next frame boundary
- `xTimelineEngineStart(spec)` — configure + `vTaskStartScheduler`

### Sub-frame structure

Sub-frames are enforced at configuration time. For each HRT slot:
- `ulSubframeId` must be `< ulMajorFrameTicks / ulSubframeTicks`
- `[ulStartOffset, ulEndOffset)` must lie wholly inside sub-frame `ulSubframeId`

The trace records the sub-frame ID with each `RELEASE` event.

## CPU usage measurement

Two metrics:

- **`ulTimelineGetNonHrtTicks()`** — ticks per major frame with no HRT window
  open. Decided by the schedule, perfectly reproducible. (Historically called
  "idle" in older versions; renamed for clarity.)
- **`ulTimelineGetTrueIdleTicks()`** — cumulative ticks the FreeRTOS idle task
  ran (no application task wanted the CPU). Bumped from
  `vApplicationIdleHook()` via `vTimelineIdleTickAccount()`. This is what
  backs the spec's "CPU overhead ≤ 10%" requirement.

The demo prints both at the end. A typical run reports < 1% overhead for the
demo's 4-task schedule on QEMU lm3s6965evb.

## Error hooks (FreeRTOS-style, weak symbols)

Apps may override any subset by defining a strong symbol:

| Hook | Fires when |
|---|---|
| `vApplicationDeadlineMissHook(name, tick)` | Engine terminates an over-running HRT |
| `vApplicationTaskCreateFailedHook(name, class)` | `xTaskCreate` returns failure |
| `vApplicationScheduleErrorHook(error, detail)` | Engine-level failure (invalid spec, semaphore alloc, etc.) |

Defaults are silent no-ops; demo overrides print to UART.

## Dynamic allocation

The engine uses `xTaskCreate` / `vTaskDelete` every frame. Works cleanly on
`heap_4`. Long-running production systems may want `heap_5` with multiple
regions or `xTaskCreateStatic` with pre-allocated TCB/stack pools. Out of
scope for the project.

## Build & run

Requires `arm-none-eabi-gcc` and `qemu-system-arm`, plus the V11.3.0 kernel
edited as above in `FreeRTOS-Kernel/`.

```sh
make          # build build/scheduler.elf
make run      # run demo in QEMU (5 frames, then clean exit via semihosting)
make tests    # build build/tests.elf
make run-tests   # run the C test suite
make DEMO_FRAMES=20 run   # longer demo run
```

QEMU is launched with `-semihosting-config enable=on,target=native`, which is
what makes the BKPT-based exit work — no `Ctrl-A X` needed.

## Sample test output

```
============================================
 TIMELINE SCHEDULER - C TEST SUITE
============================================

--- Part A: static validation ---
[PASS] Reject overlap         : validate=0
[PASS] Reject OOB end         : validate=0
[PASS] Reject end<=start      : validate=0
[PASS] Accept good spec       : validate=1
[PASS] Reject sub-frame cross : validate=0
[PASS] Reject wrong sub-id    : validate=0
[PASS] Reject sub-id range    : validate=0

--- Part B: on-target behaviour ---

----- scenario: S1 in-window -----
[PASS] HRT in window          : 3 COMPLETE, 0 DEADLINE_MISS

----- scenario: S2 deadline-miss -----
[PASS] HRT deadline-miss      : 3 DEADLINE_MISS, 0 COMPLETE

----- scenario: S3 SRT order -----
[PASS] SRT fixed order        : X@1 < Y@3 < Z@5

----- scenario: S4 frame replay -----
[PASS] Frame replay           : 5 frames, COMPLETE always @offset 3

----- scenario: S5 release jitter -----
[PASS] Release jitter         : max jitter 0 tick over 5 releases

----- scenario: S6 stress 5 HRTs -----
[PASS] Stress 5 HRTs          : miss=0  completes H0..H4 = 4/4/4/4/4

----- scenario: S7 edge 1-tick -----
[PASS] Edge 1-tick window     : 1-tick body completes (3 COMPLETE, 0 MISS)

----- scenario: S8 edge back-to-back -----
[PASS] Edge back-to-back      : A1=3  A2=3  miss=0  ordered=1

----- scenario: S9 edge boundaries -----
[PASS] Edge frame boundaries  : E0=3  E1=3  miss=0

============================================
 RESULT: 16/16 passed
============================================
```

## Spec coverage

| Brief requirement | Status |
|---|---|
| Major frame, compile-time | ✅ `ulMajorFrameTicks` |
| Sub-frames | ✅ enforced at validation, logged on RELEASE |
| One-shot task model | ✅ trampolines, no self-reschedule |
| HRT spawn at start, terminate on overrun | ✅ engine loop |
| HRT non-preemptive | ✅ (see semantics section above) |
| SRT in idle, fixed order, preemptible by HRT | ✅ |
| Frame end: destroy + recreate, replay | ✅ |
| `vConfigureScheduler` system call | ✅ |
| Release jitter ≤ 1 tick | ✅ test S5 |
| CPU overhead ≤ 10% | ✅ measured by `vApplicationIdleHook` |
| Portability: QEMU Cortex-M | ✅ lm3s6965evb |
| Thread safety | ✅ binary semaphore + 32-bit atomic reads |
| Error hooks (FreeRTOS-style) | ✅ 3 weak hooks |
| Trace with tick-level resolution | ✅ live + in-RAM dump |
| Test suite with PASS/FAIL | ✅ 16 tests |