/**
 * @file    main_tarea3.c
 * @author  Daniel Felipe Vargas Arias
 * @date    9 de julio de 2026
 * @brief   Código fuente principal para la Tarea #3 (ADC, DAC/PWM, USART, Encoder).
 *
 * @details
 * Asignatura: Taller V (2.0) - 1er Semestre 2026.
 * Profesor: Nerio Andres Montoya Giraldo, PhD.
 * Plataforma: Microcontrolador STM32F411RE.
 *
 * Mapeo de Pines y Periféricos (Hardware):
 * - USART2 (Comandos) : PA2 (TX) / PA3 (RX) [Típico enlace USB-Serial Nucleo]
 * - TIM2 (Encoder)    : PA0 (TIM2_CH1 / TI1) y PA1 (TIM2_CH2 / TI2) en AF1.
 * - TIM3 (LED RGB)    : CH1 (Rojo), CH2 (Verde), CH4 (Azul).
 * - TIM4 (TRGO/Base)  : PB9 (TIM4_CH4) configurado en evento interno (sin salida física).
 * - ADC1 (Potenciómetro): PC2 (Canal analógico 12 / ADC1_IN12).
 *
 * Descripción general de la arquitectura:
 * El sistema implementa una máquina de estados finitos guiada por eventos
 * para controlar los canales independientes de un LED RGB mediante
 * tres mecanismos de entrada distintos:
 *
 * 1. USART2: Recepción asíncrona de comandos por interrupción a 19200 baudios. Modifica
 *    el ancho de pulso del canal rojo.
 * 2. Encoder (TIM2): Interfaz de decodificación en cuadratura por hardware esclavo, sin
 *    uso de interrupciones de software. Modifica el ancho de pulso del canal verde.
 * 3. ADC1 (12 bits): Muestreo analógico disparado por hardware mediante la coincidencia
 *    del canal 4 (CC4) del TIM4 operando cada 50 ms. La lectura del potenciómetro modifica
 *    el canal azul procesado en el callback de fin de conversión (EOC).
 *
 * Se incluye adicionalmente un indicador visual de estado (Blinky) gestionado por el
 * desbordamiento periódico del TIM5. Toda la configuración de periféricos se realiza
 * mediante la biblioteca de abstracción de hardware (HAL) de STMicroelectronics.
 *
 * Los comandos de control para comunicacion serial son:
 * +/-: Aumenta y disminuye la intensidad del LED rojo.
 * a: Apaga completamente los LEDs rojo y verde.
 * b: Enciende completamente los LEDs rojo y verde.
 *
 * El LED azul conectado al canal del ADC no se puede controlar mediante serial.
 * Iniciar el control serial a 19200 baudios y oprimir reset.
 */


#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdio.h>

/*
 * fsm_state_t:
 * Enumeracion de estados.
 */
typedef enum {
	ST_IDLE = 0,       // reposo, muestreando fuentes de eventos
	ST_PROC_SERIAL,    // procesando byte recibido por USART
	ST_PROC_ADC,       // procesando resultado del ADC
	ST_PROC_ENCODER    // procesando movimiento del encoder
} fsm_state_t;

/* Estado actual de la FSM. */
static fsm_state_t fsm_state = ST_IDLE;

/* Manejadores globales */
TIM_HandleTypeDef htim2 = { 0 };    // Encoder (32 bits)
TIM_HandleTypeDef htim3 = { 0 };    // PWM RGB
TIM_HandleTypeDef htim4 = { 0 }; // TRGO (Trigger Output) para disparar ADC cada 50 ms
TIM_HandleTypeDef htim5 = { 0 };    // Blinky
UART_HandleTypeDef huart2 = { 0 };  // USART2
ADC_HandleTypeDef hadc1 = { 0 };    // ADC1 sobre PC2 (canal 12)

/* Variables utilizadas en ISRs */
volatile uint8_t rx_data = 0;        // byte recibido por USART2
volatile uint8_t rx_flag = 0;        // bandera de USART
volatile uint16_t adc_raw = 0;       // ultima conversion ADC (0..4095)
volatile uint8_t adc_flag = 0;       // bandera de ADC

