/* Placeholder trace.h for phases 3 -> 4.
 * The real implementation lands in phase 4 (feature/trace, owner: C).
 * Until then, no-op inline stubs let the engine compile and link. */
#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

typedef enum
{
    TRACE_FRAME_START = 0,
    TRACE_TICK,
    TRACE_RELEASE,
    TRACE_START,
    TRACE_COMPLETE,
    TRACE_DEADLINE_MISS,
    TRACE_SRT_START,
    TRACE_SRT_COMPLETE,
    TRACE_IDLE
} TraceEvent_t;

static inline void vTraceEnable( uint8_t e )       { ( void ) e; }
static inline void vTraceLog( uint32_t t, TraceEvent_t e,
                               const char *n, uint32_t x )
    { ( void ) t; ( void ) e; ( void ) n; ( void ) x; }
static inline void vTraceIdleReport( uint32_t t, uint32_t i )
    { ( void ) t; ( void ) i; }

#define traceTICK_PACK( off, hrt )   ( ( uint32_t ) 0 )

#endif