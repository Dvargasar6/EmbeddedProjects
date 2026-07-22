/*
 * comm.h — Enlace UART con el PC (Fase 2)
 *
 * USART2 (PA2 = TX, PA3 = RX) esta cableado internamente al ST-Link de la
 * Nucleo (puentes SB13/SB14) y aparece en el PC como /dev/ttyACM0.
 * Protocolo: lineas ASCII terminadas en '\n' o '\r', 115200 8N1.
 *
 * El modulo es autocontenido: Comm_Init() realiza toda la puesta en marcha
 * de hardware (relojes, GPIO, USART2, NVIC), siguiendo la convencion del
 * proyecto de que cada driver posee su propio *_HW_Init.
 */
#ifndef COMM_H
#define COMM_H

#include "stm32f4xx_hal.h"

/* Handle exportado por si otra parte del firmware necesita el USART2
   (por ejemplo, un printf de depuracion futuro). Definido en comm.c. */
extern UART_HandleTypeDef huart2;

/* Inicializacion completa del enlace: GPIO PA2/PA3 (AF7), USART2 a 115200
   8N1 sobre PCLK1 = 16 MHz, interrupcion de recepcion armada. */
void Comm_Init(void);

/* Devuelve un puntero a la ultima linea completa recibida (sin el
   terminador), exactamente una vez por linea; NULL si no hay linea nueva.
   Debe llamarse periodicamente desde el lazo principal. */
const char *Comm_Poll(void);

/* Transmision con formato estilo printf (bloqueante, corta).
   Uso tipico: Comm_Printf("OK PONG\r\n"); */
void Comm_Printf(const char *fmt, ...);

/* Debe llamarse desde USART2_IRQHandler() en stm32f4xx_it.c:
   delega en el manejador HAL, que a su vez invoca el callback de RX. */
void Comm_IRQHandler(void);

/* 1 si ya se recibio al menos una linea valida desde el arranque
   (usado por main para reportar "enlace establecido" en el LCD). */
uint8_t Comm_LinkSeen(void);

#endif /* COMM_H */
