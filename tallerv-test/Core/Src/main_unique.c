/*
 * main_unique.c
 * VERSION MONOLITICA del proyecto: contiene TODAS las funciones propias del
 * firmware en un unico archivo, sin depender de ningun otro .c ni de las
 * cabeceras de Core/Inc.
 *
 * PARCIAL TALLER V 2026-1 - DANIEL VARGAS - UNAL
 * Plataforma: STM32 Nucleo-F411RE (STM32F411RET6)
 *
 * Que incluye este archivo (equivalente a la suma de los modulos separados):
 *   system_clock  : arbol de relojes a 100 MHz, LSE y salida MCO1 (PA8)
 *   gpio          : LED en PH1 y pulsador del joystick en PB5
 *   timer         : TIM3 (250 ms, LED) y TIM4 (100 ms, base del sistema)
 *   uart          : USART2 a 115200 por interrupciones, con buffer circular
 *   adc_joystick  : ADC1 sobre PA0/PA1 por interrupciones
 *   rtc           : RTC con LSE y despertador de 1 s
 *   i2c_lcd       : LCD HD44780 via PCF8574 sobre I2C1
 *   fsm           : maquina de estados y logica de presentacion
 *   hal_msp       : configuracion fisica de pines de cada periferico
 *   stm32f4xx_it  : rutinas de servicio de interrupcion
 *   syscalls      : stubs minimos que exige newlib
 *   main          : arranque y bucle principal
 *
 * Lo unico que queda fuera son las bibliotecas de terceros que el proyecto
 * usa tal cual: los drivers HAL de ST, el CMSIS y el startup en ensamblador.
 *
 * COMPILACION
 *   Este archivo es ALTERNATIVO a los modulos separados: define los mismos
 *   simbolos, de modo que NO puede enlazarse junto a ellos (habria duplicados).
 *   El Makefile lo contempla con la variable UNIQUE:
 *
 *       make            -> compila la version modular (por defecto)
 *       make UNIQUE=1   -> compila unicamente este archivo
 *
 *   O de forma manual, sustituyendo los .c de Core/Src (salvo este) por
 *   Core/Src/main_unique.c en la lista C_SOURCES.
 */

/* ==========================================================================
 * 0. Inclusiones: solo bibliotecas externas, ninguna cabecera del proyecto
 * ========================================================================== */
#include "stm32f4xx_hal.h"   /* API de la libreria HAL para la familia F4   */
#include <stdint.h>          /* tipos enteros de ancho fijo: uint8_t, etc.  */
#include <string.h>          /* memcpy, strlen, strncmp                     */
#include <stdio.h>           /* snprintf para formatear cadenas             */
#include <sys/stat.h>        /* struct stat, requerida por _fstat           */
#include <errno.h>           /* EINVAL, requerido por _kill                 */

/* ==========================================================================
 * 1. Constantes de configuracion (antes repartidas por Core/Inc)
 * ========================================================================== */

/* --- GPIO: LED de actividad ---------------------------------------------- */
#define LED_GPIO_PORT      GPIOH          /* puerto del LED                 */
#define LED_PIN            GPIO_PIN_1     /* PH1, impuesto por el enunciado */

/* --- GPIO: pulsador integrado del joystick ------------------------------- */
#define JOY_SW_GPIO_PORT   GPIOB          /* puerto del pulsador            */
#define JOY_SW_PIN         GPIO_PIN_5     /* PB5 = D4 en el conector Arduino */
#define JOY_SW_EXTI_IRQn   EXTI9_5_IRQn   /* las lineas EXTI5..9 comparten IRQ */

/* --- UART ----------------------------------------------------------------- */
#define UART_TX_BUF_SIZE   512U           /* potencia de 2: modulo barato    */

/* --- ADC / joystick ------------------------------------------------------- */
/* Umbrales sobre la escala de 12 bits (0..4095) del ADC. El centro mecanico
   queda proximo a 2048; la banda muerta evita que el ruido genere movimiento. */
#define JOY_CENTER         2048U
#define JOY_DEADZONE        600U          /* +-600 cuentas alrededor del centro */

/* --- RTC ------------------------------------------------------------------ */
/* Firma escrita en el registro de respaldo tras la primera inicializacion:
   permite saber si el RTC ya estaba en hora y no reescribirla en cada arranque. */
#define RTC_BACKUP_SIGNATURE   0x32F2A5C3U

/* --- LCD ------------------------------------------------------------------ */
#define LCD_COLS           20U
#define LCD_ROWS            4U

/* ==========================================================================
 * 2. Tipos propios del proyecto
 * ========================================================================== */

/*
 * mco_src_t: fuentes seleccionables para la salida MCO1 (PA8).
 */
typedef enum {
    MCO_SRC_HSI = 0,   /* oscilador interno de alta velocidad, 16 MHz     */
    MCO_SRC_LSE,       /* cristal externo de baja velocidad, 32.768 kHz   */
    MCO_SRC_PLL,       /* salida del PLL principal, 100 MHz               */
    MCO_SRC_COUNT      /* numero de fuentes (util para validar indices)   */
} mco_src_t;

/*
 * Estados de la maquina de estados.
 *   ST_INIT    : arranque; mensaje de bienvenida.
 *   ST_IDLE    : sin modo seleccionado; la linea 2 muestra el texto movil.
 *   ST_MODE_X  : modo X activo.       ST_MODE_Y : modo Y activo.
 *   ST_MESSAGE : mensaje temporal; al expirar se vuelve al estado guardado.
 *   ST_ERROR   : fallo irrecuperable de un periferico.
 */
typedef enum {
    ST_INIT = 0,
    ST_IDLE,
    ST_MODE_X,
    ST_MODE_Y,
    ST_MESSAGE,
    ST_ERROR,
    ST_COUNT
} fsm_state_t;

/* Eventos que consume la maquina de estados */
typedef enum {
    EV_NONE = 0,
    EV_CMD_CLEAR,      /* 'c' recibido por UART                      */
    EV_CMD_MODE_X,     /* 'x' recibido por UART                      */
    EV_CMD_MODE_Y,     /* 'y' recibido por UART                      */
    EV_CMD_INC,        /* '+' recibido por UART                      */
    EV_CMD_DEC,        /* '-' recibido por UART                      */
    EV_CMD_MCO_HSI,    /* 'h' recibido por UART                      */
    EV_CMD_MCO_LSE,    /* 'l' recibido por UART                      */
    EV_CMD_MCO_PLL,    /* 'p' recibido por UART                      */
    EV_CMD_HELP,       /* '?' recibido por UART                      */
    EV_CMD_DIAG,       /* 'd': informe de estado de los perifericos  */
    EV_CMD_SCAN,       /* 's': sondeo completo del bus I2C           */
    EV_BUTTON,         /* pulsador del joystick (EXTI)               */
    EV_TICK_100MS,     /* desbordamiento de TIM4                     */
    EV_SECOND,         /* despertador del RTC, una vez por segundo   */
    EV_COUNT
} fsm_event_t;

/* ==========================================================================
 * 3. Manejadores globales de periferico
 *    Son globales porque las ISR y varios modulos necesitan alcanzarlos.
 * ========================================================================== */
UART_HandleTypeDef huart2;
ADC_HandleTypeDef  hadc1;
I2C_HandleTypeDef  hi2c1;
RTC_HandleTypeDef  hrtc;
TIM_HandleTypeDef  htim3;   /* base de 250 ms para el LED */
TIM_HandleTypeDef  htim4;   /* base de 100 ms del sistema */

/* ==========================================================================
 * 4. Prototipos
 *    Al no haber cabeceras, se declaran aqui todas las funciones para que el
 *    orden de definicion dentro del archivo no imponga restricciones.
 * ========================================================================== */

