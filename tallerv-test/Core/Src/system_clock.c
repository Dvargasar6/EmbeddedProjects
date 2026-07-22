/*
 * system_clock.c
 * Configuracion del arbol de relojes del STM32F411RE y de la salida MCO1.
 *
 * Cadena de reloj implementada:
 *
 *   HSI (16 MHz) --> /PLLM=8 --> 2 MHz --> xPLLN=100 --> VCO 200 MHz
 *                                                  --> /PLLP=2 --> 100 MHz = SYSCLK
 *
 *   AHB  = SYSCLK / 1 = 100 MHz   (limite del F411: 100 MHz)
 *   APB1 = SYSCLK / 2 =  50 MHz   (limite del F411:  50 MHz)
 *   APB2 = SYSCLK / 1 = 100 MHz   (limite del F411: 100 MHz)
 *
 * Se elige HSI y no HSE como fuente del PLL por dos razones:
 *   1. El LED exigido esta en PH1, que es el pin OSC_OUT del HSE. Usando HSI
 *      el puerto GPIOH queda completamente libre.
 *   2. En la Nucleo-F411RE el HSE proviene del MCO del ST-LINK (8 MHz) y
 *      depende de la configuracion de los puentes SB54/SB55.
 */
#include "system_clock.h"

/* Fuente actualmente enrutada a MCO1. Estatica: solo visible en este archivo. */
static mco_src_t s_mco_src = MCO_SRC_PLL;

/* Frecuencia real medida en PA8 tras aplicar el prescaler, en hercios. */
static uint32_t  s_mco_freq = 20000000U;

/* 1 si el cristal LSE arranco correctamente; 0 si no esta montado o no oscila */
static uint8_t   s_lse_ok = 0;

/*
 * system_clock_init
 * Configura la alimentacion, la latencia de FLASH, los osciladores y los buses.
 * Debe llamarse inmediatamente despues de HAL_Init() y antes de cualquier
 * inicializacion de periferico.
 */
void system_clock_init(void)
{
    RCC_OscInitTypeDef       osc = {0};   /* configuracion de osciladores */
    RCC_ClkInitTypeDef       clk = {0};   /* configuracion de buses       */

    /* --- 1. Alimentacion del regulador interno -------------------------- */
    /* Habilita el reloj del bloque PWR: sin el no se puede escribir en PWR->CR */
    __HAL_RCC_PWR_CLK_ENABLE();

    /* Escala de voltaje 1: unica que admite 100 MHz en el F411.
       Alternativas: SCALE2 (hasta 84 MHz) y SCALE3 (hasta 64 MHz), que
       consumen menos corriente pero limitan la frecuencia maxima. */
    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    /* Permite escribir en el dominio de respaldo (Backup Domain), necesario
       para arrancar el LSE y para configurar el RTC. */
    HAL_PWR_EnableBkUpAccess();

    /* --- 2. Oscilador HSI y PLL (critico) ------------------------------- */
    /* El LSE se configura APARTE, mas abajo. Motivo: muchas placas
       NUCLEO-F411RE se sirven sin el cristal X2 de 32.768 kHz montado. Si el
       LSE se configurase en la misma llamada, su fallo abortaria tambien la
       configuracion del PLL y el sistema entero quedaria detenido en
       Error_Handler, sin LCD, sin UART y sin LED de 250 ms. */
    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;              /* enciende el HSI     */
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT; /* trim de fabrica  */

    osc.PLL.PLLState        = RCC_PLL_ON;              /* activa el PLL       */
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;       /* entrada del PLL     */
    osc.PLL.PLLM            = 8U;    /* divisor de entrada: 16 MHz / 8 = 2 MHz.
                                        La entrada al VCO debe quedar entre
                                        1 y 2 MHz; 2 MHz minimiza el jitter */
    osc.PLL.PLLN            = 100U;  /* multiplicador: 2 MHz * 100 = 200 MHz.
                                        El VCO debe quedar entre 100 y 432 MHz */
    osc.PLL.PLLP            = RCC_PLLP_DIV2; /* 200 MHz / 2 = 100 MHz = SYSCLK.
                                        Valores posibles: DIV2, DIV4, DIV6, DIV8 */
    osc.PLL.PLLQ            = 4U;    /* rama para USB/SDIO: 200 MHz / 4 = 50 MHz.
                                        No se usa en este proyecto, pero la HAL
                                        exige un valor valido (2..15) */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();             /* fallo del PLL: este si es irrecuperable */
    }

    /* --- 3. Buses del sistema ------------------------------------------- */
    /* Se indican los cuatro relojes que se van a modificar */
    clk.ClockType      = RCC_CLOCKTYPE_HCLK   | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1  | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK; /* el PLL alimenta el sistema */
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;         /* HCLK  = 100 MHz            */
    clk.APB1CLKDivider = RCC_HCLK_DIV2;           /* PCLK1 =  50 MHz (maximo)   */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;           /* PCLK2 = 100 MHz            */

    /* FLASH_LATENCY_3 = 3 estados de espera.
       A 3.3 V la FLASH admite 30 MHz por estado de espera:
         0 WS <= 30 MHz, 1 WS <= 60 MHz, 2 WS <= 90 MHz, 3 WS <= 100 MHz.
       Un valor menor al necesario provoca lecturas erroneas de instrucciones. */
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK) {
        Error_Handler();
    }

    /* Actualiza la variable global SystemCoreClock y reprograma el SysTick
       para que siga generando interrupciones cada 1 ms con el nuevo HCLK. */
    SystemCoreClockUpdate();
    HAL_InitTick(TICK_INT_PRIORITY);

    /* --- 4. Cristal LSE de 32.768 kHz (NO critico) ----------------------- */
    /* Se intenta arrancar por separado. Si el cristal no esta montado o no
       oscila, la llamada agota LSE_STARTUP_TIMEOUT (5 s por defecto) y
       devuelve error, pero el sistema CONTINUA: el RTC recurrira al LSI y el
       resto de funciones queda intacto. El diagnostico se emite por UART. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_LSE;
    osc.PLL.PLLState   = RCC_PLL_NONE;   /* no volver a tocar el PLL ya activo */
    osc.LSEState       = RCC_LSE_ON;
    /* Alternativa: RCC_LSE_BYPASS si se inyecta un reloj externo ya generado */

    s_lse_ok = (HAL_RCC_OscConfig(&osc) == HAL_OK) ? 1U : 0U;

    if (!s_lse_ok) {
        /* Reserva: oscilador interno de baja velocidad. Permite que el RTC
           siga contando y que el equipo sea diagnosticable, pero NO cumple el
           enunciado: el LSI se detiene al desaparecer VDD, de modo que la
           hora no sobrevive a la perdida de alimentacion. La causa debe
           corregirse en el hardware (cristal X2 y puentes SB48/SB49). */
        osc.OscillatorType = RCC_OSCILLATORTYPE_LSI;
        osc.PLL.PLLState   = RCC_PLL_NONE;
        osc.LSIState       = RCC_LSI_ON;
        HAL_RCC_OscConfig(&osc);
    }

    /* --- 5. Salida MCO1 por defecto ------------------------------------- */
    system_clock_mco_set(MCO_SRC_PLL);
}

