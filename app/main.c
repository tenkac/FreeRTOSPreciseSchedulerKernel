/*
 * FreeRTOS V11.3.0 - Timeline Scheduler Extension
 * Copyright (C) 2026 Group N, Politecnico di Torino
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * https://www.FreeRTOS.org
 */

/*
 * main.c  -  Project 1 demo: timeline scheduler with THIN kernel patch.
 *
 * Architecture:
 *   - tasks.c calls xTimelineTickHookFromISR() once per tick (the only kernel
 *     edit besides an include). That hook advances a frame-relative counter and
 *     gives a binary semaphore.
 *   - prvEngineTask (timeline_engine.c, highest priority) wakes on that
 *     semaphore each tick and owns ALL scheduling policy.
 *
 * Build: arm-none-eabi-gcc, FreeRTOS, GCC/ARM_CM3 port.
 * Run:   qemu-system-arm -M lm3s6965evb -nographic -kernel build/scheduler.elf
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#include <stdio.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "timeline_engine.h"
#include "trace.h"

/* Busy-wait N ticks (models CPU compute time without yielding). */
static void prvBurnTicks( uint32_t ulTicks )
{
    TickType_t xStart = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xStart ) < ulTicks )
    {
        __asm volatile ( "nop" );
    }
}

/* -------------------------------------------------------------------------- */
/* Build mode                                                                 */
/* -------------------------------------------------------------------------- */
/* BENCHMARK_MODE=0 (default): full demo with CPU-bound task bodies, live UART
 * trace, and the deadline-miss showcase. CPU summary at the end therefore
 * includes application + UART time, not just scheduler overhead.
 *
 * BENCHMARK_MODE=1: identical schedule shape but every task body is a no-op
 * and live UART trace is disabled. CPU "busy" time now reflects only the
 * scheduler's own work (engine wake-ups, task create/delete, frame reset).
 * This is the number that backs the spec's "<=10% overhead" requirement.
 *
 * Build with `make BENCHMARK=1 run` to use this mode.                        */
#ifndef BENCHMARK_MODE
    #define BENCHMARK_MODE    0
#endif

#if BENCHMARK_MODE
    /* No-op bodies so "busy" time = scheduler infrastructure only. */
    static void HRT_A( void *pv ) { ( void ) pv; }
    static void HRT_B( void *pv ) { ( void ) pv; }
    static void SRT_X( void *pv ) { ( void ) pv; }
    static void SRT_Y( void *pv ) { ( void ) pv; }
#else
    /* ---- one-shot task bodies: run start->return, no loops ---- */

    static void HRT_A( void *pvArg )   /* well-behaved, finishes in window */
    {
        ( void ) pvArg;
        printf( "    (HRT_A: 3 ticks work)\n" );
        prvBurnTicks( 3 );
    }

    static void HRT_B( void *pvArg )   /* overruns -> terminated at deadline */
    {
        ( void ) pvArg;
        printf( "    (HRT_B: 8 ticks work - exceeds 5-tick window!)\n" );
        prvBurnTicks( 8 );
    }

    static void SRT_X( void *pvArg )
    {
        ( void ) pvArg;
        printf( "    (SRT_X: soft work)\n" );
        prvBurnTicks( 4 );
    }

    static void SRT_Y( void *pvArg )
    {
        ( void ) pvArg;
        printf( "    (SRT_Y: soft work)\n" );
        prvBurnTicks( 4 );
    }
#endif

/* ---- timeline: 50-tick major frame, 10-tick sub-frames ---- */
static const TimelineSlot_t xSlots[] =
{
    { "HRT_A", HRT_A, NULL, HARD_RT, 10, 15, 1, 0, configMINIMAL_STACK_SIZE },
    { "HRT_B", HRT_B, NULL, HARD_RT, 25, 30, 2, 0, configMINIMAL_STACK_SIZE },
    { "SRT_X", SRT_X, NULL, SOFT_RT,  0,  0, 0, 0, configMINIMAL_STACK_SIZE },
    { "SRT_Y", SRT_Y, NULL, SOFT_RT,  0,  0, 0, 1, configMINIMAL_STACK_SIZE },
};

static const TimelineSpec_t xSpec =
{
    .ulMajorFrameTicks = 50,
    .ulSubframeTicks   = 10,
    .pxSlots           = xSlots,
    .ulSlotCount       = sizeof( xSlots ) / sizeof( xSlots[ 0 ] ),
#if BENCHMARK_MODE
    .ucTraceEnabled    = 0,    /* no UART spam during benchmark */
#else
    .ucTraceEnabled    = 1,
#endif
};

