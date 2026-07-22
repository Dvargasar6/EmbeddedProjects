/*
 * adc_joystick.c
 * Adquisicion de los dos ejes del joystick con ADC1 en modo interrupcion.
 *
 * Estrategia: el ADC del STM32F4 dispone de un unico registro de datos. Para
 * leer dos canales sin DMA se convierte de uno en uno y se conmuta el canal
 * dentro del callback de fin de conversion:
 *
 *   TIM4 (100 ms) --> adc_joystick_start() --> conversion canal IN0
 *          callback --> guarda VRX, selecciona IN1, arranca de nuevo
 *          callback --> guarda VRY, secuencia terminada
 *
 * De este modo no hay ninguna espera activa: el nucleo solo interviene
 * durante los pocos microsegundos de cada interrupcion.
 */
#include "adc_joystick.h"

ADC_HandleTypeDef hadc1;

/* Indice del canal en curso: 0 = VRX (IN0), 1 = VRY (IN1). volatile porque
   se modifica dentro de una interrupcion y se lee fuera de ella. */
static volatile uint8_t  s_channel = 0;

/* Ultimas muestras validas de cada eje */
static volatile uint16_t s_raw_x = JOY_CENTER;
static volatile uint16_t s_raw_y = JOY_CENTER;

/*
 * adc_select_channel
 * Programa el canal que ocupara el primer puesto de la secuencia de conversion.
 * Funcion interna (static): no forma parte de la interfaz del modulo.
 */
static void adc_select_channel(uint8_t idx)
{
    ADC_ChannelConfTypeDef ch = {0};

    ch.Channel      = (idx == 0) ? ADC_CHANNEL_0 : ADC_CHANNEL_1;
    ch.Rank         = 1;   /* primera y unica posicion de la secuencia */
    ch.SamplingTime = ADC_SAMPLETIME_84CYCLES;
    /* Tiempo de muestreo: 84 ciclos de ADCCLK. Debe ser suficiente para que el
       condensador de muestreo se cargue a traves de la impedancia de la fuente.
       El potenciometro del joystick ronda los 10 kohm, valor alto, por lo que
       se toma un tiempo generoso. Alternativas: 3, 15, 28, 56, 112, 144, 480
       ciclos. Un tiempo demasiado corto produce lecturas bajas y ruidosas. */

    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) {
        Error_Handler();
    }
}

void adc_joystick_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();   /* reloj del ADC1 (bus APB2) */

    hadc1.Instance = ADC1;

    /* ADCCLK = PCLK2 / 4 = 100 MHz / 4 = 25 MHz.
       El maximo admitido por el ADC del F411 es 36 MHz; DIV2 daria 50 MHz y
       violaria la especificacion. Alternativas: DIV2, DIV4, DIV6, DIV8. */
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;

    hadc1.Init.Resolution = ADC_RESOLUTION_12B;  /* 0..4095. Alternativas:
                                        10B, 8B, 6B; menor resolucion implica
                                        conversiones mas rapidas */
    hadc1.Init.ScanConvMode          = DISABLE;  /* un solo canal por secuencia */
    hadc1.Init.ContinuousConvMode    = DISABLE;  /* conversion unica bajo demanda */
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
                                        /* el disparo lo da el software desde
                                           la ISR de TIM4. Alternativa: disparo
                                           directo por TRGO de un temporizador */
    hadc1.Init.ExternalTrigConvEdge  = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
                                        /* resultado justificado a la derecha:
                                           el valor se lee directamente sin
                                           desplazamientos */
    hadc1.Init.NbrOfConversion       = 1;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    hadc1.Init.EOCSelection          = ADC_EOC_SINGLE_CONV;
                                        /* la bandera EOC se activa al terminar
                                           cada conversion individual */

    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    adc_select_channel(0);   /* deja seleccionado VRX para la primera lectura */

    /* Prioridad 6: por debajo de la UART, por encima de los temporizadores */
    HAL_NVIC_SetPriority(ADC_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);
}

/*
 * adc_joystick_start
 * Inicia la secuencia de dos conversiones. Se invoca desde la ISR de TIM4,
 * por lo que debe retornar de inmediato.
 */
void adc_joystick_start(void)
{
    s_channel = 0;              /* la secuencia empieza siempre por VRX */
    adc_select_channel(0);
    HAL_ADC_Start_IT(&hadc1);   /* arranca la conversion con interrupcion EOC */
}

/*
 * HAL_ADC_ConvCpltCallback
 * La HAL la invoca desde HAL_ADC_IRQHandler al activarse la bandera EOC.
 * Encadena la segunda conversion sin bloquear el nucleo.
 */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) {
        return;   /* proteccion por si se anadiese otro ADC al proyecto */
    }

    if (s_channel == 0) {
        s_raw_x   = (uint16_t)HAL_ADC_GetValue(hadc); /* lee y limpia EOC */
        s_channel = 1;
        adc_select_channel(1);        /* conmuta a VRY */
        HAL_ADC_Start_IT(hadc);       /* segunda conversion */
    } else {
        s_raw_y   = (uint16_t)HAL_ADC_GetValue(hadc);
        s_channel = 0;                /* secuencia completa */
    }
}

/* Accesores de las ultimas muestras crudas */
uint16_t adc_joystick_raw_x(void) { return s_raw_x; }
uint16_t adc_joystick_raw_y(void) { return s_raw_y; }

/*
 * adc_dir
 * Convierte una muestra cruda en una direccion discreta aplicando la banda
 * muerta. Devuelve -1, 0 o +1.
 */
static int8_t adc_dir(uint16_t raw)
{
    if (raw > (JOY_CENTER + JOY_DEADZONE)) return  1;
    if (raw < (JOY_CENTER - JOY_DEADZONE)) return -1;
    return 0;
}

int8_t adc_joystick_dir_x(void) { return adc_dir(s_raw_x); }
int8_t adc_joystick_dir_y(void) { return adc_dir(s_raw_y); }