/* --- comun --------------------------------------------------------------- */
void        Error_Handler(void);

/* --- system_clock -------------------------------------------------------- */
void        system_clock_init(void);
void        system_clock_mco_set(mco_src_t src);
mco_src_t   system_clock_mco_get(void);
const char *system_clock_mco_label(void);
uint32_t    system_clock_mco_freq(void);
uint8_t     system_clock_lse_ok(void);

/* --- gpio ---------------------------------------------------------------- */
void        gpio_init(void);
void        gpio_led_toggle(void);
void        gpio_led_write(uint8_t on);

/* --- uart ---------------------------------------------------------------- */
void        uart_init(void);
void        uart_send(const char *s);
void        uart_send_line(const char *s);

/* --- adc_joystick -------------------------------------------------------- */
void        adc_joystick_init(void);
void        adc_joystick_start(void);
uint16_t    adc_joystick_raw_x(void);
uint16_t    adc_joystick_raw_y(void);
int8_t      adc_joystick_dir_x(void);
int8_t      adc_joystick_dir_y(void);

/* --- rtc ----------------------------------------------------------------- */
void        rtc_init(void);
void        rtc_get_time(RTC_TimeTypeDef *t, RTC_DateTypeDef *d);
void        rtc_format_line(char *dst, uint8_t len);

/* --- i2c_lcd ------------------------------------------------------------- */
void        lcd_init(void);
void        lcd_clear(void);
void        lcd_set_cursor(uint8_t row, uint8_t col);
void        lcd_print(const char *s);
void        lcd_print_line(uint8_t row, const char *s);
uint8_t     lcd_is_present(void);
uint8_t     lcd_address(void);
uint32_t    lcd_error_count(void);
void        lcd_bus_scan_report(void);

/* --- timer --------------------------------------------------------------- */
void        timer_init(void);

/* --- fsm ----------------------------------------------------------------- */
void        fsm_init(void);
void        fsm_post_event(fsm_event_t ev);
void        fsm_run(void);
fsm_state_t fsm_get_state(void);

/* --- rutinas de servicio de interrupcion (nombres fijados por el startup) - */
/*
void NMI_Handler(void);
void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);
void SVC_Handler(void);
void DebugMon_Handler(void);
void PendSV_Handler(void);
*/
void SysTick_Handler(void);
void USART2_IRQHandler(void);
void ADC_IRQHandler(void);
void TIM3_IRQHandler(void);
void TIM4_IRQHandler(void);
void EXTI9_5_IRQHandler(void);
void RTC_WKUP_IRQHandler(void);

/* ==========================================================================
 * 5. MODULO system_clock
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
 * ========================================================================== */

/* Fuente actualmente enrutada a MCO1 */
static mco_src_t s_mco_src  = MCO_SRC_PLL;

/* Frecuencia real medida en PA8 tras aplicar el prescaler, en hercios */
static uint32_t  s_mco_freq = 20000000U;

/* 1 si el cristal LSE arranco correctamente; 0 si no esta montado o no oscila */
static uint8_t   s_lse_ok   = 0;

/*
 * system_clock_init
 * Configura la alimentacion, la latencia de FLASH, los osciladores y los buses.
 * Debe llamarse inmediatamente despues de HAL_Init() y antes de cualquier
 * inicializacion de periferico.
 */
void system_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};   /* configuracion de osciladores */
    RCC_ClkInitTypeDef clk = {0};   /* configuracion de buses       */

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
    osc.HSIState            = RCC_HSI_ON;                 /* enciende el HSI  */
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT; /* trim de fabrica  */

    osc.PLL.PLLState  = RCC_PLL_ON;              /* activa el PLL       */
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;       /* entrada del PLL     */
    osc.PLL.PLLM      = 8U;    /* divisor de entrada: 16 MHz / 8 = 2 MHz.
                                  La entrada al VCO debe quedar entre
                                  1 y 2 MHz; 2 MHz minimiza el jitter */
    osc.PLL.PLLN      = 100U;  /* multiplicador: 2 MHz * 100 = 200 MHz.
                                  El VCO debe quedar entre 100 y 432 MHz */
    osc.PLL.PLLP      = RCC_PLLP_DIV2; /* 200 MHz / 2 = 100 MHz = SYSCLK.
                                  Valores posibles: DIV2, DIV4, DIV6, DIV8 */
    osc.PLL.PLLQ      = 4U;    /* rama para USB/SDIO: 200 MHz / 4 = 50 MHz.
                                  No se usa en este proyecto, pero la HAL
                                  exige un valor valido (2..15) */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();       /* fallo del PLL: este si es irrecuperable */
    }

    /* --- 3. Buses del sistema ------------------------------------------- */
    /* Se indican los cuatro relojes que se van a modificar */
    clk.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
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
 *   - HSI 16 MHz     -> /1 = 16 MHz     (sin division, se observa integra)
 *   - LSE 32.768 kHz -> /1 = 32.768 kHz (dividirla la haria irreconocible)
 *   - PLL 100 MHz    -> /5 = 20 MHz     (/5 es la maxima division disponible;
 *                                        reduce el slew rate exigido al pin y
 *                                        evita el deterioro del flanco que
 *                                        presenta la salida directa a 100 MHz)
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

/* Indica si el LSE llego a oscilar. Lo consultan el RTC y el diagnostico. */
uint8_t system_clock_lse_ok(void)
{
    return s_lse_ok;
}

/* Devuelve la frecuencia real presente en PA8, en hercios. */
uint32_t system_clock_mco_freq(void)
{
    return s_mco_freq;
}

/* ==========================================================================
 * 6. MODULO gpio
 *    Pines discretos: LED en PH1 y pulsador SW del joystick en PB5.
 *    Los pines de periferico (ADC, UART, I2C, MCO) se configuran en la
 *    seccion del MSP, que es el lugar que la HAL reserva para ello.
 * ========================================================================== */

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

/* ==========================================================================
 * 7. MODULO uart
 *    USART2 a 115200 baudios, totalmente gobernada por interrupciones.
 *
 *    En la Nucleo-F411RE la USART2 esta cableada al depurador ST-LINK, que la
 *    expone en el ordenador como puerto virtual (/dev/ttyACM0 en Linux).
 *      PA2 -> USART2_TX     PA3 -> USART2_RX
 *
 *    Comandos aceptados (un solo caracter, sin pulsar Enter):
 *      c  coordenadas a cero     x  modo X          y  modo Y
 *      +  incrementa             -  decrementa
 *      h  MCO1 = HSI             l  MCO1 = LSE      p  MCO1 = PLL
 *      d  diagnostico            s  sondeo I2C      ?  ayuda
 * ========================================================================== */

/* Byte donde la HAL deposita cada caracter recibido */
static volatile uint8_t  s_rx_byte;

/* --- Buffer circular de transmision -------------------------------------- */
static volatile uint8_t  s_tx_buf[UART_TX_BUF_SIZE];
static volatile uint16_t s_tx_head = 0;   /* posicion de escritura */
static volatile uint16_t s_tx_tail = 0;   /* posicion de lectura   */
static volatile uint8_t  s_tx_busy = 0;   /* 1 si hay un envio en curso */

/* Byte que se esta transmitiendo en este momento */
static volatile uint8_t  s_tx_current;

/*
 * uart_start_tx
 * Si el buffer no esta vacio y no hay transmision en curso, extrae un byte
 * y lo entrega a la HAL. Se llama tanto desde el hilo principal como desde
 * la interrupcion de fin de transmision.
 */
