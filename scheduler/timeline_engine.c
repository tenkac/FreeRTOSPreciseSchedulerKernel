/*
 * timeline_engine.c
 *
 * User-space timeline scheduling engine. All policy lives here.
 *
 * ============================ RUNTIME MODEL =================================
 *
 * Priorities (mechanism only, not the scheduling policy):
 *      tePRIO_ENGINE = configMAX_PRIORITIES - 1   (highest)
 *      tePRIO_HRT    = configMAX_PRIORITIES - 2
 *      tePRIO_SRT    = tskIDLE_PRIORITY + 1
 *
 * The kernel tick hook (timeline_hook.c) gives s_xTickSignal once per tick.
 * The engine task blocks on that semaphore, so it runs once per tick with
 * <= 1 tick jitter, then reads the frame offset the hook computed and drives:
 *
 *   - frame wrap (offset < previous) -> destroy & recreate all tasks
 *   - offset == HRT.start            -> spawn that HRT (preempts SRT)
 *   - offset == HRT.end & not done   -> terminate (deadline overrun)
 *   - no HRT window open             -> SRT chain runs in fixed order
 *
 * The engine never makes scheduling decisions inside the ISR; the ISR only
 * signals time. This keeps the kernel patch trivially small and portable.
 *
 * Distributed under the FreeRTOS MIT licensing schema.
 * ===========================================================================
 */

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "timeline_engine.h"
#include "timeline_hook.h"
#include "trace.h"

#define tePRIO_ENGINE   ( configMAX_PRIORITIES - 1 )
#define tePRIO_HRT      ( configMAX_PRIORITIES - 2 )
#define tePRIO_SRT      ( tskIDLE_PRIORITY + 1 )

/* -------------------------------------------------------------------------- */
/* Weak default error hooks. The application overrides any subset by defining */
/* a strong symbol of the same name in its own translation unit. Declared     */
/* __attribute__((weak)) so the strong app definition wins at link time.      */
/* -------------------------------------------------------------------------- */
__attribute__(( weak ))
void vApplicationDeadlineMissHook( const char *pcTaskName, uint32_t ulTick )
{
    ( void ) pcTaskName;
    ( void ) ulTick;
    /* default: silent. The engine already logs DEADLINE_MISS to the trace. */
}

__attribute__(( weak ))
void vApplicationTaskCreateFailedHook( const char *pcTaskName, TaskClass_t eClass )
{
    ( void ) pcTaskName;
    ( void ) eClass;
    /* default: silent. */
}

__attribute__(( weak ))
void vApplicationScheduleErrorHook( TimelineError_t eError, const char *pcDetail )
{
    ( void ) eError;
    ( void ) pcDetail;
    /* default: silent. */
}

/* Per-slot runtime control block. */
typedef struct
{
    const TimelineSlot_t *pxSlot;
    TaskHandle_t          xHandle;
    volatile uint8_t      ucStarted;
    volatile uint8_t      ucCompleted;
} SlotCB_t;

/* ---- module state ---- */
static TimelineSpec_t    s_xSpec;
static SlotCB_t          s_xHrt[ teMAX_HRT_TASKS ];
static SlotCB_t          s_xSrt[ teMAX_SRT_TASKS ];
static uint32_t          s_ulHrtCount;
static uint32_t          s_ulSrtCount;

static TaskHandle_t      s_xEngineTask;
static SemaphoreHandle_t s_xTickSignal;     /* given by kernel hook each tick   */

static volatile uint32_t s_ulSrtNext;       /* next SRT to release this frame   */
static uint32_t          s_ulNonHrtTicks;   /* ticks/frame with no HRT window   */
static uint32_t          s_ulNonHrtLatched; /* ulNonHrtTicks from last frame     */

/* Cumulative count of FreeRTOS idle-task invocations since boot. Bumped from
 * vApplicationIdleHook() via vTimelineIdleTickAccount(). Read by the demo and
 * tests to compute true CPU overhead.                                          */
static volatile uint32_t s_ulTrueIdleCount = 0;

/* Reload support: when non-NULL, the engine adopts this spec at the next
 * frame boundary, then clears it. Written by xTimelineEngineReload(). */
static const TimelineSpec_t *volatile s_pxPendingSpec = NULL;

/* forward decls */
static void prvEngineTask( void *pvArg );
static void prvHrtTramp( void *pvArg );
static void prvSrtTramp( void *pvArg );
static void prvFrameReset( uint32_t ulNowTick );
static void prvReleaseNextSrt( void );
static SlotCB_t *prvSelfCB( void );

