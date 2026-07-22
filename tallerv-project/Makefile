# Makefile - Nucleo-F411RE access control project (Arch Linux toolchain)
# Packages: arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils stlink
# (Debian equivalents: gcc-arm-none-eabi, stlink-tools)

TARGET  = acceso
BUILD   = build

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
SIZE    = arm-none-eabi-size

# Cortex-M4F: hardware single-precision FPU
CPU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard

DEFS    = -DSTM32F411xE -DUSE_HAL_DRIVER
INCS    = -ICore/Inc -IDrivers/HAL/Inc -IDrivers/HAL/Inc/Legacy \
          -IDrivers/CMSIS/Device -IDrivers/CMSIS/Include

CFLAGS  = $(CPU) $(DEFS) $(INCS) -std=c11 -Wall -Wextra -Og -g3 \
          -ffunction-sections -fdata-sections
LDFLAGS = $(CPU) -TSTM32F411RETx_FLASH.ld -Wl,--gc-sections \
          -Wl,-Map=$(BUILD)/$(TARGET).map --specs=nano.specs --specs=nosys.specs

SRCS    = $(wildcard Core/Src/*.c) $(wildcard Drivers/HAL/Src/*.c)
ASMS    = startup/startup_stm32f411xe.s
OBJS    = $(addprefix $(BUILD)/,$(SRCS:.c=.o) $(ASMS:.s=.o))

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).bin
	$(SIZE) $(BUILD)/$(TARGET).elf

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s
	@mkdir -p $(dir $@)
	$(CC) -c $(CPU) -x assembler-with-cpp $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/$(TARGET).bin: $(BUILD)/$(TARGET).elf
	$(OBJCOPY) -O binary $< $@

# Flash over the on-board ST-Link (st-flash from the Arch `stlink` package)
flash: $(BUILD)/$(TARGET).bin
	st-flash --reset write $< 0x08000000

# Alternative: openocd (pacman -S openocd)
flash-ocd: $(BUILD)/$(TARGET).elf
	openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
	  -c "program $< verify reset exit"

clean:
	rm -rf $(BUILD)

.PHONY: all flash flash-ocd clean