/*
 * system_clock_mco_set
 * Enruta la fuente indicada al pin PA8 (MCO1) con el mejor prescaler posible.
 *
 * Criterio de "mejor prescaler": la menor division que mantenga la senal
 * medible con un osciloscopio o analizador logico corriente.
 *   - HSI 16 MHz  -> /1  = 16 MHz     (sin division, se observa integra)
 *   - LSE 32.768 kHz -> /1 = 32.768 kHz (dividirla la haria irreconocible)
 *   - PLL 100 MHz -> /5  = 20 MHz     (/5 es la maxima division disponible;
 *                                      reduce el slew rate exigido al pin y
 *                                      evita el deterioro del flanco que
 *                                      presenta la salida directa a 100 MHz)
 *
 * HAL_RCC_MCOConfig se encarga internamente de habilitar el reloj de GPIOA y
 * de configurar PA8 como funcion alternativa AF0 a muy alta velocidad.
 */
void system_clock_mco_set(mco_src_t src)
{
    switch (src) {

    case MCO_SRC_HSI:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
        s_mco_freq = HSI_VALUE;              /* 16 000 000 Hz */
        break;

    case MCO_SRC_LSE:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_LSE, RCC_MCODIV_1);
        /* Si el cristal no oscila, MCO1 queda en reposo: se refleja con una
           frecuencia de 0 para que el informe por UART no sea enganoso. */
        s_mco_freq = s_lse_ok ? LSE_VALUE : 0U;
        break;

    case MCO_SRC_PLL:
    default:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_5);
        s_mco_freq = HAL_RCC_GetSysClockFreq() / 5U;  /* 100 MHz / 5 = 20 MHz */
        src = MCO_SRC_PLL;                   /* normaliza el valor guardado  */
        break;
    }

    s_mco_src = src;   /* memoriza la seleccion para consultarla despues */
}

/* Devuelve la fuente MCO1 activa. */
mco_src_t system_clock_mco_get(void)
{
    return s_mco_src;
}

/* Devuelve una cadena corta describiendo la senal presente en PA8. */
const char *system_clock_mco_label(void)
{
    switch (s_mco_src) {
    case MCO_SRC_HSI: return "MCO1:HSI 16MHz";
    case MCO_SRC_LSE: return s_lse_ok ? "MCO1:LSE 32768" : "MCO1:LSE NO OSC";
    case MCO_SRC_PLL: return "MCO1:PLL 20MHz";
    default:          return "MCO1:?";
    }
}

/* Indica si el LSE llego a oscilar. Lo consultan rtc.c y el diagnostico. */
uint8_t system_clock_lse_ok(void)
{
    return s_lse_ok;
}

/* Devuelve la frecuencia real presente en PA8, en hercios. */
uint32_t system_clock_mco_freq(void)
{
    return s_mco_freq;
}