/* Estado actual del duty de cada canal */
uint16_t duty_r = 0;
uint16_t duty_g = 0;
uint16_t duty_b = 0;

/* Posicion previa del encoder */
int32_t enc_pos_prev = 0;

/* Prototipos */
static void clock_Init(void);
static void pwm_Init(void);
static void blinky_Init(void);
static void usart_Init(void);
static void encoder_Init(void);
static void trgo_tim_Init(void);
static void adc_Init(void);
static void process_rx(void);
static void process_encoder(void);
static void process_adc(void);
static void update_green(int16_t delta);

/*
 * clock_Init:
 * Funcion de configuracion de las senales de reloj
 * que se utilizan en el programa.
 * Estas son: SYSCLK, AHB, APB1 y APB2.
 * Utiliza el oscilador HSI de 16 MHz
 */
static void clock_Init(void) {

	RCC_OscInitTypeDef RCC_OscInitStruct = { 0 };
	RCC_ClkInitTypeDef RCC_ClkInitStruct = { 0 };

	/*
	 * Configuracion del oscilador HSI de 16 MHz.
	 * Para usar una frecuencia diferente se puede usar PLL
	 * que es una senal de base lenta (8 MHz)
	 * y se multiplica para lograr frecuencias hasta 72 MHz.
	 */

	RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
	RCC_OscInitStruct.HSIState = RCC_HSI_ON;
	RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
	RCC_OscInitStruct.PLL.PLLState = RCC_PLL_NONE;

	// Se aplica la estructura configurada con la funcion de HAL:
	HAL_RCC_OscConfig(&RCC_OscInitStruct);

	/*
	 * Aplicar la configuracion a los demas relojes del sistema:
	 * SYSCLK, AHB, APB1, APB2.
	 */
	RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
	RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
	RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;

	// Pre-scalers en 1:
	RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
	RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
	RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

	// Se aplica la estructura configurada con la funcion de HAL:
	HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

/*
 * pwm_Init:
 * Funcion de configuracion de los tres canales PWM.
 * Estos canales estan respectivamente en los pines:
 * PA6, PA7 y PB1.
 * Configura TIM3 a 2 kHz respetando la restriccion 1 kHz - 5 kHz.
 */
static void pwm_Init(void) {

	// Estructura de configuracion de los GPIO:
	GPIO_InitTypeDef GPIO_PWM = { 0 };

	// Habilitacion de los relojes de los puertos A y B:
	__HAL_RCC_GPIOA_CLK_ENABLE();
	__HAL_RCC_GPIOB_CLK_ENABLE();

	/*
	 * Configuracion de los pines del puerto A: PA6 y PA7.
	 * Se configuran en Alternative Function.
	 */
	GPIO_PWM.Pin = GPIO_PIN_6 | GPIO_PIN_7;
	GPIO_PWM.Mode = GPIO_MODE_AF_PP;
	GPIO_PWM.Pull = GPIO_NOPULL;
	GPIO_PWM.Speed = GPIO_SPEED_FREQ_LOW;
	GPIO_PWM.Alternate = GPIO_AF2_TIM3;
	HAL_GPIO_Init(GPIOA, &GPIO_PWM);

	// Se aplica la misma configuracion para PB1:
	GPIO_PWM.Pin = GPIO_PIN_1;
	HAL_GPIO_Init(GPIOB, &GPIO_PWM);

	// Habilitar el reloj de TIM3:
	__HAL_RCC_TIM3_CLK_ENABLE();

	/*
	 * Configuracion de TIM3:
	 */
	htim3.Instance = TIM3;
	htim3.Init.Prescaler = 7;
	htim3.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim3.Init.Period = 999;
	htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
	HAL_TIM_PWM_Init(&htim3);

	/*
	 * Configuracion de los canales OC (Output Compare) para PWM:
	 */
	TIM_OC_InitTypeDef sConfigOC = { 0 };
	sConfigOC.OCMode = TIM_OCMODE_PWM1;
	sConfigOC.Pulse = 0;
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;

	// Aplica la configuracion de OC a los canales 1,2 y 4:
	HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_1);
	HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_2);
	HAL_TIM_PWM_ConfigChannel(&htim3, &sConfigOC, TIM_CHANNEL_4);

	// Inicia la generacion de la senal PWM y el conteo de TIM3:
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_1);
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_2);
	HAL_TIM_PWM_Start(&htim3, TIM_CHANNEL_4);
}