/* Build the HRT/SRT control-block tables from a spec. Returns pdPASS/pdFAIL.
 * Pure table setup - does not create tasks or the engine. Used by Configure
 * and by the reload path. */
static BaseType_t prvBuildTables( const TimelineSpec_t *pxSpec )
{
    uint32_t i, j;

    s_ulHrtCount = 0;
    s_ulSrtCount = 0;

    for( i = 0; i < pxSpec->ulSlotCount; i++ )
    {
        const TimelineSlot_t *pxS = &pxSpec->pxSlots[ i ];
        if( pxS->eClass == HARD_RT )
        {
            if( s_ulHrtCount >= teMAX_HRT_TASKS ) return pdFAIL;
            s_xHrt[ s_ulHrtCount ].pxSlot      = pxS;
            s_xHrt[ s_ulHrtCount ].xHandle     = NULL;
            s_xHrt[ s_ulHrtCount ].ucStarted   = 0;
            s_xHrt[ s_ulHrtCount ].ucCompleted = 0;
            s_ulHrtCount++;
        }
        else
        {
            if( s_ulSrtCount >= teMAX_SRT_TASKS ) return pdFAIL;
            s_xSrt[ s_ulSrtCount ].pxSlot      = pxS;
            s_xSrt[ s_ulSrtCount ].xHandle     = NULL;
            s_xSrt[ s_ulSrtCount ].ucStarted   = 0;
            s_xSrt[ s_ulSrtCount ].ucCompleted = 0;
            s_ulSrtCount++;
        }
    }

    /* Sort SRT by ulOrder (insertion sort; small N). */
    for( i = 1; i < s_ulSrtCount; i++ )
    {
        SlotCB_t xKey = s_xSrt[ i ];
        j = i;
        while( j > 0 && s_xSrt[ j - 1 ].pxSlot->ulOrder > xKey.pxSlot->ulOrder )
        {
            s_xSrt[ j ] = s_xSrt[ j - 1 ];
            j--;
        }
        s_xSrt[ j ] = xKey;
    }

    return pdPASS;
}

/* =========================================================================== */
/* Pure spec validation (no side effects) - used by Configure and by tests.    */
/* Returns pdPASS if the spec is well-formed, pdFAIL otherwise.                 */
/* =========================================================================== */
BaseType_t xTimelineEngineValidate( const TimelineSpec_t *pxSpec )
{
    uint32_t i, j, ulHrt = 0, ulSrt = 0;

    if( pxSpec == NULL || pxSpec->pxSlots == NULL || pxSpec->ulSlotCount == 0 ||
        pxSpec->ulMajorFrameTicks == 0 || pxSpec->ulSubframeTicks == 0 )
    {
        return pdFAIL;
    }
    if( ( pxSpec->ulMajorFrameTicks % pxSpec->ulSubframeTicks ) != 0 )
    {
        return pdFAIL;
    }

    for( i = 0; i < pxSpec->ulSlotCount; i++ )
    {
        const TimelineSlot_t *pxS = &pxSpec->pxSlots[ i ];
        if( pxS->pxBody == NULL )
        {
            return pdFAIL;
        }
        if( pxS->eClass == HARD_RT )
        {
            if( ++ulHrt > teMAX_HRT_TASKS )
            {
                return pdFAIL;
            }
            if( pxS->ulEndOffset <= pxS->ulStartOffset ||
                pxS->ulEndOffset > pxSpec->ulMajorFrameTicks )
            {
                return pdFAIL;
            }

            /* Sub-frame k spans [k * sub, (k+1) * sub). End-of-window is
             * exclusive, so an HRT that ends exactly on the sub-frame
             * boundary is still considered "contained". */
            {
                uint32_t ulNumSubframes = pxSpec->ulMajorFrameTicks /
                                          pxSpec->ulSubframeTicks;
                uint32_t ulSubLo, ulSubHi;

                if( pxS->ulSubframeId >= ulNumSubframes )
                {
                    return pdFAIL;
                }
                ulSubLo = pxS->ulSubframeId * pxSpec->ulSubframeTicks;
                ulSubHi = ulSubLo + pxSpec->ulSubframeTicks;
                if( pxS->ulStartOffset < ulSubLo ||
                    pxS->ulEndOffset   > ulSubHi )
                {
                    return pdFAIL;
                }
            }
        }
        else
        {
            if( ++ulSrt > teMAX_SRT_TASKS )
            {
                return pdFAIL;
            }
        }
    }

    /* HRT windows must not overlap. */
    for( i = 0; i < pxSpec->ulSlotCount; i++ )
    {
        if( pxSpec->pxSlots[ i ].eClass != HARD_RT ) continue;
        for( j = i + 1; j < pxSpec->ulSlotCount; j++ )
        {
            if( pxSpec->pxSlots[ j ].eClass != HARD_RT ) continue;
            uint32_t a0 = pxSpec->pxSlots[ i ].ulStartOffset;
            uint32_t a1 = pxSpec->pxSlots[ i ].ulEndOffset;
            uint32_t b0 = pxSpec->pxSlots[ j ].ulStartOffset;
            uint32_t b1 = pxSpec->pxSlots[ j ].ulEndOffset;
            if( a0 < b1 && b0 < a1 )
            {
                return pdFAIL;
            }
        }
    }

    return pdPASS;
}

