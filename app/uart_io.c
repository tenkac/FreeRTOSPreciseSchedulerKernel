/**
 * @file uart_io.c
 * @brief UART0 driver for LM3S6965 (QEMU) — enables printf output
 *
 * Implements the _write syscall so that printf/puts send characters
 * through UART0 on the Stellaris LM3S6965 board emulated by QEMU.
 */

#include <stdint.h>
#include <sys/stat.h>

/* LM3S6965 UART0 registers */
#define UART0_DR   (*((volatile uint32_t *)0x4000C000))
#define UART0_FR   (*((volatile uint32_t *)0x4000C018))
#define UART0_FR_TXFF  (1U << 5)  /* TX FIFO full flag */

/**
 * @brief Send one character via UART0
 */
static void uart_putc(char c)
{
    /* Wait while TX FIFO is full */
    while (UART0_FR & UART0_FR_TXFF)
        ;
    UART0_DR = (uint32_t)c;
}

/* ---- newlib syscall stubs ---- */

int _write(int fd, const char *buf, int len)
{
    (void)fd;
    for (int i = 0; i < len; i++)
    {
        if (buf[i] == '\n')
            uart_putc('\r');   /* CRLF for terminal */
        uart_putc(buf[i]);
    }
    return len;
}

int _read(int fd, char *buf, int len)
{
    (void)fd; (void)buf; (void)len;
    return 0;
}

int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
void _exit(int status) { (void)status; while(1); }
int _getpid(void) { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; return -1; }
static char *heap_ptr = 0;
extern char end;  /* defined by linker script */
extern char _end; /* alternate name */

void *_sbrk(int incr)
{
    if (heap_ptr == 0)
        heap_ptr = &end;
    char *prev = heap_ptr;
    heap_ptr += incr;
    return (void *)prev;
}

/**
 * @brief Initialize UART0 for QEMU LM3S6965
 * Must be called before any printf.
 */
void uart0_init(void)
{
    /* QEMU's LM3S6965 UART0 works out-of-reset at default baud.
     * On real hardware you'd configure RCGC1/IBRD/FBRD/LCRH/CTL.
     * For QEMU we just need to make sure the UART is enabled.
     */
    volatile uint32_t *UART0_CTL  = (volatile uint32_t *)0x4000C030;
    volatile uint32_t *UART0_LCRH = (volatile uint32_t *)0x4000C02C;
    volatile uint32_t *RCGC1      = (volatile uint32_t *)0x400FE104;

    *RCGC1 |= (1U << 0);          /* Enable UART0 clock          */
    *UART0_CTL &= ~(1U << 0);     /* Disable UART during config  */
    *UART0_LCRH = (0x3 << 5);     /* 8-N-1                       */
    *UART0_CTL |= (1U << 0) | (1U << 8) | (1U << 9); /* EN+TXE+RXE */
}