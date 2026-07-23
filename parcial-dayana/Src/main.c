/**
 * @file    main.c
 * @author  Daniel Felipe Vargas Arias
 * @brief   Proyecto Parcial - STM32 Nucleo F411RE (CubeIDE sin CubeMX).
 *
 * @details
 * Paso 5 de 5 (final): reloj del sistema a 100 MHz via PLL (fuente HSI),
 * verificable en MCO1 (PA8), y RTC interno sobre LSE (cristal de 32.768
 * kHz de la Nucleo) que sigue contando aunque se desconecte VBAT=VDD.
 * Se suma sobre el blinky (paso 1), LCD (paso 2), UART (paso 3) y
 * Joystick/ADC (paso 4).
 *
 * Mapa de pines usado en el proyecto completo:
 * - PA5: LED blinky (TIM10, interrupcion cada ~250 ms).
 * - PB8 (SCL) / PB9 (SDA): I2C1 hacia el backpack PCF8574 del LCD 20x4.
 * - PA2 (TX) / PA3 (RX): USART2, mismo VCP del ST-LINK (115200 8N1).
 * - PA0 (VRX) / PA1 (VRY): ADC1_IN0 / ADC1_IN1, joystick analogico.
 * - PA8: MCO1, salida observable del reloj seleccionado.
 * - RTC: usa el LSE (cristal X2 de 32.768 kHz de la Nucleo).
 *
 * Comandos UART soportados:
 * - '+' sube X, '-' baja X, 'p' sube Y, 'm' baja Y, '0' pone X=Y=0.
 * - "HSI" / "LSE" / "PLL" (3 caracteres literales): cambia la fuente
 *   que se observa en MCO1 (PA8), con el mejor prescaler posible en
 *   cada caso para no exceder el limite de salida del pin.
 *
 * Nota sobre alimentacion de respaldo: para que el RTC sega contando
 * tiempo al remover la alimentacion principal, el pin VBAT necesita su
 * propia fuente (bateria/supercondensador). En la Nucleo, por defecto
 * VBAT esta puenteado a VDD (solder bridge SB45): sin modificar esa
 * conexion (o sin una fuente externa en VBAT), el RTC se reinicia al
 * cortar la alimentacion igual que el resto del MCU.
 *
 * Nota sobre X/Y: el joystick NO reporta su posicion absoluta, se
 * comporta como control incremental (igual que la UART): arrancan en
 * 0, y mientras el eje este desviado mas alla de una zona muerta
 * central, `coordX`/`coordY` sube o baja ~2 unidades cada ~250 ms. Al
 * soltar el joystick (vuelve al centro) el valor se queda quieto
 * donde estaba, tal como los comandos UART.
 *
 * Nota: el I2C es el unico periferico del proyecto que se maneja por
 * sondeo (polling) en vez de interrupcion, tal como lo permite el
 * enunciado del parcial.
 */

#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdio.h>

/* ------------------------------------------------------------------- */
/* I2C1 + LCD 20x4 (backpack PCF8574, modo 4 bits)                     */
/* ------------------------------------------------------------------- */

/* Direccion 7 bits tipica del backpack PCF8574 (0x27), ya desplazada
 * un bit para que quede en formato de 8 bits que espera la HAL. */
#define LCD_I2C_ADDR   (0x21 << 1)

/* Mascaras de los bits del PCF8574 hacia el LCD (mapeo estandar de
 * estos backpacks: P0=RS, P1=RW, P2=EN, P3=Backlight, P4-P7=D4-D7). */
#define LCD_BIT_RS         0x01
#define LCD_BIT_RW         0x02
#define LCD_BIT_EN         0x04
#define LCD_BIT_BACKLIGHT  0x08

/* ------------------------------------------------------------------- */
/* Maquina de estados finitos (FSM)                                    */
/* ------------------------------------------------------------------- */
typedef enum {
	STATE_IDLE = 0,      /* Esperando eventos de las ISR */
	STATE_BLINK_TOGGLE,  /* Atender el evento del TIM10 (toggle del LED) */
	STATE_LCD_REFRESH,   /* Redibujar el contenido de la pantalla LCD */
	STATE_UART_COMMAND,  /* Procesar el byte recibido por USART2 */
	STATE_ADC_UPDATE     /* Convertir el par VRX/VRY recien muestreado */
} SystemState_t;