static void uart_start_tx(void)
{
    if (s_tx_busy || (s_tx_head == s_tx_tail)) {
        return;   /* ya hay un envio activo, o no queda nada por enviar */
    }
    s_tx_current = s_tx_buf[s_tx_tail];
    s_tx_tail    = (uint16_t)((s_tx_tail + 1U) % UART_TX_BUF_SIZE);
    s_tx_busy    = 1U;

    HAL_UART_Transmit_IT(&huart2, (uint8_t *)&s_tx_current, 1U);
}

void uart_init(void)
{
    __HAL_RCC_USART2_CLK_ENABLE();

    huart2.Instance        = USART2;
    huart2.Init.BaudRate   = 115200U;   /* velocidad estandar; el ST-LINK
                                    admite hasta 1 Mbaudio */
    huart2.Init.WordLength = UART_WORDLENGTH_8B;  /* 8 bits de datos.
                                    Alternativa: 9B, usada cuando se anade
                                    bit de paridad y se quieren 8 datos */
    huart2.Init.StopBits   = UART_STOPBITS_1;
    huart2.Init.Parity     = UART_PARITY_NONE;
    huart2.Init.Mode       = UART_MODE_TX_RX;     /* bidireccional */
    huart2.Init.HwFlowCtl  = UART_HWCONTROL_NONE; /* sin RTS/CTS   */
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
                                    /* 16 muestras por bit: mayor inmunidad
                                       al ruido. OVERSAMPLING_8 permitiria
                                       doblar la velocidad maxima */

    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();
    }

    /* Prioridad 5: la mas alta de los perifericos del proyecto, para no
       perder caracteres si coinciden varias interrupciones. */
    HAL_NVIC_SetPriority(USART2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    /* Arma la primera recepcion. Sin esta llamada no se genera ninguna
       interrupcion de recepcion. */
    HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
}

/*
 * uart_send
 * Copia la cadena al buffer circular y activa la transmision si estaba parada.
 * No bloquea: si el buffer se llena, los caracteres sobrantes se descartan.
 */
void uart_send(const char *s)
{
    uint16_t next;

    while (*s != '\0') {
        next = (uint16_t)((s_tx_head + 1U) % UART_TX_BUF_SIZE);
        if (next == s_tx_tail) {
            break;   /* buffer lleno: se descarta el resto */
        }
        s_tx_buf[s_tx_head] = (uint8_t)*s++;
        s_tx_head = next;
    }
    uart_start_tx();
}

void uart_send_line(const char *s)
{
    uart_send(s);
    uart_send("\r\n");
}

/*
 * HAL_UART_TxCpltCallback
 * La HAL la invoca al terminar de transmitir el byte en curso.
 * Encadena el siguiente byte pendiente del buffer circular.
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) {
        return;
    }
    s_tx_busy = 0U;
    uart_start_tx();
}

/*
 * HAL_UART_RxCpltCallback
 * La HAL la invoca al recibir un caracter. Traduce el caracter a un evento
 * de la maquina de estados y vuelve a armar la recepcion.
 *
 * Se aceptan mayusculas y minusculas para mayor comodidad del operador.
 */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    uint8_t c;

    if (huart->Instance != USART2) {
        return;
    }

    c = s_rx_byte;

    switch (c) {
    case 'c': case 'C': fsm_post_event(EV_CMD_CLEAR);   break;
    case 'x': case 'X': fsm_post_event(EV_CMD_MODE_X);  break;
    case 'y': case 'Y': fsm_post_event(EV_CMD_MODE_Y);  break;
    case '+':           fsm_post_event(EV_CMD_INC);     break;
    case '-':           fsm_post_event(EV_CMD_DEC);     break;
    case 'h': case 'H': fsm_post_event(EV_CMD_MCO_HSI); break;
    case 'l': case 'L': fsm_post_event(EV_CMD_MCO_LSE); break;
    case 'p': case 'P': fsm_post_event(EV_CMD_MCO_PLL); break;
    case 'd': case 'D': fsm_post_event(EV_CMD_DIAG);    break;
    case 's': case 'S': fsm_post_event(EV_CMD_SCAN);    break;
    case '?':           fsm_post_event(EV_CMD_HELP);    break;
    default:            /* cualquier otro caracter se ignora */    break;
    }

    /* Rearma la recepcion del siguiente byte. Omitir esta llamada deja la
       UART sorda tras el primer caracter: es el error mas frecuente al
       trabajar con HAL_UART_Receive_IT. */
    HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
}

/*
 * HAL_UART_ErrorCallback
 * Se invoca ante errores de trama, ruido, paridad o desbordamiento.
 * Rearma la recepcion para que un caracter corrupto no deje la UART muda.
 */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        __HAL_UART_CLEAR_OREFLAG(huart);   /* limpia el error de desbordamiento */
        HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
    }
}

/* ==========================================================================
 * 8. MODULO adc_joystick
 *    Adquisicion de los dos ejes del joystick con ADC1 en modo interrupcion.
 *
 *    Estrategia: el ADC del STM32F4 dispone de un unico registro de datos.
 *    Para leer dos canales sin DMA se convierte de uno en uno y se conmuta el
 *    canal dentro del callback de fin de conversion:
 *
 *      TIM4 (100 ms) --> adc_joystick_start() --> conversion canal IN0
 *             callback --> guarda VRX, selecciona IN1, arranca de nuevo
 *             callback --> guarda VRY, secuencia terminada
 *
 *    Conexionado: VRX -> PA0 (IN0), VRY -> PA1 (IN1), SW -> PB5.
 * ========================================================================== */

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

/* ==========================================================================
 * 9. MODULO rtc
 *    RTC interno alimentado por el LSE, con despertador de 1 s.
 *
 *    Cadena de division del RTC:
 *      f_ck_spre = LSE / ((AsynchPrediv + 1) * (SynchPrediv + 1))
 *                = 32768 / (128 * 256) = 1 Hz
 *
 *    El RTC reside en el dominio de respaldo, alimentado desde VBAT cuando
 *    desaparece VDD: con pila, la cuenta sobrevive al corte de alimentacion.
 *    Se usa RTC_BKP_DR0 como testigo para no reinicializar la hora en cada
 *    arranque.
 * ========================================================================== */

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
    RTC_TimeTypeDef          time = {0};
    RTC_DateTypeDef          date = {0};
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

/* ==========================================================================
 * 10. MODULO i2c_lcd
 *     LCD HD44780 gobernado por un expansor PCF8574 sobre el bus I2C1.
 *
 *     Conexionado por defecto: SCL -> PB8, SDA -> PB9.
 *     Alternativo con -DLCD_USE_PB6_PB7: SCL -> PB6, SDA -> PB7.
 *     VCC -> 5V (el HD44780 no funciona de forma fiable a 3.3 V), GND -> GND.
 *
 *     Bits del PCF8574 hacia el HD44780:
 *       P0 -> RS   P1 -> RW   P2 -> EN   P3 -> luz   P4..P7 -> D4..D7
 *
 *     El HD44780 se opera en modo de 4 bits: cada byte se envia en dos
 *     mitades (nibbles). Optimizacion importante: los cuatro estados que
 *     componen un caracter se envian en UNA sola transaccion I2C de cuatro
 *     bytes; el propio reloj del bus separa los flancos. Esto reduce el
 *     tiempo de escritura de una linea de unos 64 ms a unos 7 ms.
 * ========================================================================== */

/* Direccion de 7 bits del expansor detectada durante la inicializacion */
static uint8_t  s_addr7   = 0x27U;

/* 1 si algun dispositivo respondio en el bus */
static uint8_t  s_present = 0;

