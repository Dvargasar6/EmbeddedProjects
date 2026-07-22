/**
 * board.c - Nucleo-F411RE board support, written by hand (no CubeMX).
 *
 * Provides:
 *   SystemClock_Config()  HSI 16 MHz sin PLL (configuracion identica a tarea3)
 *   Board_GPIO_Init()     heartbeat LED (PH1, external) and B1 (PC13)
 *   Error_Handler()       fast blink on PH1, never returns
 */

#include "board.h"

/**
 * Configuracion de reloj replicada EXACTAMENTE de main_tarea3.c
 * (configuracion validada empiricamente en esta placa concreta):
 *
 *   Fuente: HSI interno de 16 MHz, sin PLL.
 *   SYSCLK = AHB = APB1 = APB2 = 16 MHz (todos los prescalers en /1).
 *   Latencia de flash: 0 wait states (valida hasta 30 MHz).
 *
 * Consecuencia importante: los timers de APB1 cuentan a 16 MHz
 * (prescaler de bus = 1, por lo que NO aplica la regla x2).
 * Todas las constantes de periferico del proyecto se derivan de 16 MHz.
 */
void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    /* Oscilador interno HSI de 16 MHz, sin PLL */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) Error_Handler();

    /* SYSCLK desde HSI; todos los buses a 16 MHz */
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                       | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0) != HAL_OK) Error_Handler();
}

void Board_GPIO_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOH_CLK_ENABLE();                     /* port clocks must be on before use */
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* LED de heartbeat en PH1 (OSC_OUT): libre como GPIO porque el reloj
       del sistema es el HSI interno y los pines de oscilador no se usan.
       LED externo, push-pull, activo en alto. */
    g.Pin   = HB_LED_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(HB_LED_PORT, &g);
    LED_OFF();

    /* B1 (PC13): input; the board provides the pull-up, reads LOW when pressed */
    g.Pin  = GPIO_PIN_13;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &g);
}

TIM_HandleTypeDef htim5;

/**
 * TIM5 como base de tiempo del heartbeat, valores identicos a tarea3:
 *   16 MHz / (15999+1) = 1 kHz de conteo
 *   1 kHz / (499+1)    = interrupcion cada 500 ms -> toggle -> parpadeo 1 Hz
 */
void Heartbeat_Init(void)
{
    __HAL_RCC_TIM5_CLK_ENABLE();                       /* reloj del periferico primero */

    htim5.Instance               = TIM5;
    htim5.Init.Prescaler         = 15999;              /* -> conteo a 1 kHz */
    htim5.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim5.Init.Period            = 499;                /* -> update cada 500 ms */
    htim5.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_Base_Init(&htim5) != HAL_OK) Error_Handler();

    HAL_NVIC_SetPriority(TIM5_IRQn, 5, 0);             /* misma prioridad que tarea3 */
    HAL_NVIC_EnableIRQ(TIM5_IRQn);

    HAL_TIM_Base_Start_IT(&htim5);                     /* arranca el conteo con IRQ */
}

/* Switch between 1 Hz (healthy) and 5 Hz (LCD not detected) */
void Heartbeat_SetFast(uint8_t fast)
{
    __HAL_TIM_SET_AUTORELOAD(&htim5, fast ? 99 : 499);   /* 0.1 s vs 0.5 s por toggle */
    __HAL_TIM_SET_COUNTER(&htim5, 0);                     /* apply immediately */
}

/* HAL routes the TIM5 update interrupt here (weak override) */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM5) {
        LED_TOGGLE();                                  /* heartbeat on PH1, nothing else */
    }
}

/**
 * Terminal error state: the heartbeat LED (PH1) blinks fast forever.
 * Used before UART/LCD exist; later phases also report errors there.
 */
void Error_Handler(void)
{
    __disable_irq();
    while (1) {
        LED_TOGGLE();
        for (volatile uint32_t i = 0; i < 400000; i++);  /* crude delay: SysTick may be unusable here */
    }
}