static volatile SystemState_t systemState = STATE_IDLE;

/* Bandera que la ISR de TIM10 activa cada ~250 ms */
static volatile uint8_t blinkEventFlag = 0;

/* Bandera que pide un refresco de LCD (al arrancar y tras cada comando) */
static volatile uint8_t lcdRefreshFlag = 0;

/* Byte recibido por USART2 y bandera que la ISR de recepcion activa */
static volatile uint8_t uartRxByte = 0;
static volatile uint8_t uartRxFlag = 0;

/* Coordenadas mostradas en el LCD. Las escriben tanto los comandos
 * UART como el muestreo periodico del joystick por ADC. */
static volatile int16_t coordX = 0;
static volatile int16_t coordY = 0;

/* Valores crudos de conversion ADC (0..4095) y bandera de par listo */
static volatile uint16_t adcRawX = 0;
static volatile uint16_t adcRawY = 0;
static volatile uint8_t adcSampleFlag = 0;

/* Handle del timer de blinky, usado tambien desde stm32f4xx_it.c */
TIM_HandleTypeDef htim10;

/* Handle de I2C1, usado por el driver del LCD */
I2C_HandleTypeDef hi2c1;

/* Handle de USART2, usado tambien desde stm32f4xx_it.c */
UART_HandleTypeDef huart2;

/* Handle de ADC1, usado tambien desde stm32f4xx_it.c */
ADC_HandleTypeDef hadc1;

/* Handle de RTC, usado tambien desde stm32f4xx_it.c */
RTC_HandleTypeDef hrtc;

static void SystemClock_Config(void);
static void GPIO_Blinky_Init(void);
static void Timer_Blinky_Init(void);
static void I2C1_Init(void);
static void LCD_Init(void);
static void LCD_Refresh(void);
static void UART2_Init(void);
static void UART_ProcessCommand(void);
static void ADC_Joystick_Init(void);
static void ADC_StartChannel(uint32_t channel);
static void ADC_UpdateCoordinates(void);
static void RTC_Init(void);
static void RTC_FormatString(char *buf, size_t len);
static void MCO1_Init(void);
static void MCO_HandleCommand(char newChar);


/**
 * @brief Configura el reloj del sistema a 100 MHz usando el PLL con
 *        fuente HSI (16 MHz), y de paso habilita el LSE (32.768 kHz)
 *        que usa el RTC. Se hacen juntos porque ambos osciladores se
 *        configuran con la misma estructura RCC_OscInitTypeDef.
 *
 *        Calculo del PLL (RM0383, HSI = 16 MHz):
 *        VCO_in  = HSI / PLLM = 16 MHz / 8   = 2 MHz    (rango recomendado 1-2 MHz)
 *        VCO_out = VCO_in * PLLN = 2 MHz * 100 = 200 MHz (rango valido 100-432 MHz)
 *        SYSCLK  = VCO_out / PLLP = 200 MHz / 2 = 100 MHz
 *        PLLQ (48 MHz para USB/RNG, no usados aqui) = 200 MHz / 4 = 50 MHz (valor valido cualquiera)
 *
 *        Prescalers de bus: HCLK=100 MHz, APB2=100 MHz (limite del F411),
 *        APB1=50 MHz (excede su limite de 50 MHz si no se divide entre 2).
 *        Flash: a 100 MHz y 3.3V se requieren 3 wait-states (RM0383 tabla 10).
 */