/* Numero de transacciones I2C que devolvieron error */
static uint32_t s_errors  = 0;

/* Estado de la retroiluminacion: se mezcla con cada byte enviado */
static uint8_t  s_backlight = 0x08U;   /* bit P3 a 1 = luz encendida */

/* Direcciones de memoria DDRAM donde empieza cada fila.
   En los modulos de 4 filas las lineas no son contiguas:
   la fila 3 continua a la fila 1 y la fila 4 continua a la fila 2. */
static const uint8_t s_row_offset[LCD_ROWS] = { 0x00U, 0x40U, 0x14U, 0x54U };

/* ---------------------------------------------------------------------------
 * Retardo de microsegundos basado en el contador de ciclos del nucleo (DWT).
 * HAL_Delay solo ofrece resolucion de milisegundo, que aqui es tres ordenes de
 * magnitud mas de lo necesario y hace el refresco inaceptablemente lento.
 * El DWT existe en todos los Cortex-M3/M4/M7 y funciona sin depurador
 * conectado, siempre que se habilite TRCENA.
 * ------------------------------------------------------------------------- */
static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;  /* habilita el bloque de traza */
    DWT->CYCCNT = 0U;                                /* reinicia el contador        */
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;            /* arranca el contador         */
}

static void delay_us(uint32_t us)
{
    uint32_t start = DWT->CYCCNT;
    /* A 100 MHz cada microsegundo son 100 ciclos */
    uint32_t ticks = us * (SystemCoreClock / 1000000U);

    /* La resta de enteros sin signo es correcta aunque CYCCNT desborde */
    while ((DWT->CYCCNT - start) < ticks) {
        __NOP();
    }
}

/* --- Capa fisica --------------------------------------------------------- */

/*
 * lcd_tx
 * Envia n bytes al PCF8574 en una sola transaccion. Devuelve 1 si tuvo exito.
 * Contabiliza los fallos para poder diagnosticarlos desde la consola.
 */
static uint8_t lcd_tx(uint8_t *data, uint16_t n)
{
    HAL_StatusTypeDef st;

    /* La HAL espera la direccion de 8 bits: el bit 0 queda reservado al
       sentido de la transferencia, de ahi el desplazamiento a la izquierda.
       Pasar 0x27 en lugar de 0x4E es el error mas frecuente al portar codigo
       de Arduino, donde la libreria Wire espera los 7 bits sin desplazar. */
    st = HAL_I2C_Master_Transmit(&hi2c1, (uint16_t)(s_addr7 << 1), data, n, 50U);

    if (st != HAL_OK) {
        s_errors++;
        return 0U;
    }
    return 1U;
}

/*
 * lcd_send
 * Envia un byte completo al controlador en dos nibbles, con los cuatro
 * estados de la linea EN agrupados en una unica transaccion I2C.
 * mode = 0x00 -> comando (RS = 0);  mode = 0x01 -> dato (RS = 1).
 */
static void lcd_send(uint8_t value, uint8_t mode)
{
    uint8_t hi = (uint8_t)((value & 0xF0U)        | mode | s_backlight);
    uint8_t lo = (uint8_t)(((value << 4) & 0xF0U) | mode | s_backlight);

    uint8_t seq[4];
    seq[0] = (uint8_t)(hi | 0x04U);   /* nibble alto, EN = 1 */
    seq[1] = (uint8_t)(hi & ~0x04U);  /* nibble alto, EN = 0 -> captura */
    seq[2] = (uint8_t)(lo | 0x04U);   /* nibble bajo, EN = 1 */
    seq[3] = (uint8_t)(lo & ~0x04U);  /* nibble bajo, EN = 0 -> captura */

    lcd_tx(seq, 4U);

    /* El HD44780 necesita 37 us para ejecutar la mayoria de instrucciones.
       Los comandos lentos (borrado, retorno a origen) anaden su propia espera. */
    delay_us(50U);
}

/*
 * lcd_pulse_nibble
 * Variante para la secuencia de arranque, donde solo se envia el nibble alto
 * porque el controlador aun no esta en modo de 4 bits.
 */
static void lcd_pulse_nibble(uint8_t nibble)
{
    uint8_t seq[2];
    seq[0] = (uint8_t)(nibble | 0x04U);
    seq[1] = (uint8_t)(nibble & ~0x04U);

    lcd_tx(seq, 2U);
    delay_us(50U);
}

static void lcd_command(uint8_t c) { lcd_send(c, 0x00U); }
static void lcd_data(uint8_t c)    { lcd_send(c, 0x01U); }

/* --- Bus I2C ------------------------------------------------------------- */
static void i2c_bus_init(void)
{
    __HAL_RCC_I2C1_CLK_ENABLE();

    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 100000U;  /* 100 kHz, modo estandar */
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0U;
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2     = 0U;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;

    if (HAL_I2C_Init(&hi2c1) != HAL_OK) {
        Error_Handler();
    }
}

/*
 * lcd_bus_scan_report
 * Recorre todas las direcciones validas de 7 bits e informa por la consola de
 * cuales responden. Es la herramienta de diagnostico decisiva: distingue un
 * problema de direccion (responde algo, pero en otra direccion) de un problema
 * de cableado o alimentacion (no responde nadie).
 */
void lcd_bus_scan_report(void)
{
    char    buf[48];
    uint8_t found = 0U;
    uint8_t a;

    uart_send_line("");
    uart_send_line("--- Sondeo del bus I2C1 ---");

    /* 0x00 a 0x07 y 0x78 a 0x7F estan reservadas por la especificacion I2C */
    for (a = 0x08U; a <= 0x77U; a++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2U, 5U) == HAL_OK) {
            snprintf(buf, sizeof(buf), "  dispositivo en 0x%02X", (unsigned)a);
            uart_send_line(buf);
            found++;
        }
    }

    if (found == 0U) {
        uart_send_line("  NINGUN dispositivo responde.");
        uart_send_line("  Revisar: alimentacion del modulo, SDA/SCL sin cruzar,");
        uart_send_line("  GND comun y resistencias de pull-up.");
    } else {
        snprintf(buf, sizeof(buf), "  total: %u dispositivo(s)", (unsigned)found);
        uart_send_line(buf);
    }
    uart_send_line("---------------------------");
}

/* --- Inicializacion del controlador -------------------------------------- */
void lcd_init(void)
{
    char    buf[48];
    uint8_t a;

    dwt_init();
    i2c_bus_init();

    /* Deteccion automatica de la direccion. En lugar de probar solo 0x27 y
       0x3F se recorren los dos rangos completos que puede ocupar el expansor
       segun el estado de sus puentes A0/A1/A2:
         PCF8574  -> 0x20 a 0x27
         PCF8574A -> 0x38 a 0x3F
       Asi el firmware funciona con cualquier modulo comercial sin recompilar. */
    s_present = 0U;

    for (a = 0x20U; a <= 0x27U; a++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2U, 10U) == HAL_OK) {
            s_addr7 = a; s_present = 1U; break;
        }
    }
    if (!s_present) {
        for (a = 0x38U; a <= 0x3FU; a++) {
            if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 2U, 10U) == HAL_OK) {
                s_addr7 = a; s_present = 1U; break;
            }
        }
    }

    if (s_present) {
        snprintf(buf, sizeof(buf), "LCD: expansor detectado en 0x%02X",
                 (unsigned)s_addr7);
        uart_send_line(buf);
    } else {
        uart_send_line("LCD: NO se detecto ningun expansor I2C.");
        lcd_bus_scan_report();
        return;   /* sin dispositivo no tiene sentido enviar la secuencia */
    }

    /* Secuencia de arranque prescrita en la hoja de datos del HD44780 */
    HAL_Delay(50);              /* espera a que VCC se estabilice (>40 ms) */

    lcd_pulse_nibble(0x30U | s_backlight);  HAL_Delay(5);  /* 8 bits, intento 1 */
    lcd_pulse_nibble(0x30U | s_backlight);  HAL_Delay(1);  /* intento 2         */
    lcd_pulse_nibble(0x30U | s_backlight);  HAL_Delay(1);  /* intento 3         */
    lcd_pulse_nibble(0x20U | s_backlight);  HAL_Delay(1);  /* conmuta a 4 bits  */

    lcd_command(0x28U);   /* Function set: 4 bits, 2 lineas logicas, fuente 5x8 */
    lcd_command(0x08U);   /* Display off                                        */
    lcd_command(0x01U);   /* Clear display                                      */
    HAL_Delay(2);         /* el borrado tarda 1.52 ms                           */
    lcd_command(0x06U);   /* Entry mode: incrementa el cursor, sin desplazar    */
    lcd_command(0x0CU);   /* Display on, cursor oculto, sin parpadeo.
                             Alternativas: 0x0E muestra el cursor, 0x0F lo hace
                             parpadear                                          */

    if (s_errors != 0U) {
        snprintf(buf, sizeof(buf), "LCD: %lu fallos I2C durante el arranque",
                 (unsigned long)s_errors);
        uart_send_line(buf);
    }
}

