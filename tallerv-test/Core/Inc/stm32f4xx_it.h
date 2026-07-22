/*
 * stm32f4xx_it.h - Declaracion de las rutinas de servicio de interrupcion (ISR).
 * Los nombres deben coincidir EXACTAMENTE con los del vector de startup_stm32f411xe.s
 */
#ifndef STM32F4xx_IT_H
#define STM32F4xx_IT_H

/* Excepciones del nucleo Cortex-M4 */
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);

#endif /* STM32F4xx_IT_H */
