/*
 * stm32f4xx_it.c
 * Interrupt service routines
 * Author: your name
 */

#include "stm32f4xx_hal.h"

/* Declare the TIM3 handle — defined in main.c */
extern TIM_HandleTypeDef htim5;
extern UART_HandleTypeDef huart2;
extern TIM_HandleTypeDef htim3;
extern ADC_HandleTypeDef hadc1;

/* SysTick handler — required by HAL for HAL_Delay() and timeouts */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* TIM3 update event handler */
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}

/**
  * @brief This function handles TIM5 global interrupt.
  */
void TIM5_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim5);
}

/**
  * @brief This function handles USART2 global interrupt.
  */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

/**
  * @brief This function handles ADC global interrupt.
  */
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}
