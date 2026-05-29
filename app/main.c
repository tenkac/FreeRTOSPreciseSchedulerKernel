#include <stdio.h>
#include "FreeRTOS.h"
#include "task.h"

static void prvHello( void *pv )
{
    ( void ) pv;
    for( ;; )
    {
        printf( "hello from FreeRTOS\n" );
        vTaskDelay( pdMS_TO_TICKS( 1000 ) );
    }
}

int main( void )
{
    xTaskCreate( prvHello, "HELLO", 256, NULL, 2, NULL );
    vTaskStartScheduler();
    for( ;; ) {}
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