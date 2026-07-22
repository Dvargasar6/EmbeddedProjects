# ------------------------------------------------------------------------------
# Makefile - Proyecto Parcial Taller V 2026-1
# Target: STM32 Nucleo-F411RE (STM32F411RET6, Cortex-M4F)
# Toolchain: arm-none-eabi-gcc (GNU Arm Embedded)
# ------------------------------------------------------------------------------

######################################
# Nombre del binario de salida
######################################
TARGET = parcial_tallerV

######################################
# Nivel de optimizacion y depuracion
######################################
# DEBUG = 1 -> genera simbolos de depuracion (-g) para usar con gdb/openocd
DEBUG = 1
# -Og optimiza sin dificultar la depuracion. Alternativas: -O0, -O2, -Os (tamano)
OPT = -Og

######################################
# Directorio donde se generan los objetos y binarios
######################################
BUILD_DIR = build

######################################
# Fuentes en C
######################################
# Se listan explicitamente para tener control total sobre lo que se compila.
#
# El proyecto ofrece DOS versiones equivalentes del mismo firmware:
#   - modular  (por defecto): un archivo por periferico, en Core/Src
#   - unica    (UNIQUE=1)   : Core/Src/main_unique.c, que reune todas las
#                             funciones propias en un solo archivo
# No pueden enlazarse a la vez: definen los mismos simbolos.
#
#   make            -> version modular
#   make UNIQUE=1   -> version de archivo unico
ifeq ($(UNIQUE), 1)
APP_SOURCES = Core/Src/main_unique.c
else
APP_SOURCES = \
Core/Src/main.c \
Core/Src/system_clock.c \
Core/Src/gpio.c \
Core/Src/uart.c \
Core/Src/adc_joystick.c \
Core/Src/rtc.c \
Core/Src/i2c_lcd.c \
Core/Src/timer.c \
Core/Src/fsm.c \
Core/Src/stm32f4xx_it.c \
Core/Src/stm32f4xx_hal_msp.c \
Core/Src/syscalls.c
endif

C_SOURCES =  \
$(APP_SOURCES) \
Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/system_stm32f4xx.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_cortex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rcc_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_gpio.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_pwr_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_flash_ramfunc.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_dma_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_exti.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_adc_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_i2c_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_uart.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_tim_ex.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc.c \
Drivers/STM32F4xx_HAL_Driver/Src/stm32f4xx_hal_rtc_ex.c

######################################
# Fuentes en ensamblador
######################################
# startup: tabla de vectores + Reset_Handler (copia .data, limpia .bss, llama main)
ASM_SOURCES = startup_stm32f411xe.s

#######################################
# Definicion de la cadena de herramientas
#######################################
# GCC_PATH permite apuntar a un toolchain instalado en una ruta concreta.
# En Arch Linux, con el paquete 'arm-none-eabi-gcc' los binarios quedan en /usr/bin,
# por lo que basta con dejar GCC_PATH vacio y usar el PATH del sistema.
PREFIX = arm-none-eabi-
ifdef GCC_PATH
CC  = $(GCC_PATH)/$(PREFIX)gcc
AS  = $(GCC_PATH)/$(PREFIX)gcc -x assembler-with-cpp
CP  = $(GCC_PATH)/$(PREFIX)objcopy
SZ  = $(GCC_PATH)/$(PREFIX)size
else
CC  = $(PREFIX)gcc
AS  = $(PREFIX)gcc -x assembler-with-cpp
CP  = $(PREFIX)objcopy
SZ  = $(PREFIX)size
endif
HEX = $(CP) -O ihex          # convierte ELF -> Intel HEX
BIN = $(CP) -O binary -S     # convierte ELF -> binario plano

#######################################
# Opciones del nucleo (CPU / FPU)
#######################################
CPU = -mcpu=cortex-m4        # nucleo ARM Cortex-M4
FPU = -mfpu=fpv4-sp-d16      # unidad de punto flotante de precision simple
FLOAT-ABI = -mfloat-abi=hard # usa registros de la FPU para pasar flotantes
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)   # -mthumb: set de instrucciones Thumb-2

#######################################
# Macros del preprocesador
#######################################
AS_DEFS =
C_DEFS = \
-DUSE_HAL_DRIVER \
-DSTM32F411xE

