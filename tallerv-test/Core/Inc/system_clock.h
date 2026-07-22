/*
 * system_clock.h
 * Modulo de configuracion del arbol de relojes y de la salida MCO1.
 *
 * Responsabilidades:
 *   - Llevar el SYSCLK a 100 MHz (maximo del STM32F411RE) usando el PLL.
 *   - Arrancar el LSE (cristal de 32.768 kHz) para el RTC.
 *   - Exponer por PA8 (MCO1) las senales HSI, LSE o PLL, conmutables por UART.
 */
#ifndef SYSTEM_CLOCK_H
#define SYSTEM_CLOCK_H

#include "main.h"

/*
 * mco_src_t: fuentes seleccionables para la salida MCO1.
 * Se usa un enum en lugar de constantes sueltas para que el compilador
 * verifique los valores admitidos y para documentar el codigo.
 */
typedef enum {
    MCO_SRC_HSI = 0,   /* oscilador interno de alta velocidad, 16 MHz     */
    MCO_SRC_LSE,       /* cristal externo de baja velocidad, 32.768 kHz   */
    MCO_SRC_PLL,       /* salida del PLL principal, 100 MHz               */
    MCO_SRC_COUNT      /* numero de fuentes (util para validar indices)   */
} mco_src_t;

void        system_clock_init(void);              /* SYSCLK a 100 MHz      */
void        system_clock_mco_set(mco_src_t src);  /* conmuta fuente MCO1   */
mco_src_t   system_clock_mco_get(void);           /* fuente MCO1 activa    */
const char *system_clock_mco_label(void);         /* texto para LCD/UART   */
uint32_t    system_clock_mco_freq(void);          /* frecuencia real [Hz]  */
uint8_t     system_clock_lse_ok(void);            /* 1 si el LSE oscila    */

#endif /* SYSTEM_CLOCK_H */
