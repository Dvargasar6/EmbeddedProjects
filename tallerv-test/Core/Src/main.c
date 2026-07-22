/*
 * main.c
 * Punto de entrada del proyecto.
 *
 * PARCIAL TALLER V 2026-1 - DANIEL VARGAS - UNAL
 * Plataforma: STM32 Nucleo-F411RE (STM32F411RET6)
 *
 * El archivo se limita a orquestar la inicializacion de los modulos y a
 * ceder el control a la maquina de estados. Toda la logica reside en fsm.c
 * y cada periferico esta encapsulado en su propio par de archivos.
 */
#include "main.h"
#include "system_clock.h"
#include "gpio.h"
#include "timer.h"
#include "uart.h"
#include "adc_joystick.h"
#include "rtc.h"
#include "i2c_lcd.h"
#include "fsm.h"

int main(void)
{
    /* --- Capa de arranque ------------------------------------------------ */
    HAL_Init();     /* configura la FLASH, el SysTick a 1 ms y llama a
                       HAL_MspInit(). Debe ser la primera llamada. */

    /* Cuatro bits de prioridad de expropiacion y ninguno de subprioridad.
       Simplifica el diseno: el numero que se pasa a HAL_NVIC_SetPriority es
       directamente la prioridad, entre 0 (maxima) y 15 (minima). */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    system_clock_init();   /* SYSCLK a 100 MHz, LSE en marcha, MCO1 activa */

    /* --- Perifericos ----------------------------------------------------- */
    gpio_init();           /* LED en PH1 y pulsador del joystick en PB5     */
    uart_init();           /* USART2 a 115200 por interrupciones            */
    adc_joystick_init();   /* ADC1 sobre PA0 y PA1 por interrupciones       */
    rtc_init();            /* RTC con LSE y despertador de 1 s              */
    lcd_init();            /* LCD 16x4 por I2C1 (unico periferico sondeado) */
    timer_init();          /* TIM3 (LED, 250 ms) y TIM4 (base de 100 ms)    */

    /* --- Aplicacion ------------------------------------------------------ */
    fsm_init();            /* estado inicial y pantalla de bienvenida       */

    while (1) {
        fsm_run();         /* consume los eventos encolados por las ISR     */
    }
}

/*
 * Error_Handler
 * Punto unico de fallo. Deshabilita las interrupciones y hace parpadear el
 * LED muy rapido para que el error sea visible sin necesidad de depurador.
 */
void Error_Handler(void)
{
    __disable_irq();

    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
        /* Retardo por bucle: HAL_Delay depende del SysTick, que esta
           deshabilitado junto con el resto de interrupciones. */
        for (volatile uint32_t i = 0; i < 500000U; i++) {
            __NOP();
        }
    }
}

#ifdef USE_FULL_ASSERT
/*
 * assert_failed
 * La HAL la invoca cuando un parametro no supera sus comprobaciones internas.
 * Solo se compila si se define USE_FULL_ASSERT en stm32f4xx_hal_conf.h.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