/* --- Interfaz de dibujo -------------------------------------------------- */
void lcd_clear(void)
{
    if (!s_present) return;
    lcd_command(0x01U);
    HAL_Delay(2);
}

void lcd_set_cursor(uint8_t row, uint8_t col)
{
    if (!s_present) return;
    if (row >= LCD_ROWS) row = LCD_ROWS - 1U;
    if (col >= LCD_COLS) col = LCD_COLS - 1U;

    /* El comando Set DDRAM Address lleva el bit 7 a 1 */
    lcd_command((uint8_t)(0x80U | (s_row_offset[row] + col)));
}

void lcd_print(const char *s)
{
    if (!s_present) return;
    while (*s != '\0') {
        lcd_data((uint8_t)*s++);
    }
}

/*
 * lcd_print_line
 * Escribe una linea completa rellenando con espacios hasta el final, de forma
 * que no queden restos del texto anterior. Evita tener que borrar la pantalla
 * entera, operacion lenta que produce parpadeo visible.
 */
void lcd_print_line(uint8_t row, const char *s)
{
    uint8_t i;
    uint8_t end = 0U;   /* 1 cuando ya se alcanzo el fin de la cadena */

    if (!s_present) return;

    lcd_set_cursor(row, 0U);

    for (i = 0U; i < LCD_COLS; i++) {
        if (!end && (s[i] == '\0')) {
            end = 1U;
        }
        lcd_data((uint8_t)(end ? ' ' : s[i]));
    }
}

/* --- Diagnostico --------------------------------------------------------- */
uint8_t  lcd_is_present(void)  { return s_present; }
uint8_t  lcd_address(void)     { return s_addr7;   }
uint32_t lcd_error_count(void) { return s_errors;  }

/* ==========================================================================
 * 11. MODULO timer
 *     TIM3 (LED a 250 ms) y TIM4 (base de tiempo de 100 ms).
 *
 *     Calculo de la temporizacion:
 *       f_TIM = f_APB1 * 2 = 50 MHz * 2 = 100 MHz
 *       f_cnt = f_TIM / (PSC + 1)
 *       T     = (ARR + 1) / f_cnt
 *
 *     TIM3: PSC = 9999 -> f_cnt = 10 kHz; ARR = 2499 -> T = 250 ms
 *     TIM4: PSC = 9999 -> f_cnt = 10 kHz; ARR =  999 -> T = 100 ms
 *
 *     Se eligen PSC y ARR de forma que ARR quede lo mas grande posible dentro
 *     de los 16 bits: cuanto mayor es ARR, mejor la resolucion del periodo.
 * ========================================================================== */