/*
 * blinky_Init:
 * Funcion que configura el blinky de estado.
 * Se implementa en el pin PH1.
 * TIM5 con interrupcion cada 500 ms:
 */
static void blinky_Init(void) {

	GPIO_InitTypeDef GPIO_Blinky = { 0 };

	__HAL_RCC_GPIOH_CLK_ENABLE();

	GPIO_Blinky.Pin = GPIO_PIN_1;
	GPIO_Blinky.Mode = GPIO_MODE_OUTPUT_PP;
	GPIO_Blinky.Pull = GPIO_NOPULL;
	GPIO_Blinky.Speed = GPIO_SPEED_FREQ_LOW;
	HAL_GPIO_Init(GPIOH, &GPIO_Blinky);

	__HAL_RCC_TIM5_CLK_ENABLE();

	htim5.Instance = TIM5;
	htim5.Init.Prescaler = 15999;
	htim5.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim5.Init.Period = 499;
	htim5.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim5.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

	// Inicializa los registros base de TIM5:
	HAL_TIM_Base_Init(&htim5);

	// Prioridad de la interrupcion de TIM5:
	HAL_NVIC_SetPriority(TIM5_IRQn, 5, 0);

	// Habilita la interrupcion de TIM5 en el NVIC:
	HAL_NVIC_EnableIRQ(TIM5_IRQn);

	// Inicia el temporizador de TIM5:
	HAL_TIM_Base_Start_IT(&htim5);
}

/*
 * 	usart_Init:
 *	Funcion de configuracion del periferico USART2 a 19200 8N1
 *	Define los pines Tx (PA2) y Rx (PA3).
 */
static void usart_Init(void) {
	GPIO_InitTypeDef GPIO_Usart = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	/*
	 *  Asigna los pines PA2 y PA3 
	 *  a AF7 para enrutar a USART2.
	 */
	GPIO_Usart.Pin = GPIO_PIN_2 | GPIO_PIN_3;
	GPIO_Usart.Mode = GPIO_MODE_AF_PP;
	GPIO_Usart.Pull = GPIO_PULLUP;
	GPIO_Usart.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_Usart.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &GPIO_Usart);

	__HAL_RCC_USART2_CLK_ENABLE();

	/*
	 *  Configuracion de registros para USART2.
	 *  Se configura como UART para comunicacion asincrona.
	 *  Se habilitan ambos recepcion y transmision.
	 */
	huart2.Instance = USART2;
	huart2.Init.BaudRate = 19200;
	huart2.Init.WordLength = UART_WORDLENGTH_8B;
	huart2.Init.StopBits = UART_STOPBITS_1;
	huart2.Init.Parity = UART_PARITY_NONE;
	huart2.Init.Mode = UART_MODE_TX_RX;
	huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
	huart2.Init.OverSampling = UART_OVERSAMPLING_16;
	HAL_UART_Init(&huart2);

	HAL_NVIC_SetPriority(USART2_IRQn, 6, 0);
	HAL_NVIC_EnableIRQ(USART2_IRQn);

	/*
	 *  Recepcion asincrona de 1 byte en rx_data.
	 *  Esta funcion llama automaticamente la interrupcion al 
	 al detectar un caracter.
	 */

	HAL_UART_Receive_IT(&huart2, (uint8_t*) &rx_data, 1);

	// Mensaje de diagnostico e instruccion:
	char *msg =
			"STM32 listo. +/-: Regula LED rojo, a: apaga LED, b: Blanco. Encoder: verde. Pot: azul.\r\n";

	/*
	 * 	Transmision del mensaje de diagnostico (msg).
	 */
	HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
}

