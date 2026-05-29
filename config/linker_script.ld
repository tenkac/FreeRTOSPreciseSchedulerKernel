/*
 * Linker script for QEMU lm3s6965evb (ARM Cortex-M3)
 * Tailored for FreeRTOS and CMSIS-compliant startup code.
 */

/* ----------------------------------------------------------------------------
 * Memory Definitions
 * ---------------------------------------------------------------------------- */
MEMORY
{
    FLASH (rx)  : ORIGIN = 0x00000000, LENGTH = 256K
    RAM (rwx)   : ORIGIN = 0x20000000, LENGTH = 64K
}

/* Entry point defined in the C startup code */
ENTRY(Reset_Handler)

/* Highest address of the user mode stack */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* Define minimum heap and stack sizes for the linker to verify */
_Min_Heap_Size  = 0x2000; /* 8KB Heap (Used by newlib/printf, if necessary) */
_Min_Stack_Size = 0x1000; /* 4KB Main Stack (Used by main() and FreeRTOS interrupts) */

/* ----------------------------------------------------------------------------
 * Sections Configuration
 * ---------------------------------------------------------------------------- */
SECTIONS
{
    /* The program code and read-only data go into FLASH */
    .text :
    {
        . = ALIGN(4);
        KEEP(*(.isr_vector))    /* Vector table MUST be first in flash */
        *(.text)                /* Core execution code */
        *(.text*)               /* Code from libraries */
        *(.rodata)              /* Read-only data (constants, strings) */
        *(.rodata*)             /* Read-only data from libraries */

        . = ALIGN(4);
        _etext = .;             /* End of text section */
    } > FLASH

    /* The load address (LMA) of the .data section, used by startup code to copy to RAM */
    _sidata = LOADADDR(.data);

    /* Initialized data goes into RAM (VMA) but is loaded in FLASH (LMA) */
    .data :
    {
        . = ALIGN(4);
        _sdata = .;             /* Start of .data in RAM */
        *(.data)                /* Initialized variables */
        *(.data*)               /* Initialized variables from libraries */

        . = ALIGN(4);
        _edata = .;             /* End of .data in RAM */
    } > RAM AT > FLASH

    /* Uninitialized data goes into RAM */
    .bss :
    {
        . = ALIGN(4);
        _sbss = .;              /* Start of .bss */
        *(.bss)                 /* Zero-initialized variables */
        *(.bss*)
        *(COMMON)

        . = ALIGN(4);
        _ebss = .;              /* End of .bss */

        /* Symbols required by newlib syscalls (like sbrk for malloc) */
        PROVIDE( end = . );
        PROVIDE( _end = . );
    } > RAM

    /* * Dummy section to check if there is enough RAM left for the Stack and Heap.
     * This generates a linker error if your application is too large, rather than
     * failing silently at runtime.
     */
    ._user_heap_stack (NOLOAD) :
    {
        . = ALIGN(8);           /* 8-byte alignment required by ARM ABI */
        PROVIDE ( __heap_start = . );
        . = . + _Min_Heap_Size;
        PROVIDE ( __heap_end = . );
        . = . + _Min_Stack_Size;
        . = ALIGN(8);
    } > RAM
}