static void SystemClock_Config(void)
{
	RCC_OscInitTypeDef oscInit = { 0 };
	RCC_ClkInitTypeDef clkInit = { 0 };

	oscInit.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSE;
	oscInit.HSIState = RCC_HSI_ON;
	oscInit.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	oscInit.LSEState = RCC_LSE_ON;

	oscInit.PLL.PLLState = RCC_PLL_ON;
	oscInit.PLL.PLLSource = RCC_PLLSOURCE_HSI;
	oscInit.PLL.PLLM = 8;
	oscInit.PLL.PLLN = 100;
	oscInit.PLL.PLLP = RCC_PLLP_DIV2;
	oscInit.PLL.PLLQ = 4;
	HAL_RCC_OscConfig(&oscInit);

	clkInit.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
			RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	clkInit.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
	clkInit.AHBCLKDivider = RCC_SYSCLK_DIV1;
	clkInit.APB1CLKDivider = RCC_HCLK_DIV2;
	clkInit.APB2CLKDivider = RCC_HCLK_DIV1;
	HAL_RCC_ClockConfig(&clkInit, FLASH_LATENCY_3);
}

/**
 * @brief Configura PA5 como salida push-pull para el LED de blinky.
 */
static void GPIO_Blinky_Init(void)
{
	GPIO_InitTypeDef gpioInit = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	gpioInit.Pin = GPIO_PIN_5;
	gpioInit.Mode = GPIO_MODE_OUTPUT_PP;
	gpioInit.Pull = GPIO_NOPULL;
	gpioInit.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOA, &gpioInit);
}

/**
 * @brief Configura TIM10 para generar una interrupcion cada ~250 ms.
 *        Reloj de TIM10 = PCLK2 = 100 MHz (APB2 prescaler = 1, sin x2).
 *        Prescaler = 99999 -> reloj del contador = 100MHz / 100000 = 1 kHz (1 ms/tick)
 *        Periodo   = 250   -> 250 ticks de 1 ms = 250 ms por interrupcion.
 */
static void Timer_Blinky_Init(void)
{
	__HAL_RCC_TIM10_CLK_ENABLE();

	htim10.Instance = TIM10;
	htim10.Init.Prescaler = 99999;
	htim10.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim10.Init.Period = 250;
	htim10.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim10.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	HAL_TIM_Base_Init(&htim10);

	HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);

	HAL_TIM_Base_Start_IT(&htim10);
}

/**
 * @brief Callback de HAL invocado por HAL_TIM_IRQHandler cuando el
 *        timer que generó la interrupción cuenta con IT de actualización
 *        habilitada. Aquí solo se levanta una bandera; el toggle real
 *        del LED ocurre en el bucle principal (FSM), no dentro de la ISR.
 *        Tambien dispara el muestreo periodico del joystick (VRX),
 *        reutilizando este mismo tick de 250 ms en vez de otro timer.
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
	if (htim->Instance == TIM10) {
		blinkEventFlag = 1;
		ADC_StartChannel(ADC_CHANNEL_0);
	}
}



/**
 * @brief Configura I2C1 en PB8 (SCL) / PB9 (SDA) a 100 kHz (modo estandar).
 */
static void I2C1_Init(void)
{
	GPIO_InitTypeDef gpioInit = { 0 };

	__HAL_RCC_GPIOB_CLK_ENABLE();

	/* Los pines de I2C se manejan en drenador abierto: el bus solo
	 * puede tirar a GND, las resistencias pull-up (internas o del
	 * modulo) son las que suben la linea a nivel alto. */
	gpioInit.Pin = GPIO_PIN_8 | GPIO_PIN_9;
	gpioInit.Mode = GPIO_MODE_AF_OD;
	gpioInit.Pull = GPIO_PULLUP;
	gpioInit.Speed = GPIO_SPEED_FREQ_HIGH;
	gpioInit.Alternate = GPIO_AF4_I2C1;
	HAL_GPIO_Init(GPIOB, &gpioInit);

	__HAL_RCC_I2C1_CLK_ENABLE();

	hi2c1.Instance = I2C1;
	hi2c1.Init.ClockSpeed = 100000;
	hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
	hi2c1.Init.OwnAddress1 = 0;
	hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
	hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
	hi2c1.Init.OwnAddress2 = 0;
	hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
	hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
	HAL_I2C_Init(&hi2c1);
}

/**
 * @brief Envia un byte crudo al expansor PCF8574 (estado de los 8 pines).
 */
