/*
 * rtc.c
 * Configuracion del RTC con cristal LSE de 32.768 kHz y despertador de 1 s.
 *
 * Cadena de division del RTC:
 *   f_ck_spre = LSE / ((AsynchPrediv + 1) * (SynchPrediv + 1))
 *             = 32768 / (128 * 256) = 1 Hz
 *
 * Se prefiere un preescalado asincrono grande (127) porque esa rama es la que
 * mas contribuye al ahorro de consumo del contador.
 */
#include "rtc.h"
#include "system_clock.h"
#include "uart.h"

RTC_HandleTypeDef hrtc;

/* Bandera puesta a 1 por la interrupcion de despertador cada segundo.
   La consume la maquina de estados para refrescar la linea 1 del LCD. */
volatile uint8_t g_rtc_second_flag = 0;

/* ------------------------------------------------------------------------
 * Conversion de las macros __DATE__ y __TIME__ del compilador.
 * Permite que, la primera vez que se programa la placa, el RTC arranque con
 * la fecha y hora reales del momento de compilacion en lugar de un valor fijo.
 * __DATE__ tiene el formato "Jul 21 2026"; __TIME__ el formato "14:35:07".
 * ---------------------------------------------------------------------- */
static uint8_t build_month(void)
{
    /* Tabla de las tres primeras letras de cada mes en ingles */
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;
    uint8_t i;

    for (i = 0; i < 12U; i++) {
        if (d[0] == months[i * 3] &&
            d[1] == months[i * 3 + 1] &&
            d[2] == months[i * 3 + 2]) {
            return (uint8_t)(i + 1U);   /* los meses del RTC van de 1 a 12 */
        }
    }
    return 1U;
}

static uint8_t build_day(void)
{
    const char *d = __DATE__;
    /* La posicion 4 es un espacio para los dias de un digito */
    uint8_t tens = (d[4] == ' ') ? 0U : (uint8_t)(d[4] - '0');
    return (uint8_t)(tens * 10U + (uint8_t)(d[5] - '0'));
}

static uint8_t build_year(void)
{
    const char *d = __DATE__;
    /* El RTC almacena solo los dos ultimos digitos del ano */
    return (uint8_t)((d[9] - '0') * 10 + (d[10] - '0'));
}

static uint8_t build_field(uint8_t offset)
{
    const char *t = __TIME__;   /* "HH:MM:SS" */
    return (uint8_t)((t[offset] - '0') * 10 + (t[offset + 1] - '0'));
}

