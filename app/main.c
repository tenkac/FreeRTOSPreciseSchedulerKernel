#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timeline_engine.h"

static void HRT_A(void *pv) { (void)pv; printf("  HRT_A ran\n"); }

static const TimelineSlot_t slots[] = {
    { "HRT_A", HRT_A, NULL, HARD_RT, 10, 15, 1, 0, configMINIMAL_STACK_SIZE },
};
static const TimelineSpec_t spec = {
    .ulMajorFrameTicks = 50, .ulSubframeTicks = 10,
    .pxSlots = slots, .ulSlotCount = 1, .ucTraceEnabled = 1,
};

int main(void)
{
    printf("Engine smoke test\n");
    xTimelineEngineStart(&spec);
    for(;;){}
}

/* Required hooks for static allocation */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxTCB, StackType_t **ppxStack, uint32_t *pulSize )
{
    static StaticTask_t xTCB;
    static StackType_t  xStack[ configMINIMAL_STACK_SIZE ];
    *ppxTCB = &xTCB; *ppxStack = xStack; *pulSize = configMINIMAL_STACK_SIZE;
}
void vApplicationGetTimerTaskMemory( StaticTask_t **ppxTCB, StackType_t **ppxStack, uint32_t *pulSize )
{
    static StaticTask_t xTCB;
    static StackType_t  xStack[ configTIMER_TASK_STACK_DEPTH ];
    *ppxTCB = &xTCB; *ppxStack = xStack; *pulSize = configTIMER_TASK_STACK_DEPTH;
}
void vApplicationMallocFailedHook(void) { for(;;); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t;(void)n; for(;;); }
void vApplicationIdleHook(void) {}
void vApplicationTickHook(void) {}
void vAssertCalled(void) { for(;;); }