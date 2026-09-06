# ECU-25 firmware — STM32F407VGT6 target build.
#
#   make            build/ecu25.elf + .bin + .hex, and print the size
#   make ARM_PREFIX=/path/to/arm-none-eabi-   use a toolchain elsewhere
#
# The same core/ sources build here and on the host, so anything that
# compiles clean under `make -C firmware test` is already portable.

ARM_PREFIX ?= arm-none-eabi-
CC := $(ARM_PREFIX)gcc
OBJCOPY := $(ARM_PREFIX)objcopy
SIZE := $(ARM_PREFIX)size

TOP := ../..
BUILD := build

CPU := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

CFLAGS := $(CPU) -std=c11 -Os -g3 -ffunction-sections -fdata-sections \
          -Wall -Wextra -Werror -Wno-unused-parameter \
          -DSTM32F407xx \
          -I$(TOP)/core -I../vendor/cmsis

LDFLAGS := $(CPU) -T stm32f407.ld -Wl,--gc-sections -Wl,-Map=$(BUILD)/ecu25.map \
           --specs=nano.specs --specs=nosys.specs -lm

CORE := $(TOP)/core/gcu_config.c $(TOP)/core/engine_fsm.c \
        $(TOP)/core/protection.c $(TOP)/core/amf_fsm.c $(TOP)/core/gcu_app.c \
        $(TOP)/core/j1939.c $(TOP)/core/modbus.c $(TOP)/core/ac_sense.c \
        $(TOP)/core/sensors.c $(TOP)/core/ecu_main.c

TARGET := startup.c system.c plat_stm32.c main.c

SRCS := $(CORE) $(TARGET)
OBJS := $(addprefix $(BUILD)/,$(notdir $(SRCS:.c=.o)))
VPATH := $(TOP)/core:.

all: $(BUILD)/ecu25.bin $(BUILD)/ecu25.hex
	@$(SIZE) $(BUILD)/ecu25.elf

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/ecu25.elf: $(OBJS) stm32f407.ld
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/ecu25.bin: $(BUILD)/ecu25.elf
	$(OBJCOPY) -O binary $< $@

$(BUILD)/ecu25.hex: $(BUILD)/ecu25.elf
	$(OBJCOPY) -O ihex $< $@

flash: $(BUILD)/ecu25.elf
	openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
	        -c "program $< verify reset exit"

clean:
	rm -rf $(BUILD)

.PHONY: all clean flash
