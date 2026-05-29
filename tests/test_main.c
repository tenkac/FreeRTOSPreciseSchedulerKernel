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
 * test_main.c  -  C test suite for the timeline scheduler.
 *
 * All scenarios run back-to-back in a single firmware boot. A runner task
 * configures the engine with the next scenario, waits long enough for it to
 * execute, then asserts on the recorded events (see trace_record API).
 *
 * Build via tests target; output is printed to UART as
 *     [PASS]/[FAIL] <test name>: <message>
 * with a final summary.
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "timeline_engine.h"
#include "trace.h"

/* -------------------------------------------------------------------------- */
/* Test infrastructure                                                        */
/* -------------------------------------------------------------------------- */
static uint32_t s_ulTestsRun     = 0;
static uint32_t s_ulTestsPassed  = 0;

#define TEST_REPORT( name, ok, fmt, ... )                                    \
    do {                                                                     \
        s_ulTestsRun++;                                                      \
        if( (ok) ) {                                                         \
            s_ulTestsPassed++;                                               \
            printf( "[PASS] %-22s : " fmt "\n", (name), ##__VA_ARGS__ );     \
        } else {                                                             \
            printf( "[FAIL] %-22s : " fmt "\n", (name), ##__VA_ARGS__ );     \
        }                                                                    \
    } while( 0 )

/* helper: count records matching (event, name) */
static uint32_t prvCount( TraceEvent_t e, const char *name )
{
    uint32_t total = ulTraceRecordCount();
    uint32_t i, n = 0;
    for( i = 0; i < total; i++ )
    {
        const TraceRecord_t *r = pxTraceRecordGet( i );
        if( r->eEvent == e && ( name == NULL || strcmp( r->pcName, name ) == 0 ) )
        {
            n++;
        }
    }
    return n;
}

/* -------------------------------------------------------------------------- */
/* Shared task bodies (used by several scenarios)                             */
/* -------------------------------------------------------------------------- */
static void prvBurn( uint32_t ulTicks )
{
    TickType_t xS = xTaskGetTickCount();
    while( ( xTaskGetTickCount() - xS ) < ulTicks )
    {
        __asm volatile ( "nop" );
    }
}

static void GoodHRT( void *pv )  { ( void ) pv; prvBurn( 3 ); }    /* fits  */
static void BadHRT( void *pv )   { ( void ) pv; prvBurn( 8 ); }    /* overruns */
static void SrtBody( void *pv )  { ( void ) pv; prvBurn( 2 ); }

/* -------------------------------------------------------------------------- */
/* Scenario 1: HRT completes within its window                                */
/* -------------------------------------------------------------------------- */
static const TimelineSlot_t xSlots_S1[] = {
    { "G", GoodHRT, NULL, HARD_RT, 10, 20, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S1 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S1, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

static void prvAssert_S1( void )
{
    uint32_t comp = prvCount( TRACE_COMPLETE,      "G" );
    uint32_t miss = prvCount( TRACE_DEADLINE_MISS, "G" );
    TEST_REPORT( "HRT in window",
                 comp >= 2 && miss == 0,
                 "%lu COMPLETE, %lu DEADLINE_MISS",
                 (unsigned long) comp, (unsigned long) miss );
}

/* -------------------------------------------------------------------------- */
/* Scenario 2: HRT deadline-miss is detected and terminated                   */
/* -------------------------------------------------------------------------- */
static const TimelineSlot_t xSlots_S2[] = {
    { "B", BadHRT, NULL, HARD_RT, 10, 15, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S2 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S2, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

static void prvAssert_S2( void )
{
    uint32_t comp = prvCount( TRACE_COMPLETE,      "B" );
    uint32_t miss = prvCount( TRACE_DEADLINE_MISS, "B" );
    TEST_REPORT( "HRT deadline-miss",
                 miss >= 2 && comp == 0,
                 "%lu DEADLINE_MISS, %lu COMPLETE",
                 (unsigned long) miss, (unsigned long) comp );
}

/* -------------------------------------------------------------------------- */
/* Scenario 3: SRT runs in fixed order during idle                            */
/* -------------------------------------------------------------------------- */
static const TimelineSlot_t xSlots_S3[] = {
    { "X", SrtBody, NULL, SOFT_RT, 0, 0, 0, 0, configMINIMAL_STACK_SIZE },
    { "Y", SrtBody, NULL, SOFT_RT, 0, 0, 0, 1, configMINIMAL_STACK_SIZE },
    { "Z", SrtBody, NULL, SOFT_RT, 0, 0, 0, 2, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S3 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S3, .ulSlotCount = 3, .ucTraceEnabled = 0,
};

static void prvAssert_S3( void )
{
    /* Find the first FRAME_START in the recorded window, then search for the
     * three SRT_STARTs after it. Skipping records before the first frame
     * boundary avoids being fooled by a partial chain captured at the tail
     * end of the previous scenario. */
    int32_t iFrame = lTraceRecordFind( TRACE_FRAME_START, NULL, 0 );
    uint32_t ulFrom = ( iFrame >= 0 ) ? ( uint32_t )( iFrame + 1 ) : 0;
    int32_t ix = lTraceRecordFind( TRACE_SRT_START, "X", ulFrom );
    int32_t iy = lTraceRecordFind( TRACE_SRT_START, "Y", ulFrom );
    int32_t iz = lTraceRecordFind( TRACE_SRT_START, "Z", ulFrom );
    int ok = ( ix >= 0 && iy > ix && iz > iy );
    TEST_REPORT( "SRT fixed order",
                 ok, "X@%ld < Y@%ld < Z@%ld",
                 (long) ix, (long) iy, (long) iz );
}

/* -------------------------------------------------------------------------- */
/* Scenario 4: Frame reset / deterministic replay                             */
/* -------------------------------------------------------------------------- */
/* Reuse S1 (one HRT). Run 4 frames, expect >=4 FRAME_START and >=4 COMPLETEs
 * for the HRT, all at the same offset within their respective frames. */
static void prvAssert_S4( void )
{
    uint32_t frames = prvCount( TRACE_FRAME_START, NULL );
    uint32_t total  = ulTraceRecordCount();
    uint32_t i;
    int      ok = ( frames >= 4 );

    /* Verify offset-within-frame is identical for every COMPLETE of "G".
     * Skip records before the first FRAME_START: recording starts mid-frame
     * (the runner's vTaskDelay isn't aligned to engine frame boundaries), so
     * the first few records have no frame anchor. */
    uint32_t ulFrameStart  = 0;
    int      iAnchored     = 0;
    int32_t  lExpectedOff = -1;
    for( i = 0; i < total && ok; i++ )
    {
        const TraceRecord_t *r = pxTraceRecordGet( i );
        if( r->eEvent == TRACE_FRAME_START )
        {
            ulFrameStart = r->ulTick;
            iAnchored    = 1;
        }
        else if( iAnchored &&
                 r->eEvent == TRACE_COMPLETE &&
                 strcmp( r->pcName, "G" ) == 0 )
        {
            int32_t off = ( int32_t ) ( r->ulTick - ulFrameStart );
            if( lExpectedOff < 0 ) lExpectedOff = off;
            else if( off != lExpectedOff ) ok = 0;
        }
    }
    TEST_REPORT( "Frame replay",
                 ok, "%lu frames, COMPLETE always @offset %ld",
                 (unsigned long) frames, (long) lExpectedOff );
}

/* -------------------------------------------------------------------------- */
/* Scenario 5: Release jitter <= 1 tick                                       */
/* -------------------------------------------------------------------------- */
/* The engine wakes on every tick; HRT_A release should land within 1 tick
 * of its configured ulStartOffset (=10) each frame. */
static void prvAssert_S5( void )
{
    uint32_t total = ulTraceRecordCount();
    uint32_t i;
    uint32_t ulFrameStart = 0;
    int      iAnchored    = 0;
    int32_t  lMaxJitter   = 0;
    uint32_t ulSamples    = 0;
    int      ok = 1;

    for( i = 0; i < total; i++ )
    {
        const TraceRecord_t *r = pxTraceRecordGet( i );
        if( r->eEvent == TRACE_FRAME_START )
        {
            ulFrameStart = r->ulTick;
            iAnchored    = 1;
        }
        else if( iAnchored &&
                 r->eEvent == TRACE_RELEASE &&
                 strcmp( r->pcName, "G" ) == 0 )
        {
            int32_t off    = ( int32_t ) ( r->ulTick - ulFrameStart );
            int32_t jitter = off - 10;
            if( jitter < 0 ) jitter = -jitter;
            if( jitter > lMaxJitter ) lMaxJitter = jitter;
            ulSamples++;
            if( jitter > 1 ) ok = 0;
        }
    }
    TEST_REPORT( "Release jitter",
                 ok && ulSamples > 0,
                 "max jitter %ld tick over %lu releases",
                 (long) lMaxJitter, (unsigned long) ulSamples );
}

/* -------------------------------------------------------------------------- */
/* Scenario 6: STRESS - many HRTs across all sub-frames                       */
/* -------------------------------------------------------------------------- */
/* Five HRTs, one per sub-frame, each running a short body inside its window.
 * Asserts every HRT completes every frame with zero deadline misses across
 * multiple frames -> proves the scheduler handles a densely-loaded timeline. */
static void StressBody( void *pv ) { ( void ) pv; prvBurn( 2 ); }

static const TimelineSlot_t xSlots_S6[] = {
    { "H0", StressBody, NULL, HARD_RT,  2,  8, 0, 0, configMINIMAL_STACK_SIZE },
    { "H1", StressBody, NULL, HARD_RT, 12, 18, 1, 0, configMINIMAL_STACK_SIZE },
    { "H2", StressBody, NULL, HARD_RT, 22, 28, 2, 0, configMINIMAL_STACK_SIZE },
    { "H3", StressBody, NULL, HARD_RT, 32, 38, 3, 0, configMINIMAL_STACK_SIZE },
    { "H4", StressBody, NULL, HARD_RT, 42, 48, 4, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S6 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S6, .ulSlotCount = 5, .ucTraceEnabled = 0,
};

static void prvAssert_S6( void )
{
    uint32_t miss  = prvCount( TRACE_DEADLINE_MISS, NULL );
    uint32_t completes[ 5 ];
    const char *names[5] = { "H0", "H1", "H2", "H3", "H4" };
    uint32_t i;
    int ok = ( miss == 0 );

    for( i = 0; i < 5; i++ )
    {
        completes[ i ] = prvCount( TRACE_COMPLETE, names[ i ] );
        if( completes[ i ] < 3 ) ok = 0;   /* >=3 frames each */
    }

    TEST_REPORT( "Stress 5 HRTs",
                 ok,
                 "miss=%lu  completes H0..H4 = %lu/%lu/%lu/%lu/%lu",
                 (unsigned long) miss,
                 (unsigned long) completes[0], (unsigned long) completes[1],
                 (unsigned long) completes[2], (unsigned long) completes[3],
                 (unsigned long) completes[4] );
}

/* -------------------------------------------------------------------------- */
/* Scenario 7: EDGE - 1-tick HRT window                                       */
/* -------------------------------------------------------------------------- */
/* Tightest possible HRT window. Body must complete inside a single tick or
 * be terminated. We use a no-op body so it completes; assert zero misses
 * and at least one COMPLETE per frame.                                       */
static void NoopBody( void *pv ) { ( void ) pv; }

static const TimelineSlot_t xSlots_S7[] = {
    { "T", NoopBody, NULL, HARD_RT, 10, 11, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S7 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S7, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

static void prvAssert_S7( void )
{
    uint32_t miss = prvCount( TRACE_DEADLINE_MISS, "T" );
    uint32_t comp = prvCount( TRACE_COMPLETE,      "T" );
    TEST_REPORT( "Edge 1-tick window",
                 miss == 0 && comp >= 2,
                 "1-tick body completes (%lu COMPLETE, %lu MISS)",
                 (unsigned long) comp, (unsigned long) miss );
}

/* -------------------------------------------------------------------------- */
/* Scenario 8: EDGE - back-to-back HRTs (zero gap between windows)            */
/* -------------------------------------------------------------------------- */
/* H1 [10,20) ends exactly when H2 [20,30) starts. Both must release each
 * frame; neither must overlap the other; both must complete cleanly.        */
static const TimelineSlot_t xSlots_S8[] = {
    { "A1", StressBody, NULL, HARD_RT, 10, 20, 1, 0, configMINIMAL_STACK_SIZE },
    { "A2", StressBody, NULL, HARD_RT, 20, 30, 2, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S8 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S8, .ulSlotCount = 2, .ucTraceEnabled = 0,
};

static void prvAssert_S8( void )
{
    uint32_t miss = prvCount( TRACE_DEADLINE_MISS, NULL );
    uint32_t c1   = prvCount( TRACE_COMPLETE, "A1" );
    uint32_t c2   = prvCount( TRACE_COMPLETE, "A2" );

    /* Verify A1's COMPLETE always precedes A2's RELEASE in the same frame:
     * scan the trace, after every A1 COMPLETE the next A2 event must be a
     * RELEASE (not START - we should not see A2 starting while A1 lived). */
    uint32_t total = ulTraceRecordCount();
    uint32_t i;
    int ordering_ok = 1;
    for( i = 0; i < total; i++ )
    {
        const TraceRecord_t *r = pxTraceRecordGet( i );
        if( r->eEvent == TRACE_RELEASE && strcmp( r->pcName, "A2" ) == 0 )
        {
            /* find the most recent prior event for A1; must be COMPLETE
             * (not START), otherwise A1 and A2 overlapped */
            uint32_t j;
            int found_complete = 0;
            for( j = i; j > 0; j-- )
            {
                const TraceRecord_t *p = pxTraceRecordGet( j - 1 );
                if( strcmp( p->pcName, "A1" ) == 0 )
                {
                    if( p->eEvent == TRACE_COMPLETE ) found_complete = 1;
                    break;
                }
            }
            if( !found_complete ) { ordering_ok = 0; break; }
        }
    }

    TEST_REPORT( "Edge back-to-back",
                 miss == 0 && c1 >= 2 && c2 >= 2 && ordering_ok,
                 "A1=%lu  A2=%lu  miss=%lu  ordered=%d",
                 (unsigned long) c1, (unsigned long) c2,
                 (unsigned long) miss, ordering_ok );
}

/* -------------------------------------------------------------------------- */
/* Scenario 9: EDGE - HRT at frame edges (start at 0, end at frame boundary)  */
/* -------------------------------------------------------------------------- */
static const TimelineSlot_t xSlots_S9[] = {
    { "E0", StressBody, NULL, HARD_RT,  0,  5, 0, 0, configMINIMAL_STACK_SIZE },
    { "E1", StressBody, NULL, HARD_RT, 45, 50, 4, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_S9 = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xSlots_S9, .ulSlotCount = 2, .ucTraceEnabled = 0,
};

static void prvAssert_S9( void )
{
    uint32_t e0_comp = prvCount( TRACE_COMPLETE, "E0" );
    uint32_t e1_comp = prvCount( TRACE_COMPLETE, "E1" );
    uint32_t miss    = prvCount( TRACE_DEADLINE_MISS, NULL );
    TEST_REPORT( "Edge frame boundaries",
                 miss == 0 && e0_comp >= 2 && e1_comp >= 2,
                 "E0=%lu  E1=%lu  miss=%lu",
                 (unsigned long) e0_comp,
                 (unsigned long) e1_comp,
                 (unsigned long) miss );
}

/* -------------------------------------------------------------------------- */
/* Static checks (no scenario run needed): config validation                  */
/* -------------------------------------------------------------------------- */
static void GoodBody( void *pv ) { ( void ) pv; }

/* Bad spec A: HRT windows overlap. */
static const TimelineSlot_t xBadOverlap[] = {
    { "P", GoodBody, NULL, HARD_RT, 10, 20, 1, 0, configMINIMAL_STACK_SIZE },
    { "Q", GoodBody, NULL, HARD_RT, 15, 25, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadOverlap = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadOverlap, .ulSlotCount = 2, .ucTraceEnabled = 0,
};

/* Bad spec B: HRT end exceeds major frame. */
static const TimelineSlot_t xBadOOB[] = {
    { "P", GoodBody, NULL, HARD_RT, 40, 60, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadOOB = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadOOB, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

/* Bad spec C: end <= start. */
static const TimelineSlot_t xBadInv[] = {
    { "P", GoodBody, NULL, HARD_RT, 20, 15, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadInv = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadInv, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

/* Bad spec D: HRT window crosses a sub-frame boundary.
 * sub-frame 10 ticks wide; ulSubframeId=1 spans [10,20); [15,22) leaks into 2. */
static const TimelineSlot_t xBadSubCross[] = {
    { "P", GoodBody, NULL, HARD_RT, 15, 22, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadSubCross = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadSubCross, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

/* Bad spec E: declared sub-frame ID disagrees with the actual offset.
 * Window [12,18) lives in sub-frame 1 but the slot claims sub-frame 2. */
static const TimelineSlot_t xBadSubId[] = {
    { "P", GoodBody, NULL, HARD_RT, 12, 18, 2, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadSubId = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadSubId, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

/* Bad spec F: sub-frame ID out of range (only 5 sub-frames in 50/10). */
static const TimelineSlot_t xBadSubRange[] = {
    { "P", GoodBody, NULL, HARD_RT, 10, 15, 7, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xSpec_BadSubRange = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xBadSubRange, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

static void prvStaticChecks( void )
{
    BaseType_t a = xTimelineEngineValidate( &xSpec_BadOverlap );
    BaseType_t b = xTimelineEngineValidate( &xSpec_BadOOB );
    BaseType_t c = xTimelineEngineValidate( &xSpec_BadInv );
    BaseType_t d = xTimelineEngineValidate( &xSpec_S1 );  /* good spec */
    BaseType_t e = xTimelineEngineValidate( &xSpec_BadSubCross );
    BaseType_t f = xTimelineEngineValidate( &xSpec_BadSubId );
    BaseType_t g = xTimelineEngineValidate( &xSpec_BadSubRange );
    TEST_REPORT( "Reject overlap",       a == pdFAIL, "validate=%ld", (long) a );
    TEST_REPORT( "Reject OOB end",       b == pdFAIL, "validate=%ld", (long) b );
    TEST_REPORT( "Reject end<=start",    c == pdFAIL, "validate=%ld", (long) c );
    TEST_REPORT( "Accept good spec",     d == pdPASS, "validate=%ld", (long) d );
    TEST_REPORT( "Reject sub-frame cross", e == pdFAIL, "validate=%ld", (long) e );
    TEST_REPORT( "Reject wrong sub-id",   f == pdFAIL, "validate=%ld", (long) f );
    TEST_REPORT( "Reject sub-id range",   g == pdFAIL, "validate=%ld", (long) g );
}

/* -------------------------------------------------------------------------- */
/* Runner: switch scenarios, wait, assert                                     */
/* -------------------------------------------------------------------------- */
static void prvRunScenario( const char *pcLabel,
                            const TimelineSpec_t *pxSpec,
                            uint32_t ulFramesToWait,
                            void ( *pxAssertFn )( void ) )
{
    printf( "\n----- scenario: %s -----\n", pcLabel );

    if( xTimelineEngineReload( pxSpec ) != pdPASS )
    {
        TEST_REPORT( pcLabel, 0, "reload rejected the spec" );
        return;
    }

    /* Give the engine one full frame to adopt the new spec, then start
     * recording. (The reload swaps at the NEXT frame boundary.) */
    vTaskDelay( pdMS_TO_TICKS( pxSpec->ulMajorFrameTicks ) );
    vTraceRecordReset();
    vTraceRecordEnable( 1 );

    vTaskDelay( pdMS_TO_TICKS( pxSpec->ulMajorFrameTicks * ulFramesToWait ) );

    vTraceRecordEnable( 0 );
    pxAssertFn();
}

static void prvRunnerTask( void *pvArg )
{
    ( void ) pvArg;

    printf( "\n============================================\n" );
    printf( " TIMELINE SCHEDULER - C TEST SUITE\n" );
    printf( "============================================\n" );

    /* Static checks (no engine activity required). */
    printf( "\n--- Part A: static validation ---\n" );
    prvStaticChecks();

    /* Runtime scenarios. */
    printf( "\n--- Part B: on-target behaviour ---\n" );
    prvRunScenario( "S1 in-window",         &xSpec_S1, 3, prvAssert_S1 );
    prvRunScenario( "S2 deadline-miss",     &xSpec_S2, 3, prvAssert_S2 );
    prvRunScenario( "S3 SRT order",         &xSpec_S3, 2, prvAssert_S3 );
    prvRunScenario( "S4 frame replay",      &xSpec_S1, 5, prvAssert_S4 );
    prvRunScenario( "S5 release jitter",    &xSpec_S1, 5, prvAssert_S5 );
    prvRunScenario( "S6 stress 5 HRTs",     &xSpec_S6, 4, prvAssert_S6 );
    prvRunScenario( "S7 edge 1-tick",       &xSpec_S7, 3, prvAssert_S7 );
    prvRunScenario( "S8 edge back-to-back", &xSpec_S8, 3, prvAssert_S8 );
    prvRunScenario( "S9 edge boundaries",   &xSpec_S9, 3, prvAssert_S9 );

    printf( "\n============================================\n" );
    printf( " RESULT: %lu/%lu passed\n",
            (unsigned long) s_ulTestsPassed,
            (unsigned long) s_ulTestsRun );
    printf( "============================================\n" );
    printf( "Stopping QEMU...\n" );

    /* Exit QEMU via ARM semihosting BKPT 0xAB / SYS_EXIT.
     * Requires QEMU launched with -semihosting-config enable=on. */
    {
        register int r0 __asm__ ( "r0" ) = 0x18;     /* SYS_EXIT       */
        register int r1 __asm__ ( "r1" ) = 0x20026;  /* ApplicationExit*/
        __asm__ volatile ( "bkpt 0xAB" : : "r"( r0 ), "r"( r1 ) );
    }
    for( ;; ) { vTaskDelay( pdMS_TO_TICKS( 1000 ) ); }
}

/* -------------------------------------------------------------------------- */
/* Bootstrap: identical pattern to main.c but starts with a tiny "warm-up"    */
/* spec and creates the runner task. The runner reloads scenarios in turn.    */
/* -------------------------------------------------------------------------- */
static void Warmup( void *pv ) { ( void ) pv; }

static const TimelineSlot_t xWarmupSlots[] = {
    { "W", Warmup, NULL, HARD_RT, 1, 2, 0, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t xWarmupSpec = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = xWarmupSlots, .ulSlotCount = 1, .ucTraceEnabled = 0,
};

int main( void )
{
    if( vConfigureScheduler( &xWarmupSpec ) != pdPASS )
    {
        printf( "FATAL: warmup config failed\n" );
        for( ;; ) { }
    }

    /* Runner sits just below the engine so the engine remains deterministic. */
    if( xTaskCreate( prvRunnerTask, "RUNNER",
                     configMINIMAL_STACK_SIZE * 4, NULL,
                     ( configMAX_PRIORITIES - 3 ), NULL ) != pdPASS )
    {
        printf( "FATAL: runner create failed\n" );
        for( ;; ) { }
    }

    vTaskStartScheduler();
    for( ;; ) { }
    return 0;
}

/* ---- FreeRTOS hooks (same shape as main.c) ---- */
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
void vApplicationIdleHook( void ) { }
void vApplicationTickHook( void ) { }
void vAssertCalled( void )
{
    printf( "HOOK: configASSERT failed\n" );
    taskDISABLE_INTERRUPTS();
    for( ;; );
}

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