/* encoder_Init: TIM2 en modo encoder TI12 sobre PA0/PA1, sin interrupciones */
/*
 *  encoder_Init:
 *  Funcion de configuracion de TIM2 en modo encoder.
 *  
 */
static void encoder_Init(void) {
	GPIO_InitTypeDef GPIO_Enc = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	GPIO_Enc.Pin = GPIO_PIN_0 | GPIO_PIN_1;
	GPIO_Enc.Mode = GPIO_MODE_AF_PP;
	GPIO_Enc.Pull = GPIO_PULLUP;
	GPIO_Enc.Speed = GPIO_SPEED_FREQ_LOW;
	GPIO_Enc.Alternate = GPIO_AF1_TIM2;
	HAL_GPIO_Init(GPIOA, &GPIO_Enc);

	__HAL_RCC_TIM2_CLK_ENABLE();

	htim2.Instance = TIM2;
	htim2.Init.Prescaler = 0;
	htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim2.Init.Period = 0xFFFFFFFF;
	htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;

	TIM_Encoder_InitTypeDef sConfig = { 0 };
	sConfig.EncoderMode = TIM_ENCODERMODE_TI12; // Activa el modo encoder en TI1 y TI2.
	sConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
	sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;  // Enruta TI1 a PA0.
	sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
	sConfig.IC1Filter = 10;                       // Filtro para evitar debounce
	sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
	sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;  // Enruta TI2 a PA1.
	sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
	sConfig.IC2Filter = 10;                       // Filtro para evitar debounce
	HAL_TIM_Encoder_Init(&htim2, &sConfig);

	HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);

	/*
	 *	Se obtiene la cantidad de pulsos contabilizados;
	 En el momento de inicializar el MCU su valor es 0.
	 */
	enc_pos_prev = (int32_t) __HAL_TIM_GET_COUNTER(&htim2);
}

/*
 * trgo_tim_Init:
 * Funcion de configuracion de TIM4 que servirá como temporizador del ADC.
 * Una senal TRGO permite que un periferico dispare una accion en otro.
 */
static void trgo_tim_Init(void) {
	__HAL_RCC_TIM4_CLK_ENABLE();

	htim4.Instance = TIM4;
	htim4.Init.Prescaler = 15999;
	htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim4.Init.Period = 49;
	htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

	HAL_TIM_OC_Init(&htim4);

	TIM_OC_InitTypeDef sConfigOC = { 0 };
	//sConfigOC.OCMode = TIM_OCMODE_TIMING;
	sConfigOC.OCMode = TIM_OCMODE_PWM1;
	sConfigOC.Pulse = 25;
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
	HAL_TIM_OC_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_4);

	/*
	 * Inicializacion del timer.
	 * Como la funcion no tiene "IT" no hay interrupciones.
	 */
	HAL_TIM_OC_Start(&htim4, TIM_CHANNEL_4);
}

/*
 * adc_Init
 * Configura PC2 en modo analogico, ADC1 en resolucion 12 bits, disparado por
 * TRGO de TIM4 en flanco de subida, y habilita la interrupcion de fin de
 * conversion (EOC) para que la HAL llame al callback con el resultado.
 */
