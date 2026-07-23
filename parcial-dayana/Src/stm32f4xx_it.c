/**
 * @file    stm32f4xx_it.c
 * @author  Daniel Felipe Vargas Arias
 * @brief   Manejadores de interrupcion (ISR) del proyecto.
 *
 * @details
 * Cada handler solo reenvia el evento al HAL (HAL_xxx_IRQHandler), que a su
 * vez invoca el callback correspondiente definido en main.c (por ejemplo
 * HAL_TIM_PeriodElapsedCallback).
 */

#include "stm32f4xx_hal.h"

/* Handles definidos en main.c, requeridos por los HAL_xxx_IRQHandler */
extern TIM_HandleTypeDef htim10;
extern UART_HandleTypeDef huart2;
extern ADC_HandleTypeDef hadc1;
extern RTC_HandleTypeDef hrtc;

/* Requerido por HAL para HAL_Delay() y el conteo de milisegundos */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* TIM10 comparte vector con la actualizacion de TIM1 en el F411 */
void TIM1_UP_TIM10_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim10);
}

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

/**
  * @brief This function handles ADC1 global interrupt.
  */
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}

/**
  * @brief This function handles RTC WakeUp Timer interrupt (EXTI line 22).
  */
void RTC_WKUP_IRQHandler(void)
{
    HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc);
}