/* -------------------------------------------------------------------------- */
/* Demo run limit                                                             */
/* -------------------------------------------------------------------------- */
/* How many major frames the demo runs before printing a summary and exiting
 * QEMU. Set to 0 to run forever (original behaviour). Override via -D in CFLAGS
 * if you want a longer/shorter run.                                          */
#ifndef DEMO_FRAMES
    #define DEMO_FRAMES    5
#endif

/* QEMU semihosting exit (Cortex-M, ANGEL_SWIreason_ReportException). Triggers
 * QEMU's "-semihosting" exit code path. lm3s6965evb's QEMU model accepts this
 * even without -semihosting on the command line - the BKPT just lands in the
 * default handler, which terminates the guest cleanly. */
static void prvQemuExit( int code )
{
    register int r0 __asm__ ( "r0" ) = 0x18;            /* SYS_EXIT       */
    register int r1 __asm__ ( "r1" ) = 0x20026;         /* ApplicationExit*/
    ( void ) code;
    __asm__ volatile ( "bkpt 0xAB" : : "r"( r0 ), "r"( r1 ) );
    for( ;; ) { }   /* if BKPT doesn't terminate, spin */
}

static void prvDemoSupervisor( void *pvArg )
{
    ( void ) pvArg;
    uint32_t ulFrames        = 0;
    uint32_t ulStartIdle     = ulTimelineGetTrueIdleTicks();
    TickType_t xStartTick    = xTaskGetTickCount();

    for( ;; )
    {
        vTaskDelay( pdMS_TO_TICKS( xSpec.ulMajorFrameTicks ) );
        ulFrames++;
        printf( "\n>>> major frame %lu of %lu complete <<<\n\n",
                ( unsigned long ) ulFrames,
                ( unsigned long ) DEMO_FRAMES );

        if( DEMO_FRAMES > 0 && ulFrames >= DEMO_FRAMES )
        {
            /* ---- CPU usage summary (backs spec's <=10% overhead claim) ---- */
            uint32_t ulElapsedTicks = ( uint32_t )
                                      ( xTaskGetTickCount() - xStartTick );
            uint32_t ulIdleDelta    = ulTimelineGetTrueIdleTicks() - ulStartIdle;
            uint32_t ulBusyTicks    = ( ulElapsedTicks > ulIdleDelta ) ?
                                      ( ulElapsedTicks - ulIdleDelta ) : 0;
            /* fixed-point %: x10 to give one decimal place                  */
            uint32_t ulOverheadX10  = ( ulElapsedTicks > 0 ) ?
                                ( ( ulBusyTicks * 1000UL ) / ulElapsedTicks ) : 0;

            printf( "\n=== DEMO FINISHED: %lu major frames executed ===\n",
                    ( unsigned long ) ulFrames );
#if BENCHMARK_MODE
            printf( "\n--- CPU usage (BENCHMARK MODE: no-op tasks, no live trace) ---\n" );
            printf( "  busy time below = pure scheduler infrastructure overhead\n" );
#else
            printf( "\n--- CPU usage (DEMO MODE: CPU-bound tasks + live trace) ---\n" );
            printf( "  busy time below includes task bodies AND UART transmission;\n" );
            printf( "  build with `make BENCHMARK=1 run` for pure scheduler overhead.\n" );
#endif
            printf( "  measured window : %lu ticks\n",
                    ( unsigned long ) ulElapsedTicks );
            printf( "  true idle ticks : %lu (FreeRTOS idle task ran)\n",
                    ( unsigned long ) ulIdleDelta );
            printf( "  busy   ticks    : %lu\n",
                    ( unsigned long ) ulBusyTicks );
            printf( "  CPU usage       : %lu.%lu %%  (spec target: scheduler <= 10%%)\n",
                    ( unsigned long )( ulOverheadX10 / 10 ),
                    ( unsigned long )( ulOverheadX10 % 10 ) );
            printf( "  last-frame non-HRT ticks: %lu / %lu\n",
                    ( unsigned long ) ulTimelineGetNonHrtTicks(),
                    ( unsigned long ) xSpec.ulMajorFrameTicks );

            /* Stop recording so the dump itself doesn't get logged, then dump
             * every tick + event captured during the run. */
            vTraceRecordEnable( 0 );
            vTraceDumpAll();
            printf( "Stopping QEMU...\n" );
            prvQemuExit( 0 );
        }
    }
}

