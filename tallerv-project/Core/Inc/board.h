#ifndef BOARD_H
#define BOARD_H

#include "stm32f4xx_hal.h"

void SystemClock_Config(void);
void Board_GPIO_Init(void);
void Error_Handler(void);

/* Heartbeat on TIM5: the LED blinks from the timer interrupt, autonomously
   from the main loop. Steady 1 Hz = system alive; 5 Hz = LCD not detected. */
void Heartbeat_Init(void);
void Heartbeat_SetFast(uint8_t fast);

extern TIM_HandleTypeDef htim5;   /* needed by the IRQ handler in stm32f4xx_it.c */

/* LED de heartbeat: LED externo en PH1 (OSC_OUT, libre como GPIO al usar HSI).
   Posicion Morpho: CN7 pin 31. Centralizado aqui para que moverlo sea trivial. */
#define HB_LED_PORT   GPIOH
#define HB_LED_PIN    GPIO_PIN_1

#define LED_ON()      HAL_GPIO_WritePin(HB_LED_PORT, HB_LED_PIN, GPIO_PIN_SET)
#define LED_OFF()     HAL_GPIO_WritePin(HB_LED_PORT, HB_LED_PIN, GPIO_PIN_RESET)
#define LED_TOGGLE()  HAL_GPIO_TogglePin(HB_LED_PORT, HB_LED_PIN)
#define LED_WRITE(x)  HAL_GPIO_WritePin(HB_LED_PORT, HB_LED_PIN, (x) ? GPIO_PIN_SET : GPIO_PIN_RESET)
#define B1_PRESSED()  (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_13) == GPIO_PIN_RESET)

#endif