static void adc_Init(void) {
	GPIO_InitTypeDef GPIO_ADC = { 0 };

	/* Habilitar reloj de GPIOC (no se habia usado antes) */
	__HAL_RCC_GPIOC_CLK_ENABLE();

	// PC2 en modo analogico.
	GPIO_ADC.Pin = GPIO_PIN_2;
	GPIO_ADC.Mode = GPIO_MODE_ANALOG;
	GPIO_ADC.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOC, &GPIO_ADC);

	__HAL_RCC_ADC1_CLK_ENABLE();

	/* 
	 *  Configuracion del ADC:
	 */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;  // 8 MHz.
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;   // 12 bits de resolucion.
	hadc1.Init.ScanConvMode = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;   // Desactiva conversion continua.
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	//hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T4_TRGO; 
	//  TIM4 no se puede enrutar a TRGO, se usa CC4.
	hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T4_CC4; // La conversion la dispara el CC4 de TIM4.
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV; // Se levanta la flag para la interrupcion.
	hadc1.Init.DMAContinuousRequests = DISABLE;
	HAL_ADC_Init(&hadc1);

	/* 
	 *  Configuracion del pin PC2 en el canal ADC1_IN12.
	 */
	ADC_ChannelConfTypeDef sConfig = { 0 };
	sConfig.Channel = ADC_CHANNEL_12;       // PC2 = canal 12
	sConfig.Rank = 1;
	sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
	HAL_ADC_ConfigChannel(&hadc1, &sConfig);

	// Habilitar la interrupcion en el NVIC:
	HAL_NVIC_SetPriority(ADC_IRQn, 7, 0);
	HAL_NVIC_EnableIRQ(ADC_IRQn);

	/* 
	 *  Arrancar el ADC en modo interrupcion. 
	 *  No arranca conversiones todavia solo arma el ADC 
	 para que reaccione cuando llegue el TRGO. La primera
	 conversion ocurre cuando TIM4 hace su primer update.
	 */
	HAL_ADC_Start_IT(&hadc1);
}

/*
 *  ISR del ADC:
 *  Obtiene el resultado de la conversion en un valor entre 0 y 4095.
 *  Lo guarda en la variable adc_raw.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
	if (hadc->Instance == ADC1) {
		adc_raw = HAL_ADC_GetValue(hadc);  // lee el resultado (0..4095)
		adc_flag = 1;                       // avisa al bucle principal
	}
}

/*
 *  ISR de recepcion: 
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance == USART2) {
		rx_flag = 1;
		HAL_UART_Receive_IT(&huart2, (uint8_t*) &rx_data, 1);
	}
}

/*
 * ISR del blinky:
 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == TIM5) {
		HAL_GPIO_TogglePin(GPIOH, GPIO_PIN_1);
	}
}

/* 
 *  update_green: 
 *  Funcion para actualizar el valor 
 del duty cycle del LED verde.
 */
static void update_green(int16_t delta) {
	int32_t nuevo = (int32_t) duty_g + delta;
	if (nuevo > 1000)
		nuevo = 1000;
	if (nuevo < 0)
		nuevo = 0;
	duty_g = (uint16_t) nuevo;
	__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, duty_g);

	char msg[32];
	snprintf(msg, sizeof(msg), "Verde: %u\r\n", duty_g);
	HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
}

/* 
 *  process_encoder: 
 *  Funcion para detectar los flancos del encoder
 y ajustar el duty cycle del LED verde.
 */

static void process_encoder(void) {
	int32_t enc_pos_actual = (int32_t) __HAL_TIM_GET_COUNTER(&htim2);
	int32_t delta = enc_pos_actual - enc_pos_prev;

	if (delta != 0) {
		int32_t pasos = delta / 4;
		if (pasos != 0) {
			update_green((int16_t) (pasos * 50));
			enc_pos_prev += pasos * 4;
		}
	}
}

/*
 *  process_adc:
 *  Funcion que procesa el ADC
 *  para controlar el LED azul.
 */

static void process_adc(void) {

	// Conversion de 0..4095 a 0..1023
	uint16_t nuevo_duty_b = adc_raw / 4;
	if (nuevo_duty_b > 1000)
		nuevo_duty_b = 1000;

	// Se calcula la diferencia con el valor anterior
	static uint16_t last_reported = 0;
	int16_t diferencia = (int16_t) nuevo_duty_b - (int16_t) last_reported;
	if (diferencia < 0)
		diferencia = -diferencia;

	/*
	 * Si el cambio es muy pequeno no se transmite
	 * un mensaje de reporte por UART.
	 */

	if (diferencia >= 5) {
		duty_b = nuevo_duty_b;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, duty_b);

		char msg[40];
		snprintf(msg, sizeof(msg), "Azul: %u (ADC %u)\r\n", duty_b, adc_raw);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		last_reported = nuevo_duty_b;
	} else {
		duty_b = nuevo_duty_b;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, duty_b);
	}

	adc_flag = 0;
}