void timer_init(void)
{
    TIM_ClockConfigTypeDef  src = {0};  /* fuente de reloj del contador   */
    TIM_MasterConfigTypeDef mst = {0};  /* configuracion maestro/esclavo  */

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

/* ==========================================================================
 * 12. MODULO fsm
 *     Maquina de estados finitos y logica de presentacion.
 *
 *     Arquitectura: productor-consumidor.
 *       - Productores: las rutinas de interrupcion (UART, TIM4, EXTI, RTC).
 *         Solo depositan un evento en una cola circular y retornan.
 *       - Consumidor: fsm_run(), llamado desde el bucle infinito de main().
 *         Extrae los eventos y ejecuta las transiciones y el refresco del LCD.
 *
 *     Distribucion de la pantalla:
 *       Fila 0: HH:MM:SS DD-MM
 *       Fila 1: texto movil, o "MODO X" / "MODO Y", o mensaje temporal
 *       Fila 2: X:<coordenada> ADC:<muestra cruda>
 *       Fila 3: Y:<coordenada> ADC:<muestra cruda>
 * ========================================================================== */

/* ----------------------- Cola circular de eventos ------------------------ */
#define EVQ_SIZE  32U   /* holgado: los eventos se consumen en pocos ms */

static volatile fsm_event_t s_evq[EVQ_SIZE];
static volatile uint8_t     s_evq_head = 0;   /* escribe la ISR    */
static volatile uint8_t     s_evq_tail = 0;   /* lee el bucle main */

/* ----------------------- Variables de estado ----------------------------- */
static fsm_state_t s_state = ST_INIT;
static fsm_state_t s_prev  = ST_IDLE;   /* estado al que volver tras un mensaje */

/* Coordenadas gobernadas conjuntamente por el joystick y por la UART */
static int16_t s_coord_x = 0;
static int16_t s_coord_y = 0;

#define COORD_MIN  (-999)
#define COORD_MAX  ( 999)

/* Texto de la fila 1 y desplazamiento actual del efecto de marquesina */
static const char *s_line2  = NULL;
static uint16_t    s_scroll = 0;

/* Copia de lo ultimo escrito en cada fila. Evita reenviar por I2C una linea
   que no ha cambiado: escribir una fila cuesta unos 7 ms, de modo que
   refrescar las cuatro sin necesidad consumiria una tercera parte del
   periodo de 100 ms sin aportar nada. */
static char s_shadow[LCD_ROWS][LCD_COLS + 1U];

/* Contadores de reparto de tareas, en unidades de 100 ms */
static uint16_t s_tick_scroll = 0;   /* desplaza el texto cada 300 ms */
static uint16_t s_msg_timeout = 0;   /* vida restante del mensaje temporal */

/* Textos fijos */
static const char TXT_BANNER[]  = "DANIEL VARGAS - PARCIAL TALLER V 2026-1 - UNAL";
static const char TXT_MODE_X[]  = "MODO X";
static const char TXT_MODE_Y[]  = "MODO Y";
static const char TXT_NO_MODE[] = "ACTIVAR MODO X O Y";

#define MSG_DURATION_TICKS  30U   /* 30 * 100 ms = 3 s */

/*
 * fsm_post_event
 * Deposita un evento. Es la unica funcion del modulo que pueden invocar las
 * rutinas de interrupcion. Si la cola esta llena el evento se descarta, lo
 * que es preferible a bloquear una ISR.
 */
void fsm_post_event(fsm_event_t ev)
{
    uint8_t next = (uint8_t)((s_evq_head + 1U) % EVQ_SIZE);

    if (next == s_evq_tail) {
        return;   /* cola llena */
    }
    s_evq[s_evq_head] = ev;
    s_evq_head = next;
}

/*
 * fsm_get_event
 * Extrae el evento mas antiguo. Devuelve EV_NONE si no hay ninguno.
 */
static fsm_event_t fsm_get_event(void)
{
    fsm_event_t ev;

    if (s_evq_head == s_evq_tail) {
        return EV_NONE;
    }
    ev = s_evq[s_evq_tail];
    s_evq_tail = (uint8_t)((s_evq_tail + 1U) % EVQ_SIZE);
    return ev;
}

/* ----------------------- Utilidades de presentacion ---------------------- */

/*
 * clamp_coord
 * Satura un valor dentro del rango representable en la pantalla.
 */
static int16_t clamp_coord(int32_t v)
{
    if (v > COORD_MAX) return (int16_t)COORD_MAX;
    if (v < COORD_MIN) return (int16_t)COORD_MIN;
    return (int16_t)v;
}

/*
 * lcd_update
 * Escribe una fila solo si su contenido difiere del ultimo enviado.
 */
static void lcd_update(uint8_t row, const char *txt)
{
    char norm[LCD_COLS + 1U];
    uint8_t i, end = 0U;

    /* Normaliza a la anchura exacta de la pantalla para que la comparacion
       sea fiable */
    for (i = 0U; i < LCD_COLS; i++) {
        if (!end && (txt[i] == '\0')) end = 1U;
        norm[i] = end ? ' ' : txt[i];
    }
    norm[LCD_COLS] = '\0';

    if (strncmp(norm, s_shadow[row], LCD_COLS) == 0) {
        return;                       /* sin cambios: no se toca el bus */
    }
    memcpy(s_shadow[row], norm, LCD_COLS + 1U);
    lcd_print_line(row, norm);
}

/*
 * set_line2
 * Cambia el texto de la fila 1 y reinicia el desplazamiento.
 */
static void set_line2(const char *txt)
{
    s_line2  = txt;
    s_scroll = 0;
}

/*
 * render_line2
 * Compone la ventana visible de la fila 1.
 * Si el texto cabe entero se muestra fijo; si no, se desplaza ciclicamente
 * anadiendo un separador de tres espacios entre el final y el principio.
 */
static void render_line2(void)
{
    char     win[LCD_COLS + 1U];
    uint16_t len, total, i, idx;

    if (s_line2 == NULL) {
        return;
    }

    len = (uint16_t)strlen(s_line2);

    if (len <= LCD_COLS) {
        lcd_update(1U, s_line2);   /* cabe entero: sin desplazamiento */
        return;
    }

    total = (uint16_t)(len + 3U);      /* longitud del ciclo con separador */

    for (i = 0U; i < LCD_COLS; i++) {
        idx = (uint16_t)((s_scroll + i) % total);
        win[i] = (idx < len) ? s_line2[idx] : ' ';
    }
    win[LCD_COLS] = '\0';

    lcd_update(1U, win);
}

/*
 * render_time
 * Refresca la fila 0 con la hora y la fecha del RTC.
 */
static void render_time(void)
{
    char buf[LCD_COLS + 1U];

    rtc_format_line(buf, sizeof(buf));
    lcd_update(0U, buf);
}

/*
 * render_coords
 * Refresca las filas 2 y 3 con la coordenada y la muestra cruda de cada eje.
 * Formato: "X:+025 ADC:2048".
 */
static void render_coords(void)
{
    /* El buffer se dimensiona con holgura para que el compilador pueda
       descartar cualquier posibilidad de truncamiento en snprintf. */
    char buf[24];

    snprintf(buf, sizeof(buf), "X:%+04d ADC:%04u",
             (int)s_coord_x, (unsigned)adc_joystick_raw_x());
    lcd_update(2U, buf);

    snprintf(buf, sizeof(buf), "Y:%+04d ADC:%04u",
             (int)s_coord_y, (unsigned)adc_joystick_raw_y());
    lcd_update(3U, buf);
}

/*
 * show_message
 * Muestra un texto temporal en la fila 1 y programa el regreso al estado
 * anterior transcurridos 3 segundos.
 */
static void show_message(const char *txt)
{
    if (s_state != ST_MESSAGE) {
        s_prev = s_state;      /* memoriza a donde volver */
    }
    s_state       = ST_MESSAGE;
    s_msg_timeout = MSG_DURATION_TICKS;
    set_line2(txt);
    render_line2();
}

/*
 * restore_line2
 * Devuelve a la fila 1 el texto que corresponde al estado activo.
 */
static void restore_line2(void)
{
    switch (s_state) {
    case ST_MODE_X: set_line2(TXT_MODE_X); break;
    case ST_MODE_Y: set_line2(TXT_MODE_Y); break;
    default:        set_line2(TXT_BANNER); break;
    }
    render_line2();
}

/* ----------------------- Acciones sobre las coordenadas ------------------ */

/*
 * coord_step
 * Aplica un incremento a la coordenada del modo activo. Si no hay modo
 * seleccionado emite el aviso exigido por el enunciado.
 */
static void coord_step(int16_t delta)
{
    switch (s_state) {

    case ST_MODE_X:
        s_coord_x = clamp_coord((int32_t)s_coord_x + delta);
        render_coords();
        break;

    case ST_MODE_Y:
        s_coord_y = clamp_coord((int32_t)s_coord_y + delta);
        render_coords();
        break;

    case ST_MESSAGE:
        /* Si ya se esta mostrando el aviso se prolonga su duracion, salvo que
           el modo memorizado sea valido, en cuyo caso se aplica el paso. */
        if (s_prev == ST_MODE_X) {
            s_coord_x = clamp_coord((int32_t)s_coord_x + delta);
            render_coords();
        } else if (s_prev == ST_MODE_Y) {
            s_coord_y = clamp_coord((int32_t)s_coord_y + delta);
            render_coords();
        } else {
            s_msg_timeout = MSG_DURATION_TICKS;
        }
        break;

    default:
        show_message(TXT_NO_MODE);
        uart_send_line("ERROR: seleccione modo con 'x' o 'y'");
        break;
    }
}

/*
 * apply_joystick
 * Traduce la posicion del joystick en incrementos de coordenada. Se ejecuta
 * cada 100 ms, de modo que mantener la palanca desviada produce un barrido
 * continuo de 10 unidades por segundo.
 */
static void apply_joystick(void)
{
    int8_t dx = adc_joystick_dir_x();
    int8_t dy = adc_joystick_dir_y();

    if (dx != 0) {
        s_coord_x = clamp_coord((int32_t)s_coord_x + dx);
    }
    if (dy != 0) {
        s_coord_y = clamp_coord((int32_t)s_coord_y + dy);
    }
}

/* ----------------------- Ayuda por consola ------------------------------- */
static void print_help(void)
{
    uart_send_line("");
    uart_send_line("=== PARCIAL TALLER V 2026-1 - DANIEL VARGAS - UNAL ===");
    uart_send_line(" c : coordenadas X e Y a cero");
    uart_send_line(" x : activa modo X");
    uart_send_line(" y : activa modo Y");
    uart_send_line(" + : incrementa la coordenada del modo activo");
    uart_send_line(" - : decrementa la coordenada del modo activo");
    uart_send_line(" h : MCO1 (PA8) = HSI 16 MHz");
    uart_send_line(" l : MCO1 (PA8) = LSE 32768 Hz");
    uart_send_line(" p : MCO1 (PA8) = PLL / 5 = 20 MHz");
    uart_send_line(" d : informe de diagnostico del sistema");
    uart_send_line(" s : sondeo completo del bus I2C");
    uart_send_line(" ? : esta ayuda");
    uart_send_line("======================================================");
}

/*
 * print_diagnostics
 * Informe de estado de los perifericos criticos. Se emite al arrancar y bajo
 * demanda con el comando 'd'. Permite localizar un fallo de montaje sin
 * depurador y sin osciloscopio.
 */
static void print_diagnostics(void)
{
    char buf[64];

    uart_send_line("");
    uart_send_line("--- Diagnostico del sistema ---");

    snprintf(buf, sizeof(buf), " SYSCLK        : %lu Hz",
             (unsigned long)HAL_RCC_GetSysClockFreq());
    uart_send_line(buf);

    snprintf(buf, sizeof(buf), " MCO1 (PA8)    : %s", system_clock_mco_label());
    uart_send_line(buf);

    uart_send_line(system_clock_lse_ok()
                   ? " Cristal LSE   : OK (32768 Hz)"
                   : " Cristal LSE   : NO ARRANCA -> RTC sobre LSI");

    if (lcd_is_present()) {
        snprintf(buf, sizeof(buf), " LCD I2C       : OK en 0x%02X, %lu fallos",
                 (unsigned)lcd_address(), (unsigned long)lcd_error_count());
    } else {
        snprintf(buf, sizeof(buf), " LCD I2C       : NO DETECTADO");
    }
    uart_send_line(buf);

    snprintf(buf, sizeof(buf), " Joystick ADC  : X=%u  Y=%u",
             (unsigned)adc_joystick_raw_x(), (unsigned)adc_joystick_raw_y());
    uart_send_line(buf);
    uart_send_line("-------------------------------");
}

/* ----------------------- Interfaz publica -------------------------------- */

void fsm_init(void)
{
    s_state   = ST_IDLE;
    s_prev    = ST_IDLE;
    s_coord_x = 0;
    s_coord_y = 0;

    set_line2(TXT_BANNER);

    lcd_clear();
    render_time();
    render_line2();
    render_coords();

    print_help();
    print_diagnostics();
}

fsm_state_t fsm_get_state(void)
{
    return s_state;
}

/*
 * fsm_run
 * Procesa todos los eventos pendientes. Se invoca sin cesar desde main().
 */
void fsm_run(void)
{
    fsm_event_t ev;

    /* El despertador del RTC deja una bandera en lugar de un evento para no
       depender del tamano de la cola si el bucle principal se retrasa. */
    if (g_rtc_second_flag) {
        g_rtc_second_flag = 0U;
        fsm_post_event(EV_SECOND);
    }

    while ((ev = fsm_get_event()) != EV_NONE) {

        switch (ev) {

        /* ---------- Comandos de coordenada ---------- */
        case EV_CMD_CLEAR:
            s_coord_x = 0;
            s_coord_y = 0;
            render_coords();
            uart_send_line("OK: coordenadas a cero");
            break;

        case EV_CMD_MODE_X:
            s_state = ST_MODE_X;
            restore_line2();
            uart_send_line("OK: modo X");
            break;

        case EV_CMD_MODE_Y:
            s_state = ST_MODE_Y;
            restore_line2();
            uart_send_line("OK: modo Y");
            break;

        case EV_CMD_INC:
            coord_step(+1);
            break;

        case EV_CMD_DEC:
            coord_step(-1);
            break;

        /* ---------- Conmutacion de la salida MCO1 ---------- */
        case EV_CMD_MCO_HSI:
            system_clock_mco_set(MCO_SRC_HSI);
            uart_send_line("OK: MCO1 = HSI 16 MHz (prescaler /1)");
            break;

        case EV_CMD_MCO_LSE:
            system_clock_mco_set(MCO_SRC_LSE);
            uart_send_line("OK: MCO1 = LSE 32768 Hz (prescaler /1)");
            break;

        case EV_CMD_MCO_PLL:
            system_clock_mco_set(MCO_SRC_PLL);
            uart_send_line("OK: MCO1 = PLL 100 MHz / 5 = 20 MHz");
            break;

        case EV_CMD_HELP:
            print_help();
            break;

        /* ---------- Diagnostico ---------- */
        case EV_CMD_DIAG:
            print_diagnostics();
            break;

        case EV_CMD_SCAN:
            lcd_bus_scan_report();
            break;

        /* ---------- Pulsador del joystick ---------- */
        case EV_BUTTON:
            /* Recorre ciclicamente los modos: ninguno -> X -> Y -> ninguno */
            if (s_state == ST_MODE_X) {
                s_state = ST_MODE_Y;
            } else if (s_state == ST_MODE_Y) {
                s_state = ST_IDLE;
            } else {
                s_state = ST_MODE_X;
            }
            restore_line2();
            uart_send_line("SW: cambio de modo");
            break;

        /* ---------- Base de tiempo de 100 ms ---------- */
        case EV_TICK_100MS:
            apply_joystick();     /* la palanca desplaza las coordenadas */
            render_coords();      /* refresca las filas 2 y 3            */

            /* Desplaza el texto movil cada tercer tick, esto es, 300 ms */
            if (++s_tick_scroll >= 3U) {
                s_tick_scroll = 0U;
                s_scroll++;
                render_line2();
            }

            /* Vencimiento del mensaje temporal */
            if ((s_state == ST_MESSAGE) && (s_msg_timeout > 0U)) {
                if (--s_msg_timeout == 0U) {
                    s_state = s_prev;   /* vuelve al estado anterior */
                    restore_line2();
                }
            }
            break;

        /* ---------- Segundo del RTC ---------- */
        case EV_SECOND:
            render_time();
            break;

        default:
            break;
        }
    }
}

/* ==========================================================================
 * 13. MSP: Microcontroller Support Package
 *
 *     La HAL separa la configuracion logica de un periferico (registros de
 *     modo, velocidad, formato) de su conexion fisica (reloj de bus, pines,
 *     NVIC). La primera vive en el modulo del periferico; la segunda se
 *     centraliza aqui, porque HAL_<PPP>_Init() invoca automaticamente a
 *     HAL_<PPP>_MspInit().
 *
 *     Mapa de pines completo:
 *       PA0  ADC1_IN0    VRX del joystick
 *       PA1  ADC1_IN1    VRY del joystick
 *       PA2  USART2_TX   consola serie (via ST-LINK)
 *       PA3  USART2_RX   consola serie (via ST-LINK)
 *       PA8  MCO1        salida de reloj conmutable
 *       PB5  EXTI5       pulsador SW del joystick
 *       PB8  I2C1_SCL    reloj del bus del LCD
 *       PB9  I2C1_SDA    datos del bus del LCD
 *       PH1  GPIO out    LED blinky
 *       PC14 OSC32_IN    cristal LSE de 32.768 kHz
 *       PC15 OSC32_OUT   cristal LSE de 32.768 kHz
 * ========================================================================== */

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
void HAL_RTC_MspInit(RTC_HandleTypeDef *h)
{
    (void)h;
    /* El reloj del RTC ya se habilito en rtc_init con __HAL_RCC_RTC_ENABLE */
}

/* ==========================================================================
 * 14. Rutinas de servicio de interrupcion
 *
 *     Todos los perifericos salvo el I2C trabajan por interrupcion, tal como
 *     exige el enunciado. Cada ISR se limita a delegar en el manejador
 *     generico de la HAL, que a su vez invoca el callback correspondiente.
 *     Ninguna ISR contiene logica de aplicacion.
 *
 *     Mapa de interrupciones y prioridades (0 = mas urgente):
 *       USART2_IRQn    prioridad 5   recepcion y transmision serie
 *       ADC_IRQn       prioridad 6   fin de conversion de cada eje
 *       TIM3_IRQn      prioridad 7   base de 250 ms para el LED
 *       TIM4_IRQn      prioridad 7   base de 100 ms del sistema
 *       EXTI9_5_IRQn   prioridad 8   pulsador SW del joystick (PB5)
 *       RTC_WKUP_IRQn  prioridad 9   despertador de 1 s del RTC
 *       SysTick        prioridad 15  contador de milisegundos de la HAL
 * ========================================================================== */

/* --- Excepciones del nucleo Cortex-M4 ------------------------------------ */
void NMI_Handler(void)        { while (1) { } }  /* no enmascarable        */
void HardFault_Handler(void)  { while (1) { } }  /* fallo grave            */
void MemManage_Handler(void)  { while (1) { } }  /* violacion de la MPU    */
void BusFault_Handler(void)   { while (1) { } }  /* fallo de bus           */
void UsageFault_Handler(void) { while (1) { } }  /* instruccion invalida   */
void SVC_Handler(void)        { }                /* llamada al supervisor  */
void DebugMon_Handler(void)   { }                /* monitor de depuracion  */
void PendSV_Handler(void)     { }                /* cambio de contexto     */

/*
 * SysTick_Handler
 * Se dispara cada milisegundo. Incrementa el contador que sostiene
 * HAL_GetTick() y HAL_Delay(); sin el, cualquier funcion de la HAL con
 * tiempo maximo de espera se bloquearia para siempre.
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* --- Interrupciones de periferico ---------------------------------------- */

/*
 * USART2_IRQHandler
 * Atiende recepcion, transmision y errores. HAL_UART_IRQHandler decide cual
 * de los tres casos aplica e invoca HAL_UART_RxCpltCallback,
 * HAL_UART_TxCpltCallback o HAL_UART_ErrorCallback.
 */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

/*
 * ADC_IRQHandler
 * El STM32F411 comparte un unico vector para todos sus ADC.
 * Deriva en HAL_ADC_ConvCpltCallback.
 */
void ADC_IRQHandler(void)
{
    HAL_ADC_IRQHandler(&hadc1);
}

/*
 * TIM3_IRQHandler
 * Desbordamiento cada 250 ms. Deriva en HAL_TIM_PeriodElapsedCallback,
 * que conmuta el LED de PH1.
 */
void TIM3_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim3);
}

