/*
 * main_tarea3.c
 *
 * Task 3 - Taller V
 * PWM RGB + Blinky + USART2 + Encoder TIM2 + ADC disparado por TRGO de TIM4
 * Author: daniel68
 */

#include "stm32f4xx_hal.h"
#include <string.h>
#include <stdio.h>

/* Manejadores globales */
TIM_HandleTypeDef htim2 = { 0 };    // Encoder (32 bits)
TIM_HandleTypeDef htim3 = { 0 };    // PWM RGB
TIM_HandleTypeDef htim4 = { 0 };    // TRGO (Trigger Output) para disparar ADC cada 50 ms
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
	TIM_OC_InitTypeDef sConfigOC = {0};
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
 * ------------++++++++++++++++++++++********************
 * ccccccccccccggggggHHHHHHHHHHHH7777777777777
 */



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
	HAL_TIM_Base_Init(&htim5);

	HAL_NVIC_SetPriority(TIM5_IRQn, 5, 0);
	HAL_NVIC_EnableIRQ(TIM5_IRQn);

	HAL_TIM_Base_Start_IT(&htim5);
}

/* usart_Init: USART2 a 19200 8N1, Rx por interrupcion */
static void usart_Init(void) {
	GPIO_InitTypeDef GPIO_Usart = { 0 };

	__HAL_RCC_GPIOA_CLK_ENABLE();

	GPIO_Usart.Pin = GPIO_PIN_2 | GPIO_PIN_3;
	GPIO_Usart.Mode = GPIO_MODE_AF_PP;
	GPIO_Usart.Pull = GPIO_PULLUP;
	GPIO_Usart.Speed = GPIO_SPEED_FREQ_HIGH;
	GPIO_Usart.Alternate = GPIO_AF7_USART2;
	HAL_GPIO_Init(GPIOA, &GPIO_Usart);

	__HAL_RCC_USART2_CLK_ENABLE();

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

	HAL_UART_Receive_IT(&huart2, (uint8_t*) &rx_data, 1);

	char *msg =
			"STM32 listo. r/R: rojo, a: apaga, b: blanco. Encoder: verde. Pot: azul.\r\n";
	HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
}

/* encoder_Init: TIM2 en modo encoder TI12 sobre PA0/PA1, sin interrupciones */
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
	sConfig.EncoderMode = TIM_ENCODERMODE_TI12;
	sConfig.IC1Polarity = TIM_ICPOLARITY_RISING;
	sConfig.IC1Selection = TIM_ICSELECTION_DIRECTTI;
	sConfig.IC1Prescaler = TIM_ICPSC_DIV1;
	sConfig.IC1Filter = 10;
	sConfig.IC2Polarity = TIM_ICPOLARITY_RISING;
	sConfig.IC2Selection = TIM_ICSELECTION_DIRECTTI;
	sConfig.IC2Prescaler = TIM_ICPSC_DIV1;
	sConfig.IC2Filter = 10;
	HAL_TIM_Encoder_Init(&htim2, &sConfig);

	HAL_TIM_Encoder_Start(&htim2, TIM_CHANNEL_ALL);

	enc_pos_prev = (int32_t) __HAL_TIM_GET_COUNTER(&htim2);
}

/*
 * trgo_tim_Init
 * Configura TIM4 con su canal 4 en output compare timing mode, generando un
 * evento CC4 cada 50 ms. Ese evento dispara el ADC por hardware (sin tocar
 * NVIC, sin interrupcion del timer).
 *
 * Originalmente queriamos usar TRGO del update event, pero el ADC del F411
 * no acepta T4_TRGO como fuente. Acepta T4_CC4 que produce el mismo resultado.
 *
 * tick = 16 MHz / (15999+1) = 1000 Hz -> 1 ms por tick
 * periodo = (49+1) * 1 ms = 50 ms -> evento CC4 cada 50 ms
 */
