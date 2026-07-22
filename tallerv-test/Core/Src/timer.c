/*
 * timer.c
 * Configuracion de TIM3 (LED a 250 ms) y TIM4 (base de tiempo de 100 ms).
 *
 * Calculo de la temporizacion:
 *
 *   f_TIM = f_APB1 * 2 = 50 MHz * 2 = 100 MHz
 *   f_cnt = f_TIM / (PSC + 1)
 *   T     = (ARR + 1) / f_cnt
 *
 * TIM3: PSC = 9999  -> f_cnt = 100 MHz / 10000 = 10 kHz (paso de 100 us)
 *       ARR = 2499  -> T = 2500 / 10 kHz = 250 ms
 *
 * TIM4: PSC = 9999  -> f_cnt = 10 kHz
 *       ARR = 999   -> T = 1000 / 10 kHz = 100 ms
 *
 * Se eligen PSC y ARR de forma que ARR quede lo mas grande posible dentro de
 * los 16 bits: cuanto mayor es ARR, mejor es la resolucion del periodo.
 */
#include "timer.h"
#include "gpio.h"
#include "adc_joystick.h"
#include "fsm.h"

TIM_HandleTypeDef htim3;   /* instancia global: la ISR necesita acceder a ella */
TIM_HandleTypeDef htim4;

void timer_init(void)
{
    TIM_ClockConfigTypeDef  src  = {0};  /* fuente de reloj del contador   */
    TIM_MasterConfigTypeDef mst  = {0};  /* configuracion maestro/esclavo  */

    /* ================= TIM3: parpadeo del LED cada 250 ms ============== */
    __HAL_RCC_TIM3_CLK_ENABLE();         /* reloj del periferico TIM3      */

    htim3.Instance           = TIM3;
    htim3.Init.Prescaler     = 10000U - 1U;  /* divide 100 MHz hasta 10 kHz */
    htim3.Init.CounterMode   = TIM_COUNTERMODE_UP;  /* cuenta 0 -> ARR.
                                        Alternativas: DOWN, CENTERALIGNED1/2/3 */
    htim3.Init.Period        = 2500U - 1U;   /* 2500 pasos de 100 us = 250 ms */
    htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;  /* divisor del filtro
                                        digital de las entradas, no del contador */
    htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
                                        /* ARR se escribe de inmediato; con
                                           PRELOAD_ENABLE se aplicaria en el
                                           siguiente evento de actualizacion */

    if (HAL_TIM_Base_Init(&htim3) != HAL_OK) {
        Error_Handler();
    }

    /* Reloj interno del microcontrolador como fuente del contador.
       Alternativas: ETR externo, o el reloj de otro temporizador (ITRx). */
    src.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim3, &src) != HAL_OK) {
        Error_Handler();
    }

    /* TIM3 no dispara a ningun otro periferico: TRGO en RESET */
    mst.MasterOutputTrigger = TIM_TRGO_RESET;
    mst.MasterSlaveMode     = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim3, &mst) != HAL_OK) {
        Error_Handler();
    }

    /* ================= TIM4: base de tiempo de 100 ms ================== */
    __HAL_RCC_TIM4_CLK_ENABLE();

    htim4.Instance               = TIM4;
    htim4.Init.Prescaler         = 10000U - 1U;   /* 10 kHz            */
    htim4.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim4.Init.Period            = 1000U - 1U;    /* 1000 * 100 us = 100 ms */
    htim4.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

    if (HAL_TIM_Base_Init(&htim4) != HAL_OK) {
        Error_Handler();
    }
    src.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
    if (HAL_TIM_ConfigClockSource(&htim4, &src) != HAL_OK) {
        Error_Handler();
    }
    if (HAL_TIMEx_MasterConfigSynchronization(&htim4, &mst) != HAL_OK) {
        Error_Handler();
    }

    /* ================= Interrupciones y arranque ======================= */
    /* Prioridad 7: por debajo de la UART (5) y del ADC (6) */
    HAL_NVIC_SetPriority(TIM3_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(TIM3_IRQn);
    HAL_NVIC_SetPriority(TIM4_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);

    /* _IT arranca el contador y habilita la interrupcion de actualizacion.
       La variante HAL_TIM_Base_Start() arrancaria el contador sin interrupcion,
       obligando a sondear el flag, lo que el enunciado prohibe. */
    HAL_TIM_Base_Start_IT(&htim3);
    HAL_TIM_Base_Start_IT(&htim4);
}

/*
 * HAL_TIM_PeriodElapsedCallback
 * Callback comun a todos los temporizadores: la HAL la invoca desde
 * HAL_TIM_IRQHandler cuando el contador desborda. Se distingue el origen
 * comparando el puntero de instancia.
 *
 * Esta funcion se ejecuta en contexto de interrupcion: debe ser breve y no
 * puede usar HAL_Delay ni operaciones de I2C.
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        /* Cada 250 ms: conmuta el LED de PH1 */
        gpio_led_toggle();
    }
    else if (htim->Instance == TIM4) {
        /* Cada 100 ms: lanza la lectura del joystick y avisa a la FSM */
        adc_joystick_start();          /* arranca la conversion por interrupcion */
        fsm_post_event(EV_TICK_100MS); /* encola el evento periodico             */
    }
}