/*
 * TIM4_IRQHandler
 * Desbordamiento cada 100 ms. Lanza la conversion del ADC y encola el
 * evento periodico de la maquina de estados.
 */
void TIM4_IRQHandler(void)
{
    HAL_TIM_IRQHandler(&htim4);
}

/*
 * EXTI9_5_IRQHandler
 * Vector compartido por las lineas EXTI 5 a 9. HAL_GPIO_EXTI_IRQHandler
 * comprueba cual de ellas esta pendiente, limpia la bandera e invoca
 * HAL_GPIO_EXTI_Callback.
 */
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(JOY_SW_PIN);
}

/*
 * RTC_WKUP_IRQHandler
 * Despertador periodico de 1 s. Deriva en
 * HAL_RTCEx_WakeUpTimerEventCallback.
 */
void RTC_WKUP_IRQHandler(void)
{
    HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc);
}

/*
 * HAL_GPIO_EXTI_Callback
 * Callback comun a todas las lineas EXTI. Incorpora el antirrebote por
 * software: los contactos mecanicos del pulsador generan decenas de flancos
 * espurios durante algunos milisegundos, por lo que se ignora todo flanco
 * que llegue antes de 200 ms desde el anterior.
 */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    static uint32_t last = 0;    /* marca temporal del ultimo flanco valido */
    uint32_t now = HAL_GetTick();

    if (pin != JOY_SW_PIN) {
        return;
    }
    if ((now - last) < 200U) {
        return;                  /* rebote: se descarta */
    }
    last = now;

    fsm_post_event(EV_BUTTON);
}