static void trgo_tim_Init(void) {
	__HAL_RCC_TIM4_CLK_ENABLE();

	/* Base de tiempo */
	htim4.Instance = TIM4;
	htim4.Init.Prescaler = 15999;
	htim4.Init.CounterMode = TIM_COUNTERMODE_UP;
	htim4.Init.Period = 49;
	htim4.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
	htim4.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;

	/* Inicializa el timer en modo Output Compare (no es PWM, es OC).
	 Esta funcion configura los registros base; los canales se configuran
	 individualmente abajo. */
	HAL_TIM_OC_Init(&htim4);

	/* Configurar canal 4 en modo TIMING (output frozen).
	 OCMode = TIM_OCMODE_TIMING significa que el comparador funciona y
	 genera el evento CC4 cada vez que CNT iguala CCR4, pero el pin
	 fisico de salida (PB9 para TIM4_CH4) no se modifica. Es justo lo
	 que queremos: el evento interno fluye al ADC pero no consumimos
	 ni configuramos ningun pin.

	 Pulse = 25: valor de CCR4. Como CNT cuenta de 0 a 49 (50 ticks),
	 cualquier valor entre 0 y 49 produce un evento CC4 por periodo.
	 Elegimos 25 (mitad del periodo); el valor exacto no importa porque
	 solo nos interesa la frecuencia de los eventos, no su fase. */
	TIM_OC_InitTypeDef sConfigOC = { 0 };
	//sConfigOC.OCMode = TIM_OCMODE_TIMING;
	sConfigOC.OCMode = TIM_OCMODE_PWM1;   // antes: TIM_OCMODE_TIMING
	sConfigOC.Pulse = 25;
	sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
	sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
	HAL_TIM_OC_ConfigChannel(&htim4, &sConfigOC, TIM_CHANNEL_4);

	/* Arranca el canal 4 en output compare sin interrupcion.
	 La funcion habilita la generacion del evento CC4 pero NO toca el NVIC. */
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

	/* PC2 en modo analogico: desconecta los buffers digitales, deja el pin
	 conectado al multiplexor interno del ADC. No tiene AF, no tiene pull. */
	GPIO_ADC.Pin = GPIO_PIN_2;
	GPIO_ADC.Mode = GPIO_MODE_ANALOG;
	GPIO_ADC.Pull = GPIO_NOPULL;
	HAL_GPIO_Init(GPIOC, &GPIO_ADC);

	/* Habilitar reloj del ADC1 (en bus APB2) */
	__HAL_RCC_ADC1_CLK_ENABLE();

	/* Configuracion del ADC.
	 ClockPrescaler DIV2: ADC corre a APB2/2 = 8 MHz, bien dentro del rango
	 maximo de 36 MHz que soporta el ADC del F411.
	 Resolution 12B: 12 bits, requerido por la tarea.
	 ScanConvMode DISABLE: solo convertimos un canal, no necesitamos scan.
	 ContinuousConvMode DISABLE: no queremos que el ADC convierta
	 continuamente; cada conversion la dispara TRGO.
	 ExternalTrigConv T4_TRGO: la fuente del disparo es TRGO de TIM4.
	 ExternalTrigConvEdge RISING: dispara en flanco de subida del TRGO.
	 EOCSelection EOC_SINGLE_CONV: el flag EOC se levanta al final de cada
	 conversion (lo necesitamos para que la HAL llame al callback).
	 DMAContinuousRequests DISABLE: no usamos DMA en esta tarea. */
	hadc1.Instance = ADC1;
	hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV2;
	hadc1.Init.Resolution = ADC_RESOLUTION_12B;
	hadc1.Init.ScanConvMode = DISABLE;
	hadc1.Init.ContinuousConvMode = DISABLE;
	hadc1.Init.DiscontinuousConvMode = DISABLE;
	//hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T4_TRGO;
	hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T4_CC4; // antes: T4_TRGO
	hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
	hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
	hadc1.Init.NbrOfConversion = 1;
	hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
	hadc1.Init.DMAContinuousRequests = DISABLE;
	HAL_ADC_Init(&hadc1);

	/* Configurar el canal que se va a convertir.
	 PC2 corresponde a ADC1_IN12. SamplingTime 56 ciclos es buen balance
	 entre velocidad y precision para una fuente como un potenciometro. */
	ADC_ChannelConfTypeDef sConfig = { 0 };
	sConfig.Channel = ADC_CHANNEL_12;       // PC2 = canal 12
	sConfig.Rank = 1;                       // primer (y unico) canal del scan
	sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
	HAL_ADC_ConfigChannel(&hadc1, &sConfig);

	/* Habilitar interrupcion EOC del ADC en el NVIC.
	 El ADC comparte una sola linea de interrupcion para todas sus fuentes. */
	HAL_NVIC_SetPriority(ADC_IRQn, 7, 0);   // prioridad mas baja, no critica
	HAL_NVIC_EnableIRQ(ADC_IRQn);

	/* Arrancar el ADC en modo interrupcion. NO arranca conversiones todavia:
	 solo arma el ADC para que reaccione cuando llegue el TRGO. La primera
	 conversion ocurre cuando TIM4 hace su primer update. */
	HAL_ADC_Start_IT(&hadc1);
}

/*
 * HAL_ADC_ConvCpltCallback
 * Llamado automaticamente cuando termina una conversion. Recupera el resultado
 * de 12 bits y levanta una bandera para que el bucle principal lo procese.
 * Mantener la ISR corta es buena practica.
 *
 * IMPORTANTE: el nombre debe ser EXACTAMENTE este. Tu codigo original tenia
 * "HA_ADC_..." (sin la L), por eso nunca se ejecutaba.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
	if (hadc->Instance == ADC1) {
		adc_raw = HAL_ADC_GetValue(hadc);  // lee el resultado (0..4095)
		adc_flag = 1;                       // avisa al bucle principal
	}
}

/* HAL_UART_RxCpltCallback */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
	if (huart->Instance == USART2) {
		rx_flag = 1;
		HAL_UART_Receive_IT(&huart2, (uint8_t*) &rx_data, 1);
	}
}