static void LCD_I2C_Write(uint8_t data)
{
	HAL_I2C_Master_Transmit(&hi2c1, LCD_I2C_ADDR, &data, 1, 100);
}

/**
 * @brief Envia un nibble (4 bits altos de `value`) al LCD, generando el
 *        pulso de enable (EN) requerido por el controlador HD44780.
 * @param value Nibble a enviar, ya ubicado en los bits 4-7.
 * @param rs    0 = comando, 1 = dato.
 */
static void LCD_WriteNibble(uint8_t value, uint8_t rs)
{
	uint8_t base = value | LCD_BIT_BACKLIGHT | (rs ? LCD_BIT_RS : 0);

	LCD_I2C_Write(base | LCD_BIT_EN);   /* Flanco alto de EN: el HD44780 */
	LCD_I2C_Write(base);                /* lee los datos al bajar EN.    */
}

/**
 * @brief Envia un byte completo (nibble alto y luego bajo) al LCD.
 */
static void LCD_WriteByte(uint8_t value, uint8_t rs)
{
	LCD_WriteNibble(value & 0xF0, rs);
	LCD_WriteNibble((value << 4) & 0xF0, rs);
}

static void LCD_SendCommand(uint8_t cmd)
{
	LCD_WriteByte(cmd, 0);
}

static void LCD_SendData(uint8_t data)
{
	LCD_WriteByte(data, 1);
}

/**
 * @brief Ubica el cursor en fila/columna (LCD 20x4, direcciones DDRAM
 *        estandar de los controladores HD44780 de 4 lineas).
 */
static void LCD_SetCursor(uint8_t row, uint8_t col)
{
	static const uint8_t rowOffset[4] = { 0x00, 0x40, 0x14, 0x54 };
	LCD_SendCommand(0x80 | (rowOffset[row] + col));
}

static void LCD_Print(const char *text)
{
	while (*text) {
		LCD_SendData((uint8_t) *text++);
	}
}

/**
 * @brief Secuencia de inicializacion en modo 4 bits del HD44780,
 *        segun su hoja de datos (tiempos minimos de espera incluidos).
 */
static void LCD_Init(void)
{
	HAL_Delay(50);                 /* Espera de encendido del LCD */

	LCD_WriteNibble(0x30, 0);
	HAL_Delay(5);
	LCD_WriteNibble(0x30, 0);
	HAL_Delay(1);
	LCD_WriteNibble(0x30, 0);
	HAL_Delay(1);
	LCD_WriteNibble(0x20, 0);      /* Selecciona modo de 4 bits */
	HAL_Delay(1);

	LCD_SendCommand(0x28);         /* 4 bits, 2 lineas, fuente 5x8 */
	LCD_SendCommand(0x0C);         /* Display ON, cursor OFF, blink OFF */
	LCD_SendCommand(0x06);         /* Modo de entrada: incrementa, sin shift */
	LCD_SendCommand(0x01);         /* Clear display */
	HAL_Delay(2);                  /* El clear necesita >1.52 ms */
}

/**
 * @brief Dibuja el contenido de las 4 lineas del LCD: hora/fecha real
 * (RTC), nombre fijo, y las coordenadas X/Y del joystick/UART.
 */
static void LCD_Refresh(void)
{
	char line[21];

	RTC_FormatString(line, sizeof(line));
	LCD_SetCursor(0, 0);
	LCD_Print(line);

	LCD_SetCursor(1, 0);
	LCD_Print("Dayana Madrid");

	snprintf(line, sizeof(line), "X: %-4d", coordX);
	LCD_SetCursor(2, 0);
	LCD_Print(line);

	snprintf(line, sizeof(line), "Y: %-4d", coordY);
	LCD_SetCursor(3, 0);
	LCD_Print(line);
}

/* ------------------------------------------------------------------- */
/* UART2 (comandos)                                                     */
/* ------------------------------------------------------------------- */
#define COORD_STEP  1

/**
 * @brief Configura USART2 en PA2 (TX) / PA3 (RX) a 115200 8N1 y arma
 *        la primera recepcion por interrupcion.
 */