/* ==========================================================================
 * 15. Stubs de llamadas al sistema que exige newlib
 *     En un sistema bare-metal no hay sistema operativo, por lo que se
 *     proporcionan implementaciones minimas para satisfacer al enlazador.
 * ========================================================================== */

/* Puntero al final de la region estatica; lo define el linker script */
extern int _end;

/*
 * _sbrk: reserva memoria para el heap (usado por malloc).
 * Avanza un puntero desde el final de .bss.
 */
void *_sbrk(int incr)
{
    static unsigned char *heap = NULL;  /* posicion actual del heap */
    unsigned char *prev;

    if (heap == NULL) {
        heap = (unsigned char *)&_end;  /* primera llamada: arranca en _end */
    }
    prev = heap;
    heap += incr;
    return (void *)prev;
}

int  _close(int file)                       { (void)file; return -1; }
int  _fstat(int file, struct stat *st)      { (void)file; st->st_mode = S_IFCHR; return 0; }
int  _isatty(int file)                      { (void)file; return 1; }
int  _lseek(int file, int ptr, int dir)     { (void)file; (void)ptr; (void)dir; return 0; }
int  _read(int file, char *ptr, int len)    { (void)file; (void)ptr; (void)len; return 0; }
int  _write(int file, char *ptr, int len)   { (void)file; (void)ptr; return len; }
void _exit(int status)                      { (void)status; while (1) { } }
int  _kill(int pid, int sig)                { (void)pid; (void)sig; errno = EINVAL; return -1; }
int  _getpid(void)                          { return 1; }

/* ==========================================================================
 * 16. Punto de entrada
 * ========================================================================== */

int main(void)
{
    /* --- Capa de arranque ------------------------------------------------ */
    HAL_Init();     /* configura la FLASH, el SysTick a 1 ms y llama a
                       HAL_MspInit(). Debe ser la primera llamada. */

    /* Cuatro bits de prioridad de expropiacion y ninguno de subprioridad.
       Simplifica el diseno: el numero que se pasa a HAL_NVIC_SetPriority es
       directamente la prioridad, entre 0 (maxima) y 15 (minima). */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    system_clock_init();   /* SYSCLK a 100 MHz, LSE en marcha, MCO1 activa */

    /* --- Perifericos ----------------------------------------------------- */
    gpio_init();           /* LED en PH1 y pulsador del joystick en PB5     */
    uart_init();           /* USART2 a 115200 por interrupciones            */
    adc_joystick_init();   /* ADC1 sobre PA0 y PA1 por interrupciones       */
    rtc_init();            /* RTC con LSE y despertador de 1 s              */
    lcd_init();            /* LCD por I2C1 (unico periferico sondeado)      */
    timer_init();          /* TIM3 (LED, 250 ms) y TIM4 (base de 100 ms)    */

    /* --- Aplicacion ------------------------------------------------------ */
    fsm_init();            /* estado inicial y pantalla de bienvenida       */

    while (1) {
        fsm_run();         /* consume los eventos encolados por las ISR     */
    }
}

/*
 * Error_Handler
 * Punto unico de fallo. Deshabilita las interrupciones y hace parpadear el
 * LED muy rapido para que el error sea visible sin necesidad de depurador.
 */
void Error_Handler(void)
{
    __disable_irq();

    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
        /* Retardo por bucle: HAL_Delay depende del SysTick, que esta
           deshabilitado junto con el resto de interrupciones. */
        for (volatile uint32_t i = 0; i < 500000U; i++) {
            __NOP();
        }
    }
}

#ifdef USE_FULL_ASSERT
/*
 * assert_failed
 * La HAL la invoca cuando un parametro no supera sus comprobaciones internas.
 * Solo se compila si se define USE_FULL_ASSERT en stm32f4xx_hal_conf.h.
 */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    (void)line;
    Error_Handler();
}
#endif
