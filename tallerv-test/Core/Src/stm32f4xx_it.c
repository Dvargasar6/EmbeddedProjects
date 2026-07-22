/*
 * stm32f4xx_it.c
 * Rutinas de servicio de interrupcion del proyecto.
 *
 * Todos los perifericos salvo el I2C trabajan por interrupcion, tal como
 * exige el enunciado. Cada ISR se limita a delegar en el manejador generico
 * de la HAL, que a su vez invoca el callback correspondiente en el modulo
 * del periferico. Ninguna ISR contiene logica de aplicacion.
 *
 * Mapa de interrupciones y prioridades (0 = mas urgente):
 *   USART2_IRQn    prioridad 5   recepcion y transmision serie
 *   ADC_IRQn       prioridad 6   fin de conversion de cada eje
 *   TIM3_IRQn      prioridad 7   base de 250 ms para el LED
 *   TIM4_IRQn      prioridad 7   base de 100 ms del sistema
 *   EXTI9_5_IRQn   prioridad 8   pulsador SW del joystick (PB5)
 *   RTC_WKUP_IRQn  prioridad 9   despertador de 1 s del RTC
 *   SysTick        prioridad 15  contador de milisegundos de la HAL
 */
#include "main.h"
#include "stm32f4xx_it.h"
#include "timer.h"
#include "uart.h"
#include "adc_joystick.h"
#include "rtc.h"
#include "gpio.h"
#include "fsm.h"

/* ================= Excepciones del nucleo Cortex-M4 ==================== */

void NMI_Handler(void)        { while (1) { } }  /* no enmascarable        */
void HardFault_Handler(void)  { while (1) { } }  /* fallo grave            */
void MemManage_Handler(void)  { while (1) { } }  /* violacion de la MPU    */
void BusFault_Handler(void)   { while (1) { } }  /* fallo de bus           */
void UsageFault_Handler(void) { while (1) { } }  /* instruccion invalida   */
void SVC_Handler(void)        { }                /* llamada al supervisor  */
void DebugMon_Handler(void)   { }                /* monitor de depuracion  */
void PendSV_Handler(void)     { }                /* cambio de contexto     */

/*
 * SysTick_Handler
 * Se dispara cada milisegundo. Incrementa el contador que sostiene
 * HAL_GetTick() y HAL_Delay(); sin el, cualquier funcion de la HAL con
 * tiempo maximo de espera se bloquearia para siempre.
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ================= Interrupciones de periferico ======================== */

/*
 * USART2_IRQHandler
 * Atiende recepcion, transmision y errores. HAL_UART_IRQHandler decide cual
 * de los tres casos aplica e invoca HAL_UART_RxCpltCallback,
 * HAL_UART_TxCpltCallback o HAL_UART_ErrorCallback, definidos en uart.c.
 */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

/*
 * ADC_IRQHandler
 * El STM32F411 comparte un unico vector para todos sus ADC.
 * Deriva en HAL_ADC_ConvCpltCallback, definido en adc_joystick.c.
 */
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}

/*
 * TIM3_IRQHandler
 * Desbordamiento cada 250 ms. Deriva en HAL_TIM_PeriodElapsedCallback,
 * que conmuta el LED de PH1.
 */
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}

/*
 * TIM4_IRQHandler
 * Desbordamiento cada 100 ms. Lanza la conversion del ADC y encola el
 * evento periodico de la maquina de estados.
 */
void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim4);
}

/*
 * EXTI9_5_IRQHandler
 * Vector compartido por las lineas EXTI 5 a 9. HAL_GPIO_EXTI_IRQHandler
 * comprueba cual de ellas esta pendiente, limpia la bandera e invoca
 * HAL_GPIO_EXTI_Callback.
 */
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(JOY_SW_PIN);
}

/*
 * RTC_WKUP_IRQHandler
 * Despertador periodico de 1 s. Deriva en
 * HAL_RTCEx_WakeUpTimerEventCallback, definido en rtc.c.
 */
void RTC_WKUP_IRQHandler(void)
{
    HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc);
}

/*
 * HAL_GPIO_EXTI_Callback
 * Callback comun a todas las lineas EXTI. Incorpora el antirrebote por
 * software: los contactos mecanicos del pulsador generan decenas de flancos
 * espurios durante algunos milisegundos, por lo que se ignora todo flanco
 * que llegue antes de 200 ms desde el anterior.
 */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    static uint32_t last = 0;    /* marca temporal del ultimo flanco valido */
    uint32_t now = HAL_GetTick();

    if (pin != JOY_SW_PIN) {
        return;
    }
    if ((now - last) < 200U) {
        return;                  /* rebote: se descarta */
    }
    last = now;

    fsm_post_event(EV_BUTTON);
}
