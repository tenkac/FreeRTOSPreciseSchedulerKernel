/*
 * trace.h
 *
 * Tick-level trace / monitoring for the timeline scheduler.
 * All output goes to UART0 via printf (see uart_io.c).
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#ifndef TRACE_H
#define TRACE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
    TRACE_FRAME_START = 0,  /* start of a new major frame                   */
    TRACE_TICK,             /* per-tick heartbeat with state snapshot       */
    TRACE_RELEASE,          /* HRT slot opens                                */
    TRACE_START,            /* task actually begins executing                */
    TRACE_COMPLETE,         /* task returned within its deadline             */
    TRACE_DEADLINE_MISS,    /* HRT exceeded its end tick -> terminated       */
    TRACE_SRT_START,        /* SRT best-effort task begins                   */
    TRACE_SRT_COMPLETE,     /* SRT best-effort task finishes                 */
    TRACE_IDLE              /* CPU idle tick(s)                              */
} TraceEvent_t;

/* enable/disable at runtime (set by the scheduler from the config) */
void vTraceEnable( uint8_t ucEnable );

/* emit one trace record: [ <tick> ms ] NAME EVENT (extra) */
void vTraceLog( uint32_t ulTick, TraceEvent_t eEvent, const char *pcName, uint32_t ulExtra );

/* called once per major frame to report accumulated idle ticks */
void vTraceIdleReport( uint32_t ulTick, uint32_t ulIdleTicks );

/* -------------------------------------------------------------------------- */
/* In-memory event recorder (used by the C test suite to inspect behaviour).  */
/* Every vTraceLog() call is also captured here when recording is enabled.    */
/* -------------------------------------------------------------------------- */
#ifndef traceRECORD_CAPACITY
    /* Sized to hold a full demo run with per-tick records:
     *   ~1 TICK per tick + a few event records per frame.
     *   DEMO_FRAMES=5, frame=50 ticks -> ~300-400 records expected. */
    #define traceRECORD_CAPACITY    1024
#endif

typedef struct
{
    uint32_t     ulTick;
    TraceEvent_t eEvent;
    const char  *pcName;   /* points at the slot's static name string */
    uint32_t     ulExtra;
} TraceRecord_t;

/* Clear the recorder and start (or stop) capturing events. */
void vTraceRecordReset( void );
void vTraceRecordEnable( uint8_t ucEnable );

/* Number of records captured so far (saturates at traceRECORD_CAPACITY). */
uint32_t ulTraceRecordCount( void );

/* Access record i (0-based). Returns NULL if out of range. */
const TraceRecord_t *pxTraceRecordGet( uint32_t ulIndex );

/* Find the FIRST record matching (event, name) at/after ulFromIndex.
 * pcName may be NULL to match any name. Returns the index, or -1. */
int32_t lTraceRecordFind( TraceEvent_t eEvent, const char *pcName,
                          uint32_t ulFromIndex );

/* Dump every recorded event to UART in chronological order. Safe to call from
 * a low-priority task after a run (NOT during the run - heavy printf load). */
void vTraceDumpAll( void );

/* TRACE_TICK encoding in ulExtra:
 *    bit 31 ........ HRT_OPEN flag (1 = an HRT window is active at this tick)
 *    bits 30..16 ... reserved (0)
 *    bits 15..0  ... frame offset (0 .. major_frame-1)
 * Helpers to pack/unpack:                                                   */
#define traceTICK_PACK( ulOffset, ucHrtOpen ) \
    ( ( ( uint32_t )( ulOffset ) & 0xFFFFu ) | \
      ( ( ucHrtOpen ) ? 0x80000000u : 0u ) )
#define traceTICK_OFFSET( ulExtra )   ( ( ulExtra ) & 0xFFFFu )
#define traceTICK_HRT_OPEN( ulExtra ) ( ( ( ulExtra ) >> 31 ) & 1u )

#ifdef __cplusplus
}
#endif

#endif /* TRACE_H */