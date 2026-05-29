/*
 * timeline_engine.h
 *
 * User-space timeline scheduling ENGINE (all policy lives here, NOT in kernel).
 *
 * The kernel hook (timeline_hook.c) only advances a frame-relative tick and
 * gives a semaphore. This engine is a highest-priority task that waits on that
 * semaphore and, once per tick, drives the time-triggered schedule:
 *
 *   - HRT tasks: created (one-shot) at their start offset, terminated if they
 *     overrun their end offset.
 *   - SRT tasks: run best-effort in fixed order during idle gaps.
 *   - Frame boundary: destroy + recreate everything for deterministic replay.
 *
 * PREEMPTION SEMANTICS
 * --------------------
 * The brief calls HRT "non-preemptive". In this implementation that means:
 *   - HRT is non-preemptive WITH RESPECT TO SRT: SRT cannot interrupt HRT,
 *     because HRT runs at a higher FreeRTOS priority than SRT.
 *   - HRT is non-preemptive WITH RESPECT TO OTHER HRT: configuration rejects
 *     overlapping HRT windows, so at most one HRT runs at a time and there
 *     is never another HRT ready to preempt the running one.
 *   - HRT WILL be briefly preempted by the SysTick ISR and the engine task
 *     (both at higher priority than HRT). The engine wakes for at most a
 *     handful of microseconds per tick to advance the timeline and check
 *     deadlines, then blocks again. This is required: it is how the timeline
 *     deadline is enforced and how deterministic replay across frames is
 *     achieved. It does NOT violate non-preemption in the time-triggered-
 *     architecture sense - HRT bodies still run to completion within their
 *     declared window without yielding to any other application task.
 *
 * DYNAMIC ALLOCATION NOTE
 * -----------------------
 * The engine uses xTaskCreate / vTaskDelete every frame (one create+delete per
 * HRT activation, plus the SRT chain). This matches the spec's "destroy and
 * recreate" reset semantics and works cleanly on heap_4. On a long-running
 * production system the recurring alloc/free could cause heap fragmentation;
 * the standard mitigation is to switch to heap_5 with multiple regions, or
 * port to xTaskCreateStatic with pre-allocated TCB/stack pools per slot. Both
 * are mechanical changes and have been kept out of scope for the project.
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#ifndef TIMELINE_ENGINE_H
#define TIMELINE_ENGINE_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef teMAX_HRT_TASKS
    #define teMAX_HRT_TASKS    16
#endif
#ifndef teMAX_SRT_TASKS
    #define teMAX_SRT_TASKS    8
#endif

typedef enum
{
    HARD_RT = 0,
    SOFT_RT
} TaskClass_t;

/* One-shot body: runs start->return. No internal loop, no self-reschedule. */
typedef void (*TimelineBody_t)( void *pvArg );

typedef struct
{
    const char    *pcName;
    TimelineBody_t pxBody;
    void          *pvArg;
    TaskClass_t    eClass;

    /* HRT timing, frame-relative, in ticks. */
    uint32_t       ulStartOffset;
    uint32_t       ulEndOffset;     /* deadline; terminate if exceeded */
    uint32_t       ulSubframeId;

    /* SRT ordering (lower runs first). */
    uint32_t       ulOrder;

    uint16_t       usStackWords;
} TimelineSlot_t;

typedef struct
{
    uint32_t              ulMajorFrameTicks;
    uint32_t              ulSubframeTicks;
    const TimelineSlot_t *pxSlots;
    uint32_t              ulSlotCount;
    uint8_t               ucTraceEnabled;
} TimelineSpec_t;

/*
 * Parse the spec, validate it, build internal tables, create the kernel hook
 * semaphore, wire the hook, and create the engine task. Does NOT start the
 * FreeRTOS scheduler. Returns pdPASS / pdFAIL.
 *
 * The name matches the example system call in the assignment brief
 * (vConfigureScheduler). The BaseType_t return lets callers detect bad
 * configurations; the brief's "e.g." signature is treated as illustrative.
 */
