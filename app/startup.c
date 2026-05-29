#include <stdint.h>

// ----------------------------------------------------------------------------
// External Linker Symbols
// ----------------------------------------------------------------------------
extern uint32_t _sidata;  // Start of initialized data in flash (LMA)
extern uint32_t _sdata;   // Start of initialized data in RAM (VMA)
extern uint32_t _edata;   // End of initialized data in RAM
extern uint32_t _sbss;    // Start of uninitialized data (BSS)
extern uint32_t _ebss;    // End of uninitialized data (BSS)
extern uint32_t _estack;  // Top of the initial stack

// ----------------------------------------------------------------------------
// External Functions
// ----------------------------------------------------------------------------
extern int main(void);
extern void uart0_init(void);

// ----------------------------------------------------------------------------
// FreeRTOS Port Handlers
// ----------------------------------------------------------------------------
extern void vPortSVCHandler(void);
extern void xPortPendSVHandler(void);
extern void xPortSysTickHandler(void);

// ----------------------------------------------------------------------------
// Default and Weak Handlers
// ----------------------------------------------------------------------------
void Default_Handler(void) 
{
    // Infinite loop for unhandled exceptions
    while(1);
}

// CMSIS-style weak aliases allow user application to override these
void NMI_Handler(void)        __attribute__((weak, alias("Default_Handler")));
void HardFault_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void MemManage_Handler(void)  __attribute__((weak, alias("Default_Handler")));
void BusFault_Handler(void)   __attribute__((weak, alias("Default_Handler")));
void UsageFault_Handler(void) __attribute__((weak, alias("Default_Handler")));
void DebugMon_Handler(void)   __attribute__((weak, alias("Default_Handler")));

// Standard CMSIS hook for hardware/clock initialization before main()
__attribute__((weak)) void SystemInit(void) {} 

// ----------------------------------------------------------------------------
// Reset Handler
// ----------------------------------------------------------------------------
// 'noreturn' attribute helps the compiler optimize knowing this function won't exit
__attribute__((noreturn)) void Reset_Handler(void) 
{
    uint32_t *src, *dest;

    // 1. Copy the .data section from Flash to RAM
    src = &_sidata;
    for (dest = &_sdata; dest < &_edata; ) {
        *dest++ = *src++;
    }

    // 2. Zero out the .bss section
    for (dest = &_sbss; dest < &_ebss; ) {
        *dest++ = 0;
    }

    // 3. Early hardware initialization (CMSIS standard)
    SystemInit();

    // 4. Initialize UART0 for printf output
    uart0_init();

    // 5. Call the application entry point
    main();

    // 6. Fallback infinite loop (should never be reached in FreeRTOS)
    while(1);
}

// ----------------------------------------------------------------------------
// Interrupt Vector Table
// ----------------------------------------------------------------------------
// 'used' attribute ensures the linker doesn't optimize this away during dead code stripping
__attribute__((section(".isr_vector"), used))
const uint32_t vector_table[] = {
    (uint32_t)&_estack,             // 0x00: Initial Stack Pointer
    (uint32_t)Reset_Handler,        // 0x04: Reset Handler
    (uint32_t)NMI_Handler,          // 0x08: NMI Handler
    (uint32_t)HardFault_Handler,    // 0x0C: Hard Fault Handler
    (uint32_t)MemManage_Handler,    // 0x10: MPU Fault Handler
    (uint32_t)BusFault_Handler,     // 0x14: Bus Fault Handler
    (uint32_t)UsageFault_Handler,   // 0x18: Usage Fault Handler
    0,                              // 0x1C: Reserved
    0,                              // 0x20: Reserved
    0,                              // 0x24: Reserved
    0,                              // 0x28: Reserved
    (uint32_t)vPortSVCHandler,      // 0x2C: SVCall Handler -> FreeRTOS
    (uint32_t)DebugMon_Handler,     // 0x30: Debug Monitor Handler
    0,                              // 0x34: Reserved
    (uint32_t)xPortPendSVHandler,   // 0x38: PendSV Handler -> FreeRTOS
    (uint32_t)xPortSysTickHandler,  // 0x3C: SysTick Handler -> FreeRTOS
};