/* 
 *  process_rx: 
 *  Funcion para controlar el LED rojo 
 mediante comunicacion serial.
 */
static void process_rx(void) {
	char msg[48];

	switch (rx_data) {
	case '+':
		duty_r = (duty_r + 50 > 1000) ? 1000 : duty_r + 50;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, duty_r);
		snprintf(msg, sizeof(msg), "Rojo sube a %u\r\n", duty_r);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		break;

	case '-':
		duty_r = (duty_r < 50) ? 0 : duty_r - 50;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, duty_r);
		snprintf(msg, sizeof(msg), "Rojo baja a %u\r\n", duty_r);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		break;

	case 'a':
		duty_r = duty_g = duty_b = 0;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 0);
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 0);
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, 0);
		HAL_UART_Transmit(&huart2, (uint8_t*) "LED apagado\r\n", 13, 100);
		break;

	case 'b':
		duty_r = duty_g = duty_b = 1000;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, 1000);
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_2, 1000);
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, 1000);
		HAL_UART_Transmit(&huart2, (uint8_t*) "Blanco maximo\r\n", 15, 100);
		break;

	default:
		snprintf(msg, sizeof(msg), "Desconocido: 0x%02X\r\n", rx_data);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		break;
	}

	rx_flag = 0;
}

int main(void) {

	HAL_Init();   // Inicializacion de las librerias HAL
	clock_Init();
	pwm_Init();
	blinky_Init();
	usart_Init();
	encoder_Init();
	trgo_tim_Init();
	adc_Init();

	/*
	 *  Bucle principal:
	 *  Revisa si los dos sistemas que utilizan interrupciones
	 *  tienen su bandera levantada (Serial, ADC).
	 *  Revisa si el encoder registro algun cambio.
	 */

	/*
	 * Bucle principal implementado como maquina de estados finitos.
	 *
	 * En ST_IDLE se verifican las fuentes de eventos en orden de prioridad:
	 *   1. Serial (comandos explicitos del usuario, respuesta inmediata).
	 *   2. ADC (muestreo periodico programado por hardware).
	 *   3. Encoder (verificacion pasiva del contador).
	 * La primera fuente activa determina el proximo estado. Solo se atiende
	 * una fuente por ciclo de la FSM, garantizando que todas eventualmente
	 * reciben atencion equitativa sin bloqueos.
	 *
	 * En los estados de procesamiento se ejecuta la accion asociada y se
	 * retorna a ST_IDLE. Ninguna funcion de procesamiento debe hacer polling
	 * bloqueante ni esperas largas, para no comprometer la respuesta de las
	 * demas fuentes.
	 */
	while (1) {

		switch (fsm_state) {

		case ST_IDLE:
			/* Prioridad 1: comando serial */
			if (rx_flag) {
				fsm_state = ST_PROC_SERIAL;
			}
			/* Prioridad 2: conversion ADC */
			else if (adc_flag) {
				fsm_state = ST_PROC_ADC;
			}
			/* Prioridad 3: cambio en el contador del encoder. */
			else {
				int32_t enc_actual = (int32_t) __HAL_TIM_GET_COUNTER(&htim2);
				if (enc_actual != enc_pos_prev) {
					fsm_state = ST_PROC_ENCODER;
				}
			}
			break;

		case ST_PROC_SERIAL:
			process_rx();
			fsm_state = ST_IDLE;
			break;

		case ST_PROC_ADC:
			process_adc();
			fsm_state = ST_IDLE;
			break;

		case ST_PROC_ENCODER:
			process_encoder();
			fsm_state = ST_IDLE;
			break;

		default:
			fsm_state = ST_IDLE;
			break;
		}
	}
}