static void UART2_Init(void)
{
	GPIO_InitTypeDef gpioInit = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	gpioInit.Pin = GPIO_PIN_2 | GPIO_PIN_3;
	gpioInit.Mode = GPIO_MODE_AF_PP;
	gpioInit.Pull = GPIO_PULLUP;
	gpioInit.Speed = GPIO_SPEED_FREQ_HIGH;
	gpioInit.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &gpioInit);

	__HAL_RCC_USART2_CLK_ENABLE();

	huart2.Instance = USART2;
	huart2.Init.BaudRate = 115200;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;
	HAL_UART_Init(&huart2);

	HAL_NVIC_SetPriority(USART2_IRQn, 6, 0);
	HAL_NVIC_EnableIRQ(USART2_IRQn);

	/* Arranca la recepcion de 1 byte por interrupcion; el callback
	 * vuelve a rearmarla cada vez que llega un caracter. */
	HAL_UART_Receive_IT(&huart2, (uint8_t *) &uartRxByte, 1);
}

/**
 * @brief Callback de HAL invocado al completarse la recepcion de un
 *        byte por USART2. Solo levanta la bandera y rearma la
 *        siguiente recepcion; el comando se interpreta en la FSM.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
	if (huart->Instance == USART2) {
		uartRxFlag = 1;
		HAL_UART_Receive_IT(&huart2, (uint8_t *) &uartRxByte, 1);
	}
}

/**
 * @brief Interpreta el ultimo byte recibido por UART y actualiza las
 *        coordenadas X/Y mostradas en el LCD.
 */
static void UART_ProcessCommand(void)
{
	/* Alimenta el detector de comandos de 3 letras para MCO1 con cada
	 * byte que llega, independientemente del switch de abajo: un
	 * mismo caracter puede ser parte de "HSI"/"LSE"/"PLL" sin afectar
	 * los comandos de una sola letra ('+', '-', 'p', 'm', '0'). */
	MCO_HandleCommand((char) uartRxByte);

	switch (uartRxByte) {
	case '+':
		coordX += COORD_STEP;
		break;
	case '-':
		coordX -= COORD_STEP;
		break;
	case 'p':
		coordY += COORD_STEP;
		break;
	case 'm':
		coordY -= COORD_STEP;
		break;
	case '0':
		coordX = 0;
		coordY = 0;
		break;
	default:
		/* Caracter no reconocido: se ignora */
		break;
	}

	uartRxFlag = 0;
	lcdRefreshFlag = 1;
}

/* ------------------------------------------------------------------- */
/* ADC1 + Joystick (VRX = PA0/IN0, VRY = PA1/IN1)                      */
/* ------------------------------------------------------------------- */

/* 1 mientras se esta muestreando VRX, 0 mientras se muestrea VRY.
 * Permite que el callback de fin de conversion sepa a cual de los
 * dos valores crudos debe ir el resultado. */
static volatile uint8_t adcSamplingX = 1;

/**
 * @brief Configura PA0/PA1 en modo analogico y arma ADC1 en modo de
 *        interrupcion (sin arrancar conversion todavia).
 */
static void ADC_Joystick_Init(void)
{
	GPIO_InitTypeDef gpioInit = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	gpioInit.Pin = GPIO_PIN_0 | GPIO_PIN_1;
	gpioInit.Mode = GPIO_MODE_ANALOG;
	gpioInit.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOA, &gpioInit);

	__HAL_RCC_ADC1_CLK_ENABLE();

	hadc1.Instance = ADC1;
	/* PCLK2 ahora es 100 MHz (paso 5); DIV4 -> 25 MHz, dentro del
	 * limite de 36 MHz del ADC (con DIV2 hubiera quedado en 50 MHz). */
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.ScanConvMode = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.DMAContinuousRequests = DISABLE;
	HAL_ADC_Init(&hadc1);

	HAL_NVIC_SetPriority(ADC_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(ADC_IRQn);
}