/* HAL_TIM_PeriodElapsedCallback: blinky en PH1 */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim) {
	if (htim->Instance == TIM5) {
		HAL_GPIO_TogglePin(GPIOH, GPIO_PIN_1);
	}
}

/* update_green: igual que antes */
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

/* process_encoder: igual que antes */
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
 * process_adc
 * Toma el ultimo valor convertido del ADC y lo escala al rango del duty
 * del PWM. Aplica al canal azul (TIM3_CH4, PB1). Solo reporta por USART
 * cuando el duty cambia, para no inundar la terminal con valores casi
 * iguales por ruido del ADC.
 */
static void process_adc(void) {
	/* Conversion de 0..4095 a 0..1000 con divisor entero.
	 4095 / 4 = 1023, cap en 1000. La perdida de resolucion es minima
	 y simplifica el calculo (no multiplicacion ni division costosa). */
	uint16_t nuevo_duty_b = adc_raw / 4;
	if (nuevo_duty_b > 1000)
		nuevo_duty_b = 1000;

	/* Solo actualizar y reportar si hubo cambio significativo.
	 El ADC sin filtrado tiene un par de bits de ruido, asi que pequenos
	 saltos son normales y no queremos enviar mensajes por cada uno.
	 Histeresis de +/- 5 unidades evita el spam. */
	static uint16_t last_reported = 0;
	int16_t diferencia = (int16_t) nuevo_duty_b - (int16_t) last_reported;
	if (diferencia < 0)
		diferencia = -diferencia;

	if (diferencia >= 5) {
		duty_b = nuevo_duty_b;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, duty_b);

		char msg[40];
		snprintf(msg, sizeof(msg), "Azul: %u (ADC %u)\r\n", duty_b, adc_raw);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		last_reported = nuevo_duty_b;
	} else {
		/* Aunque no reportemos, si actualizamos el duty para que el LED
		 responda suavemente. Esto sigue siendo cada 50 ms, no es spam. */
		duty_b = nuevo_duty_b;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_4, duty_b);
	}

	adc_flag = 0;
}

/* process_rx: igual que antes */
static void process_rx(void) {
	char msg[48];

	switch (rx_data) {
	case 'r':
		duty_r = (duty_r + 50 > 1000) ? 1000 : duty_r + 50;
		__HAL_TIM_SET_COMPARE(&htim3, TIM_CHANNEL_1, duty_r);
		snprintf(msg, sizeof(msg), "Rojo sube a %u\r\n", duty_r);
		HAL_UART_Transmit(&huart2, (uint8_t*) msg, strlen(msg), 100);
		break;

	case 'R':
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

	HAL_Init();
	clock_Init();
	pwm_Init();
	blinky_Init();
	usart_Init();
	encoder_Init();
	trgo_tim_Init();   // TIM4 generando TRGO cada 50 ms
	adc_Init();        // ADC armado, espera el TRGO

	/* DIAGNOSTICO TEMPORAL EXTENDIDO */
	HAL_Delay(100);

	HAL_ADC_Stop_IT(&hadc1);

	/* Verificar estado real del ADC antes de intentar conversion */
	char dbg[80];
	snprintf(dbg, sizeof(dbg), "ADC CR2=0x%08lX SR=0x%08lX\r\n",
			(unsigned long) hadc1.Instance->CR2,
			(unsigned long) hadc1.Instance->SR);
	HAL_UART_Transmit(&huart2, (uint8_t*) dbg, strlen(dbg), 100);

	/* Forzar el ADC encendido (bit ADON) si la HAL lo apago al detener */
	hadc1.Instance->CR2 |= ADC_CR2_ADON;
	HAL_Delay(1);  // breve estabilizacion del ADC

	/* Limpiar el flag EOC y disparar conversion por software */
	hadc1.Instance->SR &= ~ADC_SR_EOC;
	hadc1.Instance->CR2 |= ADC_CR2_SWSTART;

	uint32_t timeout = HAL_GetTick() + 100;
	while (!(hadc1.Instance->SR & ADC_SR_EOC) && HAL_GetTick() < timeout) {
	}

	if (hadc1.Instance->SR & ADC_SR_EOC) {
		uint16_t valor = (uint16_t) (hadc1.Instance->DR & 0x0FFF);
		snprintf(dbg, sizeof(dbg), "Diag ADC OK: %u\r\n", valor);
	} else {
		snprintf(dbg, sizeof(dbg), "Diag TIMEOUT. CR2=0x%08lX SR=0x%08lX\r\n",
				(unsigned long) hadc1.Instance->CR2,
				(unsigned long) hadc1.Instance->SR);
	}
	HAL_UART_Transmit(&huart2, (uint8_t*) dbg, strlen(dbg), 100);

	HAL_ADC_Start_IT(&hadc1);
	/* Bucle principal: tres tareas se atienden por polling de banderas y
	 lectura directa de hardware. Todo lo critico en timing se hace por
	 interrupcion o por hardware puro (TRGO -> ADC). */
	while (1) {


		if (rx_flag)
			process_rx();
		if (adc_flag)
			process_adc();
		process_encoder();
	}
}