/* =========================================================================== */
/* Configuration & validation                                                  */
/* =========================================================================== */
BaseType_t vConfigureScheduler( const TimelineSpec_t *pxSpec )
{
    if( xTimelineEngineValidate( pxSpec ) != pdPASS )
    {
        vApplicationScheduleErrorHook( teERR_INVALID_SPEC, "configure" );
        return pdFAIL;
    }

    s_xSpec = *pxSpec;
    vTraceEnable( pxSpec->ucTraceEnabled );

    if( prvBuildTables( &s_xSpec ) != pdPASS )
    {
        vApplicationScheduleErrorHook( teERR_TABLE_BUILD, "configure" );
        return pdFAIL;
    }

    /* Create the tick signal and wire up the kernel hook. */
    s_xTickSignal = xSemaphoreCreateBinary();
    if( s_xTickSignal == NULL )
    {
        vApplicationScheduleErrorHook( teERR_SEMAPHORE_ALLOC, "configure" );
        return pdFAIL;
    }
    vTimelineHookInit( s_xSpec.ulMajorFrameTicks, s_xTickSignal );

    /* Create the engine task (highest priority). */
    if( xTaskCreate( prvEngineTask, "TL_ENGINE",
                     configMINIMAL_STACK_SIZE * 2, NULL,
                     tePRIO_ENGINE, &s_xEngineTask ) != pdPASS )
    {
        vApplicationScheduleErrorHook( teERR_ENGINE_TASK_CREATE, "configure" );
        return pdFAIL;
    }

    return pdPASS;
}

/* Request a spec swap: the engine adopts pxSpec at the next frame boundary.
 * Returns pdFAIL if the new spec is invalid (current spec keeps running). */
BaseType_t xTimelineEngineReload( const TimelineSpec_t *pxSpec )
{
    if( xTimelineEngineValidate( pxSpec ) != pdPASS )
    {
        vApplicationScheduleErrorHook( teERR_INVALID_SPEC, "reload" );
        return pdFAIL;
    }
    s_pxPendingSpec = pxSpec;   /* picked up at next frame boundary */
    return pdPASS;
}

BaseType_t xTimelineEngineStart( const TimelineSpec_t *pxSpec )
{
    if( vConfigureScheduler( pxSpec ) != pdPASS )
    {
        return pdFAIL;
    }
    vTaskStartScheduler();
    return pdFAIL; /* only on fatal allocation failure */
}

/* =========================================================================== */
/* CPU statistics accessors                                                    */
/* =========================================================================== */
uint32_t ulTimelineGetNonHrtTicks( void )
{
    return s_ulNonHrtLatched;
}

uint32_t ulTimelineGetTrueIdleTicks( void )
{
    /* 32-bit aligned read on Cortex-M3 is atomic. */
    return s_ulTrueIdleCount;
}

void vTimelineIdleTickAccount( void )
{
    /* The FreeRTOS idle task runs in a tight loop while the CPU is idle, so
     * vApplicationIdleHook may be invoked many thousands of times per tick.
     * To produce a meaningful idle *ticks* count (comparable with elapsed
     * tick counts), bump the counter at most once per tick. */
    static TickType_t s_xLastTick = 0;
    TickType_t xNow = xTaskGetTickCount();
    if( xNow != s_xLastTick )
    {
        s_xLastTick = xNow;
        s_ulTrueIdleCount++;
    }
}

/* =========================================================================== */
/* Trampolines                                                                  */
/* =========================================================================== */
static SlotCB_t *prvSelfCB( void )
{
    TaskHandle_t xSelf = xTaskGetCurrentTaskHandle();
    uint32_t i;
    for( i = 0; i < s_ulHrtCount; i++ )
    {
        if( s_xHrt[ i ].xHandle == xSelf ) return &s_xHrt[ i ];
    }
    for( i = 0; i < s_ulSrtCount; i++ )
    {
        if( s_xSrt[ i ].xHandle == xSelf ) return &s_xSrt[ i ];
    }
    return NULL;