#######################################
# Rutas de cabeceras
#######################################
AS_INCLUDES =
C_INCLUDES = \
-ICore/Inc \
-IDrivers/STM32F4xx_HAL_Driver/Inc \
-IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
-IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
-IDrivers/CMSIS/Include

#######################################
# Banderas de compilacion
#######################################
ASFLAGS = $(MCU) $(AS_DEFS) $(AS_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

# -Wall            : activa avisos comunes
# -fdata-sections  : cada variable en su propia seccion
# -ffunction-sections: cada funcion en su propia seccion
# (ambas permiten que el enlazador elimine codigo no usado con --gc-sections)
CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -fdata-sections -ffunction-sections

ifeq ($(DEBUG), 1)
CFLAGS += -g -gdwarf-2      # informacion de depuracion en formato DWARF-2
endif

# Punto de entrada para macros adicionales desde la linea de ordenes.
# Ejemplo: make CFLAGS_EXTRA=-DLCD_USE_PB6_PB7
CFLAGS += $(CFLAGS_EXTRA)

# Genera archivos .d con las dependencias de cabeceras de cada .c,
# para que make recompile si cambia un .h
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"

#######################################
# Banderas de enlazado
#######################################
LDSCRIPT = STM32F411RETX_FLASH.ld

LIBS = -lc -lm -lnosys      # libc, libm (matematicas) y stubs de sistema vacios
LIBDIR =
# -specs=nano.specs : usa newlib-nano (mucho mas pequena)
# -Wl,-Map          : genera un mapa de memoria util para depurar el enlace
# -Wl,--gc-sections : elimina secciones no referenciadas
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections

# Objetivo por defecto: genera ELF, HEX y BIN
all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin

#######################################
# Reglas de construccion
#######################################
# Lista de objetos derivada de las fuentes C
OBJECTS = $(addprefix $(BUILD_DIR)/,$(notdir $(C_SOURCES:.c=.o)))
# vpath indica a make donde buscar los .c cuando solo conoce el nombre del .o
vpath %.c $(sort $(dir $(C_SOURCES)))
# Se anaden los objetos de ensamblador
OBJECTS += $(addprefix $(BUILD_DIR)/,$(notdir $(ASM_SOURCES:.s=.o)))
vpath %.s $(sort $(dir $(ASM_SOURCES)))

# Compilacion de cada .c a .o
$(BUILD_DIR)/%.o: %.c Makefile | $(BUILD_DIR)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

# Ensamblado de cada .s a .o
$(BUILD_DIR)/%.o: %.s Makefile | $(BUILD_DIR)
	$(AS) -c $(CFLAGS) $< -o $@

# Enlazado final: produce el ejecutable ELF y muestra el tamano
$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

# Conversion a formatos de programacion
$(BUILD_DIR)/%.hex: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(HEX) $< $@

$(BUILD_DIR)/%.bin: $(BUILD_DIR)/%.elf | $(BUILD_DIR)
	$(BIN) $< $@

# Creacion del directorio de compilacion
$(BUILD_DIR):
	mkdir -p $@

#######################################
# Utilidades
#######################################
# Borra todos los artefactos de compilacion
clean:
	-rm -fR $(BUILD_DIR)

# Programa la placa usando st-flash (paquete 'stlink' en Arch Linux)
flash: all
	st-flash --reset write $(BUILD_DIR)/$(TARGET).bin 0x8000000

# Programa la placa usando OpenOCD (alternativa a st-flash)
openocd-flash: all
	openocd -f interface/stlink.cfg -f target/stm32f4x.cfg \
	        -c "program $(BUILD_DIR)/$(TARGET).elf verify reset exit"

# Muestra el uso de memoria FLASH y RAM
size: $(BUILD_DIR)/$(TARGET).elf
	$(SZ) -A $<

# Abre un terminal serie a 115200 baudios (paquete 'minicom' o 'picocom')
serial:
	picocom -b 115200 /dev/ttyACM0

.PHONY: all clean flash openocd-flash size serial

#######################################
# Inclusion de dependencias generadas
#######################################
-include $(wildcard $(BUILD_DIR)/*.d)
