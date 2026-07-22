/*
 * gpio.h
 * Modulo de pines de proposito general del proyecto.
 *
 * Pines gestionados aqui:
 *   PH1 -> LED blinky (salida push-pull, conmutado por TIM3)
 *   PB5 -> Pulsador SW del joystick (entrada con pull-up, interrupcion EXTI5)
 *
 * Los pines de periferico (ADC, UART, I2C, MCO) se configuran en el archivo
 * stm32f4xx_hal_msp.c, que es el lugar que la HAL reserva para ello.
 */
#ifndef GPIO_H
#define GPIO_H

#include "main.h"

/* --- LED de actividad --------------------------------------------------- */
#define LED_GPIO_PORT      GPIOH          /* puerto del LED                 */
#define LED_PIN            GPIO_PIN_1     /* PH1, impuesto por el enunciado */

/* --- Pulsador integrado del joystick ------------------------------------ */
#define JOY_SW_GPIO_PORT   GPIOB          /* puerto del pulsador            */
#define JOY_SW_PIN         GPIO_PIN_5     /* PB5 = D4 en el conector Arduino */
#define JOY_SW_EXTI_IRQn   EXTI9_5_IRQn   /* las lineas EXTI5..9 comparten IRQ */

void gpio_init(void);        /* configura LED y pulsador                    */
void gpio_led_toggle(void);  /* invierte el estado del LED                  */
void gpio_led_write(uint8_t on); /* fuerza el LED a un estado concreto      */

#endif /* GPIO_H */