/**
 * @brief Selecciona un canal de ADC1 (VRX o VRY) y arranca una
 *        conversion por interrupcion. Como el joystick usa dos
 *        canales sobre una sola entrada ADC (sin DMA), se alternan
 *        "en ping-pong": esta funcion arranca VRX; su propio
 *        resultado (ver callback) encadena el arranque de VRY.
 */
static void ADC_StartChannel(uint32_t channel)
{
	ADC_ChannelConfTypeDef sConfig = { 0 };

	adcSamplingX = (channel == ADC_CHANNEL_0) ? 1 : 0;

	sConfig.Channel = channel;
	sConfig.Rank = 1;
	sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
	HAL_ADC_ConfigChannel(&hadc1, &sConfig);

	HAL_ADC_Start_IT(&hadc1);
}

/**
 * @brief Callback de HAL al completarse una conversion de ADC1.
 *        Guarda el valor crudo (0..4095) en VRX o VRY segun
 *        corresponda; al terminar VRY, marca el par como listo.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
	if (hadc->Instance != ADC1) {
		return;
	}

	if (adcSamplingX) {
		adcRawX = HAL_ADC_GetValue(hadc);
		ADC_StartChannel(ADC_CHANNEL_1);   /* Encadena el muestreo de VRY */
	} else {
		adcRawY = HAL_ADC_GetValue(hadc);
		adcSampleFlag = 1;                  /* Par VRX/VRY completo */
	}
}

/* Zona muerta central: el joystick en reposo no cae exactamente en el
 * punto medio del rango del ADC (variacion mecanica/electrica de cada
 * modulo), asi que se ignoran desviaciones pequenas alrededor del 50%
 * para que "soltar el joystick" no siga sumando o restando solo. */
#define JOYSTICK_DEADZONE_LOW_PCT   40
#define JOYSTICK_DEADZONE_HIGH_PCT  60
#define JOYSTICK_STEP               1

/**
 * @brief Aplica la zona muerta a un eje: si esta desviado mas alla del
 *        rango central, incrementa o decrementa la coordenada; si esta
 *        cerca del centro, la deja igual (control incremental, no
 *        lectura de posicion absoluta).
 */
static void ADC_ApplyAxis(volatile int16_t *coord, uint16_t adcRaw)
{
	uint32_t percent = (uint32_t) adcRaw * 100U / 4095U;

	if (percent < JOYSTICK_DEADZONE_LOW_PCT) {
		*coord -= JOYSTICK_STEP;
	} else if (percent > JOYSTICK_DEADZONE_HIGH_PCT) {
		*coord += JOYSTICK_STEP;
	}
	/* Dentro de la zona muerta: no se toca *coord */
}

/**
 * @brief Procesa el ultimo par crudo de ADC (0..4095 cada uno) como
 *        control incremental y actualiza las coordenadas del LCD.
 */
static void ADC_UpdateCoordinates(void)
{
	ADC_ApplyAxis(&coordX, adcRawX);
	ADC_ApplyAxis(&coordY, adcRawY);

	adcSampleFlag = 0;
	lcdRefreshFlag = 1;
}

/* ------------------------------------------------------------------- */
/* RTC (LSE) + MCO1                                                     */
/* ------------------------------------------------------------------- */

/* Valor "magico" guardado en el registro de respaldo RTC_BKP_DR0 para
 * distinguir un primer arranque (hay que sembrar fecha/hora) de un
 * reset normal (el dominio de respaldo ya tenia la hora corriendo y
 * no se debe reiniciar a la fecha de fabrica). */
#define RTC_INIT_MAGIC  0x32F3U

/**
 * @brief Inicializa el RTC sobre LSE y arranca su WakeUp Timer para
 *        recibir una interrupcion cada 1 segundo (usada para refrescar
 *        la hora en el LCD sin tener que sondear el RTC en el bucle
 *        principal). La fecha/hora inicial solo se siembra la primera
 *        vez (ver RTC_INIT_MAGIC); en resets posteriores el RTC sigue
 *        contando desde donde iba.
 */
