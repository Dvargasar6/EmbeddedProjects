/**
 * main.c - Phase 0 + Phase 1: scaffold and LCD bring-up.
 *
 * Expected behavior on the bench:
 *   1. Heartbeat LED (external, PH1) at 1 Hz, driven by the TIM5 interrupt.
 *   2. LCD shows a boot banner and its detected I2C address (Phase 1 proof).
 *   3. Pressing B1 updates line 1 (input path proof).
 *   If the LCD is absent or miswired, the heartbeat switches to 5 Hz
 *   (fast blink) and the firmware keeps running.
 */

#include "board.h"
#include "lcd_i2c.h"
#include <stdio.h>

int main(void)
{
    HAL_Init();               /* flash cache, NVIC grouping, SysTick 1 kHz */
    Board_GPIO_Init();        /* LED FIRST: GPIO works on the reset-default HSI,
                                 and Error_Handler needs the pin ready in case
                                 the clock configuration itself fails */
    SystemClock_Config();     /* HSI 16 MHz sin PLL, identico a tarea3 (board.c) */
    Heartbeat_Init();         /* TIM5 blink starts NOW: if the LED stays dark
                                 past this point, the fault is hardware, because
                                 nothing after here can stop the timer IRQ */

    uint8_t lcd_ok = (LCD_HW_Init() == HAL_OK);
    if (lcd_ok) {
        LCD_Init();
        char l2[17];
        /* Direccion I2C detectada del backpack: diagnostico de cableado */
        snprintf(l2, sizeof(l2), "LCD OK dir 0x%02X", LCD_GetAddr7());
        LCD_Line(0, "Proyecto Taller V");
        LCD_Line(1, "Daniel Vargas");
    }

    if (!lcd_ok) Heartbeat_SetFast(1);   /* 5 Hz = LCD did not answer on I2C */

    uint8_t  b1_last = 0;
    while (1) {
        /* The heartbeat now lives entirely in the TIM5 interrupt (board.c) */
        if (lcd_ok) {
            /* B1 press/release feedback on line 1 (simple edge detection) */
            uint8_t b1 = B1_PRESSED();
            if (b1 != b1_last) {
                b1_last = b1;
                LCD_Line(1, b1 ? "B1 presionado" : "Fase 1 lista");
            }
        }
    }
}