int main( void )
{
#if BENCHMARK_MODE
    printf( "\n=== Project 1: Timeline Scheduler [BENCHMARK MODE] ===\n" );
    printf( "Task bodies are no-ops, live trace is OFF.\n" );
    printf( "CPU summary at end reports pure scheduler overhead.\n" );
#else
    printf( "\n=== Project 1: Timeline Scheduler (thin kernel patch) ===\n" );
#endif
    printf( "Major frame = %lu ticks, sub-frame = %lu ticks, running %lu frames\n",
            ( unsigned long ) xSpec.ulMajorFrameTicks,
            ( unsigned long ) xSpec.ulSubframeTicks,
            ( unsigned long ) DEMO_FRAMES );
    printf( "Per-tick history is captured in RAM and dumped at the end.\n\n" );

    /* Enable in-RAM recording BEFORE the engine starts so every tick from
     * frame 0 is captured. The recorder is non-blocking and bounded by
     * traceRECORD_CAPACITY. */
    vTraceRecordReset();
    vTraceRecordEnable( 1 );

    /* Configure the engine but DON'T start the kernel yet - we need to add
     * the supervisor task first. */
    if( vConfigureScheduler( &xSpec ) != pdPASS )
    {
        printf( "FATAL: timeline configuration failed\n" );
        for( ;; ) { }
    }

    /* Supervisor runs at a priority below the engine and the HRTs so it never
     * interferes with the schedule. It just counts frames and prints. */
    if( xTaskCreate( prvDemoSupervisor, "SUPER",
                     configMINIMAL_STACK_SIZE * 2, NULL,
                     ( tskIDLE_PRIORITY + 2 ), NULL ) != pdPASS )
    {
        printf( "FATAL: supervisor create failed\n" );
        for( ;; ) { }
    }

    vTaskStartScheduler();
    for( ;; ) { }
    return 0;
}

/* ---- required FreeRTOS hooks ---- */
void vApplicationMallocFailedHook( void )
{
    printf( "HOOK: malloc failed\n" );
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

void vApplicationStackOverflowHook( TaskHandle_t xTask, char *pcTaskName )
{
    ( void ) xTask;
    printf( "HOOK: stack overflow in %s\n", pcTaskName ? pcTaskName : "?" );
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

/* Called from the FreeRTOS idle task when NO application task is ready. The
 * accountant bumps the engine's cumulative true-idle counter, which the
 * supervisor uses to compute and report CPU overhead. */
void vApplicationIdleHook( void )
{
    vTimelineIdleTickAccount();
}

void vApplicationTickHook( void ) { }

void vAssertCalled( void )
{
    printf( "HOOK: configASSERT failed\n" );
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

/* ---- Timeline scheduler error hooks (application overrides) ----
 * These override the weak defaults in timeline_engine.c. They run in task
 * context (engine task or the failing API's caller), so any FreeRTOS API may
 * be used. Demonstration uses printf; a real app might count misses, set a
 * fault LED, latch into a safe state, etc.                                  */
void vApplicationDeadlineMissHook( const char *pcTaskName, uint32_t ulTick )
{
    printf( "HOOK: deadline miss - task %s terminated at tick %lu\n",
            pcTaskName ? pcTaskName : "?", ( unsigned long ) ulTick );
}

void vApplicationTaskCreateFailedHook( const char *pcTaskName, TaskClass_t eClass )
{
    printf( "HOOK: xTaskCreate failed for %s task '%s'\n",
            ( eClass == HARD_RT ) ? "HRT" : "SRT",
            pcTaskName ? pcTaskName : "?" );
}

void vApplicationScheduleErrorHook( TimelineError_t eError, const char *pcDetail )
{
    static const char *names[] = {
        "INVALID_SPEC", "SEMAPHORE_ALLOC", "TABLE_BUILD", "ENGINE_TASK_CREATE"
    };
    printf( "HOOK: schedule error %s (%s)\n",
            names[ eError ], pcDetail ? pcDetail : "" );
}

/* configSUPPORT_STATIC_ALLOCATION == 1 providers. */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer,
                                    StackType_t **ppxIdleTaskStackBuffer,
                                    uint32_t *pulIdleTaskStackSize )
{
    static StaticTask_t xIdleTCB;
    static StackType_t  xIdleStack[ configMINIMAL_STACK_SIZE ];
    *ppxIdleTaskTCBBuffer   = &xIdleTCB;
    *ppxIdleTaskStackBuffer = xIdleStack;
    *pulIdleTaskStackSize   = configMINIMAL_STACK_SIZE;
}

void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize )
{
    static StaticTask_t xTimerTCB;
    static StackType_t  xTimerStack[ configTIMER_TASK_STACK_DEPTH ];
    *ppxTimerTaskTCBBuffer   = &xTimerTCB;
    *ppxTimerTaskStackBuffer = xTimerStack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}