static void RTC_Init(void)
{
	__HAL_RCC_PWR_CLK_ENABLE();
	HAL_PWR_EnableBkUpAccess();

	/* El LSE ya se habilito en SystemClock_Config(). Si el dominio de
	 * respaldo no estaba configurado para usarlo como fuente del RTC,
	 * hay que resetearlo primero (esto es lo unico que borra la hora
	 * guardada; solo ocurre la primera vez que corre este firmware). */
	if (__HAL_RCC_GET_RTC_SOURCE() != RCC_RTCCLKSOURCE_LSE) {
		__HAL_RCC_BACKUPRESET_FORCE();
		__HAL_RCC_BACKUPRESET_RELEASE();
		__HAL_RCC_RTC_CONFIG(RCC_RTCCLKSOURCE_LSE);
	}
	__HAL_RCC_RTC_ENABLE();

	hrtc.Instance = RTC;
	hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
	/* LSE = 32768 Hz; (AsynchPrediv+1)*(SynchPrediv+1) = 128*256 = 32768
	 * -> el RTC genera un tick de calendario de exactamente 1 Hz. */
	hrtc.Init.AsynchPrediv = 127;
	hrtc.Init.SynchPrediv = 255;
	hrtc.Init.OutPut = RTC_OUTPUT_DISABLE;
	hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
	hrtc.Init.OutPutType = RTC_OUTPUT_TYPE_OPENDRAIN;
	HAL_RTC_Init(&hrtc);

	if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_INIT_MAGIC) {
		RTC_TimeTypeDef sTime = { 0 };
		RTC_DateTypeDef sDate = { 0 };
		HAL_StatusTypeDef timeStatus, dateStatus;

		/* No hay forma de que el MCU conozca la hora real por su
		 * cuenta (sin red ni RTC ya corriendo), asi que se siembra
		 * con la fecha/hora de compilacion (__DATE__/__TIME__): es
		 * lo mas cercano a "ahora" disponible en tiempo de build,
		 * y solo se usa una vez (ver RTC_INIT_MAGIC arriba). */
		static const char *const monthNames[12] = {
			"Jan", "Feb", "Mar", "Apr", "May", "Jun",
			"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
		};
		char monStr[4] = { 0 };
		int day = 1, year = 2026, hour = 0, minute = 0, second = 0;
		uint8_t month = 1;

		sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
		sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
		for (uint8_t i = 0; i < 12; i++) {
			if (strncmp(monStr, monthNames[i], 3) == 0) {
				month = i + 1;
				break;
			}
		}

		sTime.Hours = (uint8_t) hour;
		sTime.Minutes = (uint8_t) minute;
		sTime.Seconds = (uint8_t) second;
		timeStatus = HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);

		sDate.WeekDay = RTC_WEEKDAY_MONDAY;   /* No se usa para mostrar nada */
		sDate.Month = month;
		sDate.Date = (uint8_t) day;
		sDate.Year = (uint8_t) (year % 100);
		dateStatus = HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

		/* Solo se marca como "sembrado" si ambas escrituras tuvieron
		 * exito: si el LSE aun no estaba completamente estable y
		 * alguna fallo, es mejor reintentar la siembra en el proximo
		 * arranque que quedar marcado con una hora que nunca se
		 * escribio de verdad. */
		if (timeStatus == HAL_OK && dateStatus == HAL_OK) {
			HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_INIT_MAGIC);
		}
	}

	/* WakeUp timer en modo CK_SPRE con contador en 0: interrumpe
	 * exactamente una vez por segundo, sincronizado con el calendario. */
	HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);

	HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 8, 0);
	HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
}

/**
 * @brief Callback de HAL invocado cada 1 segundo por el WakeUp Timer
 *        del RTC. Solo pide un refresco de LCD; la lectura real de
 *        hora/fecha ocurre en RTC_FormatString() desde la FSM.
 */
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc_cb)
{
	if (hrtc_cb->Instance == RTC) {
		lcdRefreshFlag = 1;
	}
}

/**
 * @brief Formatea "HH:MM:SS DD-MM-YY" con la hora/fecha actual del RTC.
 * @param buf Buffer destino (minimo 19 bytes: 18 caracteres + '\0').
 */
