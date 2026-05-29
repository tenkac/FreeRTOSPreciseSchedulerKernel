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
 * timeline_hook.c
 *
 * Implementation of the THIN kernel-side timeline hook.
 * See timeline_hook.h for the design rationale.
 *
 * This file is compiled as part of the kernel build (it is the only "kernel
 * patch" besides the ~6 line guarded call added to tasks.c). It deliberately
 * contains no scheduling policy.
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "timeline_hook.h"

/* ---- private state (single-writer: the tick ISR) ---- */
static TickType_t        s_xMajorFrame   = 0;     /* major frame length, ticks */
static TickType_t        s_xFrameStart   = 0;     /* absolute tick of frame 0  */
static volatile TickType_t s_xFrameOffset = 0;    /* 0 .. major_frame-1        */
static volatile uint32_t s_ulFrameNumber  = 0;    /* frames elapsed since init */
static SemaphoreHandle_t s_xTickSignal    = NULL; /* given each tick from ISR  */

void vTimelineHookInit( TickType_t xMajorFrameTicks,
                        SemaphoreHandle_t xTickSignal )
{
    s_xMajorFrame   = xMajorFrameTicks;
    s_xTickSignal   = xTickSignal;
    s_xFrameStart   = xTaskGetTickCount();
    s_xFrameOffset  = 0;
    s_ulFrameNumber = 0;
}

BaseType_t xTimelineTickHookFromISR( TickType_t xCurrentTick )
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;

    /* Not configured yet -> do nothing, request no switch. */
    if( ( s_xMajorFrame == 0 ) || ( s_xTickSignal == NULL ) )
    {
        return pdFALSE;
    }

    /* Advance frame-relative offset. This is the ENTIRE policy footprint
     * inside the kernel: pure arithmetic, no knowledge of tasks. */
    TickType_t xRel = ( xCurrentTick - s_xFrameStart ) % s_xMajorFrame;

    if( ( xRel == 0 ) && ( xCurrentTick != s_xFrameStart ) )
    {
        /* a new major frame just began */
        s_xFrameStart = xCurrentTick;
        s_ulFrameNumber++;
    }
    s_xFrameOffset = xRel;

    /* Single signal: "a tick happened, engine please run." */
    xSemaphoreGiveFromISR( s_xTickSignal, &xHigherPriorityTaskWoken );

    return xHigherPriorityTaskWoken;
}

TickType_t xTimelineGetFrameOffset( void )
{
    return s_xFrameOffset;
}

uint32_t ulTimelineGetFrameNumber( void )
{
    return s_ulFrameNumber;
}