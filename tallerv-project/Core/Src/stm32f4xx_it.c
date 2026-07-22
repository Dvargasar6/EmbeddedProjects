/**
 * stm32f4xx_it.c - Interrupt handlers.
 * Only the handlers needed so far; USART2_IRQHandler joins in Phase 2.
 */
#include "stm32f4xx_hal.h"
#include "board.h"          /* htim5 */

/* Cortex-M4 fault handlers: trap instead of executing garbage */
void NMI_Handler(void)        { while (1) {} }
void HardFault_Handler(void)  { while (1) {} }
void MemManage_Handler(void)  { while (1) {} }
void BusFault_Handler(void)   { while (1) {} }
void UsageFault_Handler(void) { while (1) {} }
void SVC_Handler(void)        {}
void DebugMon_Handler(void)   {}
void PendSV_Handler(void)     {}

/* SysTick: 1 kHz timebase for HAL_Delay / HAL_GetTick */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* TIM5 update interrupt: HAL clears the flag and calls the PeriodElapsed callback */
void TIM5_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim5);
}
