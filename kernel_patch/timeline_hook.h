/*
 * timeline_hook.h
 *
 * THIN kernel-side hook for the timeline scheduler.
 *
 * Design intent: keep the kernel patch minimal ("minimal intrusivity").
 * The kernel does NOT know about tasks, deadlines, sub-frames, HRT/SRT, or any
 * scheduling policy. On every tick it merely:
 *      1. advances a frame-relative tick counter, and
 *      2. raises a single "tick advanced" signal (a binary semaphore)
 *         so the user-space timeline engine can run.
 *
 * ALL policy lives outside the kernel in timeline_engine.c.
 *
 * Integration: tasks.c calls vTimelineTickHookFromISR() once per tick from
 * inside xTaskIncrementTick(), guarded by configUSE_TIMELINE_SCHED.
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#ifndef TIMELINE_HOOK_H
#define TIMELINE_HOOK_H

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"    /* SemaphoreHandle_t */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Called once by the engine before the scheduler starts. Provides the major
 * frame length (in ticks) and the binary semaphore the hook will give from ISR
 * each tick. The engine creates and owns the semaphore.
 */
void vTimelineHookInit( TickType_t xMajorFrameTicks,
                        SemaphoreHandle_t xTickSignal );

/*
 * THE KERNEL PATCH ENTRY POINT.
 * Called from xTaskIncrementTick() once per tick (ISR context).
 * Returns pdTRUE if a context switch should be requested (i.e. the engine task,
 * which is the highest priority task, was unblocked and should run now).
 *
 * Intentionally tiny: counter maths + one give. No table walking, no policy.
 */
BaseType_t xTimelineTickHookFromISR( TickType_t xCurrentTick );

/*
 * Frame-relative tick (0 .. major_frame-1) sampled at the last hook call.
 * Read by the engine; written only by the hook. Single writer => no lock.
 */
TickType_t xTimelineGetFrameOffset( void );

/* Monotonically increasing major-frame index since init. */
uint32_t ulTimelineGetFrameNumber( void );

#ifdef __cplusplus
}
#endif

#endif /* TIMELINE_HOOK_H */