BaseType_t vConfigureScheduler( const TimelineSpec_t *pxSpec );

/* Pure spec validation, no side effects. pdPASS if well-formed. */
BaseType_t xTimelineEngineValidate( const TimelineSpec_t *pxSpec );

/* Swap to a new spec at the next frame boundary. pdFAIL if spec invalid. */
BaseType_t xTimelineEngineReload( const TimelineSpec_t *pxSpec );

/* vConfigureScheduler + vTaskStartScheduler(). Returns pdFAIL only on config
 * error; otherwise never returns (FreeRTOS owns the CPU). */
BaseType_t xTimelineEngineStart( const TimelineSpec_t *pxSpec );

/* Optional explicit "I'm done early" marker for a running body. */
void vTimelineSlotComplete( void );

/* -------------------------------------------------------------------------- */
/* CPU usage statistics                                                       */
/*                                                                            */
/* Two counters with different meanings:                                      */
/*   1. ulTimelineGetNonHrtTicks() - ticks within a major frame at which no   */
/*      HRT window is open. This is what the trace module historically called */
/*      "idle" and is decided purely by the schedule. Always reproducible.    */
/*   2. ulTimelineGetTrueIdleTicks() - total ticks at which the FreeRTOS idle */
/*      task ran (i.e. NO application task wanted the CPU). Incremented by    */
/*      vApplicationIdleHook(). This is the metric that backs the spec's "CPU */
/*      overhead <= 10%" requirement.                                         */
/*                                                                            */
/* For a single major frame, CPU overhead =                                   */
/*   (frame_ticks - true_idle_ticks_during_frame) / frame_ticks               */
/* -------------------------------------------------------------------------- */
uint32_t ulTimelineGetNonHrtTicks( void );    /* per-frame, latched at frame end */
uint32_t ulTimelineGetTrueIdleTicks( void );  /* cumulative since boot           */

/* Provided by the engine, called from vApplicationIdleHook(). Application
 * idle hooks should call this once per invocation - it bumps the counter. */
void vTimelineIdleTickAccount( void );

/* -------------------------------------------------------------------------- */
/* Application-overridable error hooks (FreeRTOS-style)                       */
/*                                                                            */
/* These are declared weak in timeline_engine.c with safe defaults. The       */
/* application may override any subset by defining a strong symbol of the     */
/* same name in its own translation unit. The engine calls these from task    */
/* context (not ISR), so hooks may use any FreeRTOS API.                      */
/* -------------------------------------------------------------------------- */

typedef enum
{
    teERR_INVALID_SPEC = 0,   /* a reloaded spec failed validation            */
    teERR_SEMAPHORE_ALLOC,    /* binary semaphore creation failed             */
    teERR_TABLE_BUILD,        /* HRT/SRT table build failed (over capacity)   */
    teERR_ENGINE_TASK_CREATE  /* engine task could not be created             */
} TimelineError_t;

/*
 * Called when an HRT task is terminated for exceeding its end-tick deadline.
 * pcTaskName: the slot name; ulTick: the tick at which termination occurred.
 * Default: no-op (the trace already records DEADLINE_MISS).
 */
void vApplicationDeadlineMissHook( const char *pcTaskName, uint32_t ulTick );

/*
 * Called when xTaskCreate() returns failure while spawning an HRT slot or
 * releasing an SRT slot. eClass identifies which category failed.
 * Default: no-op.
 */
void vApplicationTaskCreateFailedHook( const char *pcTaskName, TaskClass_t eClass );

/*
 * Called on engine-level errors that are not per-task. eError identifies the
 * failure; pcDetail is an optional human-readable hint or NULL.
 * Default: no-op (caller of the failing API still gets pdFAIL).
 */
void vApplicationScheduleErrorHook( TimelineError_t eError, const char *pcDetail );

#ifdef __cplusplus
}
#endif

#endif /* TIMELINE_ENGINE_H */