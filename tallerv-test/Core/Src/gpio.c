/*
 * gpio.c
 * Configuracion de los pines discretos: LED en PH1 y pulsador del joystick.
 */
#include "gpio.h"

/*
 * gpio_init
 * Habilita los relojes de los puertos implicados y configura cada pin.
 * Nota: en el STM32F4 los relojes de periferico estan apagados tras el reset;
 * escribir en los registros de un puerto sin reloj no produce ningun efecto
 * y tampoco genera error, lo que hace que el fallo sea dificil de detectar.
 */
void gpio_init(void)
{
    GPIO_InitTypeDef gi = {0};   /* estructura de configuracion de pin */

    /* --- Relojes de los puertos utilizados ----------------------------- */
    __HAL_RCC_GPIOH_CLK_ENABLE();   /* puerto H: LED en PH1               */
    __HAL_RCC_GPIOB_CLK_ENABLE();   /* puerto B: pulsador en PB5          */

    /* --- LED en PH1 ----------------------------------------------------- */
    /* Estado inicial apagado, escrito antes de configurar el pin como salida
       para evitar un pulso espurio al habilitar el driver de salida. */
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_PIN, GPIO_PIN_RESET);

    gi.Pin   = LED_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;  /* push-pull: el pin puede forzar 0 y 1.
                                        Alternativa: GPIO_MODE_OUTPUT_OD
                                        (drenador abierto), que solo fuerza 0 */
    gi.Pull  = GPIO_NOPULL;          /* sin resistencias internas: es salida  */
    gi.Speed = GPIO_SPEED_FREQ_LOW;  /* 2 MHz basta para 4 Hz; velocidades
                                        mayores solo aumentan el ruido y el
                                        consumo. Alternativas: MEDIUM, HIGH,
                                        VERY_HIGH */
    HAL_GPIO_Init(LED_GPIO_PORT, &gi);

    /* --- Pulsador SW del joystick en PB5 -------------------------------- */
    gi.Pin  = JOY_SW_PIN;
    gi.Mode = GPIO_MODE_IT_FALLING;  /* interrupcion en flanco de bajada.
                                        El SW del joystick cortocircuita el
                                        pin a GND al pulsarlo. Alternativas:
                                        IT_RISING, IT_RISING_FALLING */
    gi.Pull = GPIO_PULLUP;           /* pull-up interna: el pin reposa en 1
                                        y cae a 0 al pulsar. Evita anadir una
                                        resistencia externa */
    HAL_GPIO_Init(JOY_SW_GPIO_PORT, &gi);

    /* --- Habilitacion de la interrupcion en el NVIC ---------------------- */
    /* Prioridad de expropiacion 8, subprioridad 0. Valor alto = prioridad
       baja: el pulsador no debe interferir con la UART ni con el ADC. */
    HAL_NVIC_SetPriority(JOY_SW_EXTI_IRQn, 8, 0);
    HAL_NVIC_EnableIRQ(JOY_SW_EXTI_IRQn);
}

/* Invierte el nivel logico del LED. Se llama desde la ISR de TIM3. */
void gpio_led_toggle(void)
{
    HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
}

/* Fija el LED a un estado concreto: on distinto de 0 lo enciende. */
void gpio_led_write(uint8_t on)
{
    HAL_GPIO_WritePin(LED_GPIO_PORT, LED_PIN,
                      on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
