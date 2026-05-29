/*
 * trace.c  -  tick-level trace implementation (UART/printf backend)
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#include <stdio.h>
#include "trace.h"

static uint8_t s_ucTraceEnabled = 1;

/* ---- in-memory event recorder (for the C test suite) ---- */
static TraceRecord_t s_xRecords[ traceRECORD_CAPACITY ];
static uint32_t      s_ulRecordCount   = 0;
static uint8_t       s_ucRecordEnabled = 0;

void vTraceRecordReset( void )
{
    s_ulRecordCount = 0;
}

void vTraceRecordEnable( uint8_t ucEnable )
{
    s_ucRecordEnabled = ucEnable;
}

uint32_t ulTraceRecordCount( void )
{
    return s_ulRecordCount;
}

const TraceRecord_t *pxTraceRecordGet( uint32_t ulIndex )
{
    if( ulIndex >= s_ulRecordCount )
    {
        return NULL;
    }
    return &s_xRecords[ ulIndex ];
}

static uint8_t prvNameEq( const char *a, const char *b )
{
    if( a == b )      return 1;
    if( !a || !b )    return 0;
    while( *a && ( *a == *b ) ) { a++; b++; }
    return ( *a == *b ) ? 1 : 0;
}

int32_t lTraceRecordFind( TraceEvent_t eEvent, const char *pcName,
                          uint32_t ulFromIndex )
{
    uint32_t i;
    for( i = ulFromIndex; i < s_ulRecordCount; i++ )
    {
        if( s_xRecords[ i ].eEvent == eEvent &&
            ( pcName == NULL || prvNameEq( s_xRecords[ i ].pcName, pcName ) ) )
        {
            return ( int32_t ) i;
        }
    }
    return -1;
}

static void prvRecord( uint32_t ulTick, TraceEvent_t eEvent,
                       const char *pcName, uint32_t ulExtra )
{
    if( s_ucRecordEnabled == 0 || s_ulRecordCount >= traceRECORD_CAPACITY )
    {
        return;
    }
    s_xRecords[ s_ulRecordCount ].ulTick  = ulTick;
    s_xRecords[ s_ulRecordCount ].eEvent  = eEvent;
    s_xRecords[ s_ulRecordCount ].pcName  = pcName;
    s_xRecords[ s_ulRecordCount ].ulExtra = ulExtra;
    s_ulRecordCount++;
}

void vTraceEnable( uint8_t ucEnable )
{
    s_ucTraceEnabled = ucEnable;
}

static const char *prvEventStr( TraceEvent_t eEvent )
{
    switch( eEvent )
    {
        case TRACE_FRAME_START:    return "FRAME_START";
        case TRACE_TICK:           return "TICK";
        case TRACE_RELEASE:        return "RELEASE";
        case TRACE_START:          return "START";
        case TRACE_COMPLETE:       return "COMPLETE";
        case TRACE_DEADLINE_MISS:  return "DEADLINE_MISS -> TERMINATED";
        case TRACE_SRT_START:      return "SRT_START";
        case TRACE_SRT_COMPLETE:   return "SRT_COMPLETE";
        case TRACE_IDLE:           return "IDLE";
        default:                   return "?";
    }
}

void vTraceLog( uint32_t ulTick, TraceEvent_t eEvent, const char *pcName, uint32_t ulExtra )
{
    /* Always feed the recorder (independent of UART printing). */
    prvRecord( ulTick, eEvent, pcName, ulExtra );

    if( s_ucTraceEnabled == 0 )
    {
        return;
    }

    /* TRACE_TICK is high-frequency (every tick) - never print it live, only
     * record it. Use vTraceDumpAll() at end-of-run to view tick history. */
    if( eEvent == TRACE_TICK )
    {
        return;
    }

    if( eEvent == TRACE_DEADLINE_MISS )
    {
        printf( "[ %5lu ms ] %-10s %s (deadline @ tick %lu)\n",
                (unsigned long) ulTick,
                pcName ? pcName : "-",
                prvEventStr( eEvent ),
                (unsigned long) ulExtra );
    }
    else if( eEvent == TRACE_RELEASE )
    {
        printf( "[ %5lu ms ] %-10s %s (sub-frame %lu)\n",
                (unsigned long) ulTick,
                pcName ? pcName : "-",
                prvEventStr( eEvent ),
                (unsigned long) ulExtra );
    }
    else
    {
        printf( "[ %5lu ms ] %-10s %s\n",
                (unsigned long) ulTick,
                pcName ? pcName : "-",
                prvEventStr( eEvent ) );
    }
}

void vTraceIdleReport( uint32_t ulTick, uint32_t ulIdleTicks )
{
    if( s_ucTraceEnabled == 0 )
    {
        return;
    }
    printf( "[ %5lu ms ] %-10s IDLE = %lu ticks\n",
            (unsigned long) ulTick, "CPU", (unsigned long) ulIdleTicks );
}

/* -------------------------------------------------------------------------- */
/* Bulk dump of the in-RAM recorder. Call from a low-priority task AFTER the  */
/* scheduler has finished its measured run - printf during a run would saturate*/
/* the UART and break tick timing.                                            */
/* -------------------------------------------------------------------------- */
void vTraceDumpAll( void )
{
    uint32_t i;

    printf( "\n========== TRACE DUMP (%lu records) ==========\n",
            (unsigned long) s_ulRecordCount );

    for( i = 0; i < s_ulRecordCount; i++ )
    {
        const TraceRecord_t *r = &s_xRecords[ i ];
        if( r->eEvent == TRACE_TICK )
        {
            uint32_t ulHrtOpen = traceTICK_HRT_OPEN( r->ulExtra );
            /* Skip inactive ticks (no HRT running) to keep the dump compact;
             * the events around them already tell that story. */
            if( ulHrtOpen == 0 )
            {
                continue;
            }
            printf( "[ %5lu ms ] TICK       offset=%lu (HRT active)\n",
                    (unsigned long) r->ulTick,
                    (unsigned long) traceTICK_OFFSET( r->ulExtra ) );
        }
        else if( r->eEvent == TRACE_DEADLINE_MISS )
        {
            printf( "[ %5lu ms ] %-10s %s (deadline @ tick %lu)\n",
                    (unsigned long) r->ulTick,
                    r->pcName ? r->pcName : "-",
                    prvEventStr( r->eEvent ),
                    (unsigned long) r->ulExtra );
        }
        else if( r->eEvent == TRACE_RELEASE )
        {
            printf( "[ %5lu ms ] %-10s %s (sub-frame %lu)\n",
                    (unsigned long) r->ulTick,
                    r->pcName ? r->pcName : "-",
                    prvEventStr( r->eEvent ),
                    (unsigned long) r->ulExtra );
        }
        else
        {
            printf( "[ %5lu ms ] %-10s %s\n",
                    (unsigned long) r->ulTick,
                    r->pcName ? r->pcName : "-",
                    prvEventStr( r->eEvent ) );
        }
    }

    printf( "========== END OF TRACE ==========\n" );
}