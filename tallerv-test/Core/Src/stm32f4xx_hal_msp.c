/*
 * stm32f4xx_hal_msp.c
 * MSP: Microcontroller Support Package.
 *
 * La HAL separa la configuracion logica de un periferico (registros de modo,
 * velocidad, formato) de su conexion fisica (reloj de bus, pines, NVIC).
 * La primera vive en el modulo del periferico; la segunda se centraliza aqui,
 * porque HAL_<PPP>_Init() invoca automaticamente a HAL_<PPP>_MspInit().
 *
 * Reunir toda la asignacion de pines en un unico archivo facilita detectar
 * conflictos de recursos, que es el error mas comun al ampliar un proyecto.
 *
 * Mapa de pines completo:
 *   PA0  ADC1_IN0    VRX del joystick
 *   PA1  ADC1_IN1    VRY del joystick
 *   PA2  USART2_TX   consola serie (via ST-LINK)
 *   PA3  USART2_RX   consola serie (via ST-LINK)
 *   PA8  MCO1        salida de reloj conmutable
 *   PB5  EXTI5       pulsador SW del joystick
 *   PB8  I2C1_SCL    reloj del bus del LCD
 *   PB9  I2C1_SDA    datos del bus del LCD
 *   PH1  GPIO out    LED blinky
 *   PC14 OSC32_IN    cristal LSE de 32.768 kHz
 *   PC15 OSC32_OUT   cristal LSE de 32.768 kHz
 */
#include "main.h"

/*
 * HAL_MspInit
 * Inicializacion global, invocada desde HAL_Init().
 */
void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();  /* necesario para el multiplexor de EXTI */
    __HAL_RCC_PWR_CLK_ENABLE();     /* control del regulador y del backup    */
}

/*
 * HAL_UART_MspInit
 * Pines de la USART2. En la Nucleo-F411RE estan cableados al ST-LINK, que
 * los expone como puerto serie virtual.
 */
void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef gi = {0};

    if (huart->Instance != USART2) {
        return;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gi.Pin       = GPIO_PIN_2 | GPIO_PIN_3;   /* TX y RX */
    gi.Mode      = GPIO_MODE_AF_PP;           /* funcion alternativa push-pull */
    gi.Pull      = GPIO_PULLUP;               /* mantiene la linea en reposo a 1,
                                                 evitando tramas espurias si el
                                                 cable se desconecta */
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF7_USART2;           /* AF7 = USART1/2/3 en el F411   */
    HAL_GPIO_Init(GPIOA, &gi);
}

/*
 * HAL_ADC_MspInit
 * Pines analogicos del joystick.
 */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    GPIO_InitTypeDef gi = {0};

    if (hadc->Instance != ADC1) {
        return;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();

    gi.Pin  = GPIO_PIN_0 | GPIO_PIN_1;   /* VRX y VRY */
    gi.Mode = GPIO_MODE_ANALOG;          /* desconecta las etapas digitales
                                            del pin, reduciendo la corriente
                                            de fuga y el ruido inyectado */
    gi.Pull = GPIO_NOPULL;               /* cualquier pull falsearia la lectura
                                            al formar un divisor con el
                                            potenciometro del joystick */
    HAL_GPIO_Init(GPIOA, &gi);
}

/*
 * HAL_I2C_MspInit
 * Bus I2C1 hacia el modulo LCD.
 */
void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    GPIO_InitTypeDef gi = {0};

    if (hi2c->Instance != I2C1) {
        return;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* I2C1 admite dos mapeos de pines. El del proyecto es PB8/PB9; muchos
       montajes previos usan PB6/PB7. Compilar con -DLCD_USE_PB6_PB7 para
       cambiar de uno a otro sin recablear. */
#ifdef LCD_USE_PB6_PB7
    gi.Pin       = GPIO_PIN_6 | GPIO_PIN_7;   /* SCL = PB6, SDA = PB7 */
#else
    gi.Pin       = GPIO_PIN_8 | GPIO_PIN_9;   /* SCL = PB8, SDA = PB9 */
#endif
    gi.Mode      = GPIO_MODE_AF_OD;           /* drenador abierto: obligatorio
                                                 en I2C, donde varios nodos
                                                 comparten la linea */
    gi.Pull      = GPIO_PULLUP;               /* pull-up interna de apoyo; el
                                                 modulo LCD ya incorpora las
                                                 suyas de 4.7 kohm */
    gi.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gi.Alternate = GPIO_AF4_I2C1;             /* AF4 = I2C1/2/3 en el F411  */
    HAL_GPIO_Init(GPIOB, &gi);
}

/*
 * HAL_RTC_MspInit
 * El RTC no usa pines externos en esta aplicacion: el cristal LSE se conecta
 * a PC14 y PC15, que quedan bajo control del dominio de respaldo y no
 * requieren configuracion GPIO.
 */
void HAL_RTC_MspInit(RTC_HandleTypeDef *hrtc)
{
    (void)hrtc;
    /* El reloj del RTC ya se habilito en rtc_init con __HAL_RCC_RTC_ENABLE */
}
