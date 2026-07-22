/*
 * main.h - Cabecera comun del proyecto.
 * Incluye la HAL y expone el manejador de errores global.
 */
#ifndef MAIN_H
#define MAIN_H

#include "stm32f4xx_hal.h"   /* API de la libreria HAL para la familia F4 */
#include <stdint.h>          /* tipos enteros de ancho fijo: uint8_t, etc. */
#include <string.h>          /* memset, strlen, strncpy                    */
#include <stdio.h>           /* snprintf para formatear cadenas            */

void Error_Handler(void);    /* bucle de error: se llama si falla una init */

#endif /* MAIN_H */