void rtc_init(void)
{
    RTC_TimeTypeDef       time = {0};
    RTC_DateTypeDef       date = {0};
    RCC_PeriphCLKInitTypeDef pclk = {0};
    uint32_t src_wanted;      /* fuente de reloj elegida para el RTC   */
    uint32_t prediv_a;        /* prescalador asincrono                 */
    uint32_t prediv_s;        /* prescalador sincrono                  */

    /* --- 1. Seleccion de la fuente de reloj del RTC ---------------------- */
    /* Si el cristal LSE arranco se usa el, que es lo que exige el enunciado.
       Si no arranco (cristal X2 ausente en la placa) se recurre al LSI para
       que el resto del sistema siga siendo utilizable, advirtiendolo por la
       consola. El LSI es un oscilador RC interno con una deriva de hasta
       +-50 % y, sobre todo, SE DETIENE al perder VDD: con el, la hora no
       sobrevive al corte de alimentacion. */
    if (system_clock_lse_ok()) {
        src_wanted = RCC_RTCCLKSOURCE_LSE;
        prediv_a   = 127U;   /* 32768 / 128 = 256 Hz */
        prediv_s   = 255U;   /* 256   / 256 = 1 Hz   */
    } else {
        __HAL_RCC_LSI_ENABLE();
        while (__HAL_RCC_GET_FLAG(RCC_FLAG_LSIRDY) == RESET) { }

        src_wanted = RCC_RTCCLKSOURCE_LSI;
        prediv_a   = 127U;   /* LSI nominal 32 kHz: 32000 / 128 = 250 Hz */
        prediv_s   = 249U;   /* 250 / 250 = 1 Hz (aproximado)            */

        uart_send_line("AVISO: el LSE no arranco. RTC funcionando con LSI.");
        uart_send_line("       Revisar el cristal X2 y los puentes SB48/SB49.");
    }

    /* Cambiar RTCSEL obliga a un reset del dominio de respaldo, lo que
       borraria la hora. Por eso solo se configura si la fuente actual no es
       ya la deseada, es decir, unicamente en el primer arranque tras montar
       la pila o tras un borrado completo. */
    if (__HAL_RCC_GET_RTC_SOURCE() != src_wanted) {
        pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
        pclk.RTCClockSelection    = src_wanted;
        if (HAL_RCCEx_PeriphCLKConfig(&pclk) != HAL_OK) {
            Error_Handler();
        }
    }

    __HAL_RCC_RTC_ENABLE();   /* habilita el reloj del bloque RTC */

    /* --- 2. Parametros del contador ------------------------------------- */
    hrtc.Instance            = RTC;
    hrtc.Init.HourFormat     = RTC_HOURFORMAT_24;  /* alternativa: _12 con AM/PM */
    hrtc.Init.AsynchPrediv   = prediv_a;           /* calculado mas arriba  */
    hrtc.Init.SynchPrediv    = prediv_s;           /* segun la fuente activa */
    hrtc.Init.OutPut         = RTC_OUTPUT_DISABLE; /* sin salida por PC13   */
    hrtc.Init.OutPutPolarity = RTC_OUTPUT_POLARITY_HIGH;
    hrtc.Init.OutPutType     = RTC_OUTPUT_TYPE_OPENDRAIN;

    if (HAL_RTC_Init(&hrtc) != HAL_OK) {
        Error_Handler();
    }

    /* --- 3. Carga de fecha y hora solo en el primer arranque -------------- */
    if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_BACKUP_SIGNATURE) {

        time.Hours   = build_field(0);   /* posiciones 0-1 de __TIME__ */
        time.Minutes = build_field(3);   /* posiciones 3-4             */
        time.Seconds = build_field(6);   /* posiciones 6-7             */
        time.TimeFormat     = RTC_HOURFORMAT12_AM;  /* ignorado en modo 24 h */
        time.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        time.StoreOperation = RTC_STOREOPERATION_RESET;

        if (HAL_RTC_SetTime(&hrtc, &time, RTC_FORMAT_BIN) != HAL_OK) {
            Error_Handler();
        }
        /* RTC_FORMAT_BIN acepta valores decimales normales; la alternativa
           RTC_FORMAT_BCD exigiria codificar cada digito en un nibble. */

        date.WeekDay = RTC_WEEKDAY_MONDAY;  /* no se muestra en el LCD */
        date.Month   = build_month();
        date.Date    = build_day();
        date.Year    = build_year();

        if (HAL_RTC_SetDate(&hrtc, &date, RTC_FORMAT_BIN) != HAL_OK) {
            Error_Handler();
        }

        /* Deja constancia de que el RTC ya esta puesto en hora */
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BACKUP_SIGNATURE);
    }

    /* --- 4. Despertador periodico de 1 s --------------------------------- */
    /* Se desactiva primero por si quedo activo de una ejecucion anterior:
       el dominio de respaldo no se reinicia con el reset del nucleo. */
    HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);

    HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 9, 0);
    HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);

    /* Contador 0 con reloj ck_spre (1 Hz) produce un evento cada segundo.
       Alternativa: RTC_WAKEUPCLOCK_RTCCLK_DIV16, que permite periodos por
       debajo del segundo a costa de un contador mas complejo. */
    if (HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0U,
                                    RTC_WAKEUPCLOCK_CK_SPRE_16BITS) != HAL_OK) {
        Error_Handler();
    }
}

/*
 * rtc_get_time
 * Lee hora y fecha. HAL_RTC_GetTime bloquea los registros sombra hasta que se
 * lee la fecha, de modo que ambas llamadas deben hacerse siempre en pareja y
 * en este orden; de lo contrario el contador deja de actualizar la lectura.
 */
void rtc_get_time(RTC_TimeTypeDef *t, RTC_DateTypeDef *d)
{
    HAL_RTC_GetTime(&hrtc, t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, d, RTC_FORMAT_BIN);
}

/*
 * rtc_format_line
 * Compone la cadena "HH:MM:SS DD-MM" en el buffer indicado.
 */
void rtc_format_line(char *dst, uint8_t len)
{
    RTC_TimeTypeDef t;
    RTC_DateTypeDef d;

    rtc_get_time(&t, &d);

    snprintf(dst, len, "%02u:%02u:%02u %02u-%02u",
             (unsigned)t.Hours, (unsigned)t.Minutes, (unsigned)t.Seconds,
             (unsigned)d.Date,  (unsigned)d.Month);
}

/*
 * HAL_RTCEx_WakeUpTimerEventCallback
 * Invocada cada segundo desde HAL_RTCEx_WakeUpTimerIRQHandler.
 * Solo levanta una bandera: el refresco del LCD usa I2C y no puede ejecutarse
 * en contexto de interrupcion.
 */
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *h)
{
    (void)h;                    /* parametro no usado: evita un aviso */
    g_rtc_second_flag = 1U;
}