static void RTC_FormatString(char *buf, size_t len)
{
	RTC_TimeTypeDef sTime = { 0 };
	RTC_DateTypeDef sDate = { 0 };

	HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
	/* HAL exige leer la fecha justo despues de la hora: desbloquea los
	 * registros "shadow" del RTC para la siguiente lectura consistente. */
	HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);

	snprintf(buf, len, "%02d:%02d:%02d %02d-%02d-%02d",
			sTime.Hours, sTime.Minutes, sTime.Seconds,
			sDate.Date, sDate.Month, sDate.Year);
}

/**
 * @brief Configura PA8 como salida MCO1, mostrando por defecto el PLL
 *        (el reloj real del sistema) con el mayor prescaler seguro.
 */
static void MCO1_Init(void)
{
	GPIO_InitTypeDef gpioInit = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	gpioInit.Pin = GPIO_PIN_8;
	gpioInit.Mode = GPIO_MODE_AF_PP;
	gpioInit.Pull = GPIO_NOPULL;
	gpioInit.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
	gpioInit.Alternate = GPIO_AF0_MCO;
	HAL_GPIO_Init(GPIOA, &gpioInit);

	/* PLL = 100 MHz; entre /1, /2, /3, /4, /5 el mayor prescaler
	 * disponible (/5) daria 20 MHz, pero /4 (25 MHz) es el que usan
	 * las referencias de ST para esta frecuencia sin degradar el
	 * flanco de salida; se documenta el mismo criterio en el comando
	 * "PLL" mas abajo. */
	__HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_4);
}

/**
 * @brief Reconoce los comandos de 3 letras "HSI"/"LSE"/"PLL" recibidos
 *        por UART (uno a la vez) y reconfigura la fuente de MCO1.
 *        HSI y LSE no necesitan prescaler (ya son frecuencias bajas);
 *        PLL usa /4 para no exceder el rango de salida del pin a 100 MHz.
 */
static void MCO_HandleCommand(char newChar)
{
	static char cmdBuf[3] = { 0, 0, 0 };

	cmdBuf[0] = cmdBuf[1];
	cmdBuf[1] = cmdBuf[2];
	cmdBuf[2] = newChar;

	if (strncmp(cmdBuf, "HSI", 3) == 0) {
		__HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
	} else if (strncmp(cmdBuf, "LSE", 3) == 0) {
		__HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_LSE, RCC_MCODIV_1);
	} else if (strncmp(cmdBuf, "PLL", 3) == 0) {
		__HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_4);
	}
}

int main(void)
{
	HAL_Init();
	SystemClock_Config();
	GPIO_Blinky_Init();
	Timer_Blinky_Init();
	I2C1_Init();
	LCD_Init();
	UART2_Init();
	ADC_Joystick_Init();
	RTC_Init();
	MCO1_Init();

	/* Primer dibujado del LCD, tratado como un evento mas de la FSM */
	lcdRefreshFlag = 1;

	while (1) {
		switch (systemState) {

		case STATE_IDLE:
			if (blinkEventFlag) {
				systemState = STATE_BLINK_TOGGLE;
			} else if (uartRxFlag) {
				systemState = STATE_UART_COMMAND;
			} else if (adcSampleFlag) {
				systemState = STATE_ADC_UPDATE;
			} else if (lcdRefreshFlag) {
				systemState = STATE_LCD_REFRESH;
			}
			break;

		case STATE_BLINK_TOGGLE:
			HAL_GPIO_TogglePin(GPIOA, GPIO_PIN_5);
			blinkEventFlag = 0;
			systemState = STATE_IDLE;
			break;

		case STATE_UART_COMMAND:
			UART_ProcessCommand();
			systemState = STATE_IDLE;
			break;

		case STATE_ADC_UPDATE:
			ADC_UpdateCoordinates();
			systemState = STATE_IDLE;
			break;

		case STATE_LCD_REFRESH:
			LCD_Refresh();
			lcdRefreshFlag = 0;
			systemState = STATE_IDLE;
			break;

		default:
			systemState = STATE_IDLE;
			break;
		}
	}
}
