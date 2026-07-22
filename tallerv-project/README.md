# Fase 0 + 1: andamiaje del proyecto y LCD (Nucleo-F411RE)

Access control project scaffold with hand-written HAL initialization (no CubeMX).
See `plan_proyecto_stm32_acceso.md` for the full project plan.

## Build (Arch Linux)

    sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils stlink
    make            # produces build/acceso.elf and build/acceso.bin
    make flash      # flashes via the on-board ST-Link (st-flash)

Note: the linker prints warnings about `_close/_read/_write not implemented`.
They come from newlib-nano because `snprintf` pulls in libc reentrancy stubs;
they are benign (no file I/O is ever performed) and can be ignored.

## Build (STM32CubeIDE)

1. File -> New -> STM32 Project -> part number STM32F411RET6 -> project type **Empty**.
2. Copy `Core/`, `Drivers/`, `startup/` and `STM32F411RETx_FLASH.ld` into the project,
   replacing the generated startup/linker files if the wizard created any.
3. Project Properties -> C/C++ General -> Paths and Symbols:
   - Include paths: Core/Inc, Drivers/HAL/Inc, Drivers/HAL/Inc/Legacy,
     Drivers/CMSIS/Device, Drivers/CMSIS/Include
   - Symbols: STM32F411xE, USE_HAL_DRIVER
4. Build and flash with the built-in ST-Link tools.

## Expected behavior on hardware

1. The heartbeat blinks at 1 Hz on the external LED (PH1, Morpho CN7
   pin 31), driven by the TIM5 update interrupt. LD2 is unused.
2. LCD shows `CTRL ACCESO v0.1` / `LCD OK dir 0xNN` (NN = detected I2C address).
3. Pressing the blue B1 button updates line 1.
4. If the LCD is absent/miswired: the heartbeat switches to 5 Hz (fast blink), firmware keeps running.

## Wiring for this phase

| LCD backpack | Nucleo |
|---|---|
| SCL | D15 (PB8) |
| SDA | D14 (PB9) |
| VCC | 3V3 |
| GND | GND |
