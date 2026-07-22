/*
 * i2c_lcd.c
 * Controlador de LCD HD44780 de 16x4 conectado por I2C mediante un PCF8574.
 *
 * El HD44780 se opera en modo de 4 bits: cada byte se envia en dos mitades
 * (nibbles), primero la alta y despues la baja. Cada nibble se valida con un
 * pulso descendente en la linea EN.
 *
 * Optimizacion importante frente a la implementacion ingenua: los cuatro
 * estados que componen un caracter (nibble alto con EN=1, con EN=0, nibble
 * bajo con EN=1, con EN=0) se envian en UNA sola transaccion I2C de cuatro
 * bytes. El PCF8574 acepta escrituras consecutivas y actualiza sus salidas
 * con cada byte recibido, de modo que el propio reloj del bus proporciona la
 * separacion temporal entre flancos. Esto reduce el tiempo de escritura de
 * una linea de unos 64 ms a unos 7 ms.
 */
#include "i2c_lcd.h"
#include "uart.h"

I2C_HandleTypeDef hi2c1;

/* Direccion de 7 bits del expansor detectada durante la inicializacion */
static uint8_t  s_addr7   = 0x27U;

/* 1 si algun dispositivo respondio en el bus */
static uint8_t  s_present = 0;

/* Numero de transacciones I2C que devolvieron error */
static uint32_t s_errors  = 0;

/* Estado de la retroiluminacion: se mezcla con cada byte enviado */
static uint8_t  s_backlight = 0x08U;   /* bit P3 a 1 = luz encendida */

/* Direcciones de memoria DDRAM donde empieza cada fila.
   En los modulos de 4 filas y 16 columnas las lineas no son contiguas:
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

/* ---------------------------------------------------------------------------
 * Capa fisica
 * ------------------------------------------------------------------------- */

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

/* ---------------------------------------------------------------------------
 * Bus I2C
 * ------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * Inicializacion del controlador
 * ------------------------------------------------------------------------- */
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

/* ---------------------------------------------------------------------------
 * Interfaz de dibujo
 * ------------------------------------------------------------------------- */
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

/* --- Diagnostico -------------------------------------------------------- */
uint8_t  lcd_is_present(void)  { return s_present; }
uint8_t  lcd_address(void)     { return s_addr7;   }
uint32_t lcd_error_count(void) { return s_errors;  }


