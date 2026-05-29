# ============================================================================
# Makefile - Project 1: Timeline Scheduler
# Target: QEMU lm3s6965evb (ARM Cortex-M3), arm-none-eabi-gcc (newlib)
#
# Assumes FreeRTOS-Kernel/tasks.c has already been edited by hand.
# ============================================================================
#
#   make           # build the demo firmware  -> build/scheduler.elf
#   make run       # demo  + QEMU
#   make tests     # build the C test suite   -> build/tests.elf
#   make run-tests # tests + QEMU
#   make debug     # demo  + QEMU with gdb stub on :1234, halted
#   make clean
# ----------------------------------------------------------------------------

CROSS   ?= arm-none-eabi-
CC       = $(CROSS)gcc
OBJCOPY  = $(CROSS)objcopy
SIZE     = $(CROSS)size

APP_DIR     = app
CFG_DIR     = config
SCHED_DIR   = scheduler
HOOK_DIR    = kernel_patch
TEST_DIR    = tests
KERNEL_DIR  = FreeRTOS-Kernel
PORT_DIR    = $(KERNEL_DIR)/portable/GCC/ARM_CM3
HEAP        = $(KERNEL_DIR)/portable/MemMang/heap_4.c

BUILD_DIR   = build
TARGET      = $(BUILD_DIR)/scheduler.elf
TEST_TARGET = $(BUILD_DIR)/tests.elf

# Sources shared by both binaries (everything except the entry-point .c).
COMMON_SRCS = \
  $(APP_DIR)/startup.c \
  $(APP_DIR)/uart_io.c \
  $(SCHED_DIR)/timeline_engine.c \
  $(SCHED_DIR)/trace.c \
  $(HOOK_DIR)/timeline_hook.c \
  $(KERNEL_DIR)/tasks.c \
  $(KERNEL_DIR)/queue.c \
  $(KERNEL_DIR)/list.c \
  $(KERNEL_DIR)/timers.c \
  $(KERNEL_DIR)/event_groups.c \
  $(KERNEL_DIR)/stream_buffer.c \
  $(PORT_DIR)/port.c \
  $(HEAP)

DEMO_SRCS = $(APP_DIR)/main.c       $(COMMON_SRCS)
TEST_SRCS = $(TEST_DIR)/test_main.c $(COMMON_SRCS)

INCS = \
  -I$(CFG_DIR) \
  -I$(SCHED_DIR) \
  -I$(APP_DIR) \
  -I$(HOOK_DIR) \
  -I$(TEST_DIR) \
  -I$(KERNEL_DIR)/include \
  -I$(PORT_DIR)

CPU     = -mcpu=cortex-m3 -mthumb
CFLAGS  = $(CPU) $(INCS) -O2 -g3 -ffunction-sections -fdata-sections \
          -Wall -Wextra -ffreestanding

# Pass BENCHMARK=1 on the make command line to build with no-op task bodies
# and live trace disabled - useful for measuring pure scheduler overhead.
# Default (BENCHMARK=0 or unset) is the showcase demo.
ifeq ($(BENCHMARK),1)
    CFLAGS += -DBENCHMARK_MODE=1
endif

LDFLAGS_BASE = $(CPU) -T $(CFG_DIR)/linker_script.ld -nostartfiles \
               -Wl,--gc-sections --specs=nano.specs --specs=nosys.specs

# ============================================================================
.PHONY: all tests run run-tests bench debug clean

all: $(TARGET)

$(TARGET): $(DEMO_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(DEMO_SRCS) $(LDFLAGS_BASE) \
	     -Wl,-Map=$(BUILD_DIR)/scheduler.map -o $@
	@echo "===== Demo Build Complete ====="
	$(SIZE) $@

tests: $(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS) | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(TEST_SRCS) $(LDFLAGS_BASE) \
	     -Wl,-Map=$(BUILD_DIR)/tests.map -o $@
	@echo "===== Test Build Complete ====="
	$(SIZE) $@

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

QEMU_FLAGS = -M lm3s6965evb -nographic \
             -semihosting-config enable=on,target=native

run: all
	qemu-system-arm $(QEMU_FLAGS) -kernel $(TARGET)


# Shortcut: clean rebuild in benchmark mode + run. Always cleans first because
# BENCHMARK_MODE is a compile-time macro - reusing demo-mode object files
# would silently produce the wrong binary.
bench:
	$(MAKE) clean
	$(MAKE) BENCHMARK=1
	qemu-system-arm $(QEMU_FLAGS) -kernel $(TARGET)

run-tests: tests
	qemu-system-arm $(QEMU_FLAGS) -kernel $(TEST_TARGET)

debug: all
	qemu-system-arm $(QEMU_FLAGS) -kernel $(TARGET) -S -gdb tcp::1234

clean:
	rm -rf $(BUILD_DIR)