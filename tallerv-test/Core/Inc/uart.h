/*
 * uart.h
 * USART2 a 115200 baudios, totalmente gobernada por interrupciones.
 *
 * En la Nucleo-F411RE la USART2 esta cableada al depurador ST-LINK, que la
 * expone en el ordenador como puerto virtual (/dev/ttyACM0 en Linux). No hace
 * falta ningun adaptador USB-serie externo.
 *
 *   PA2 -> USART2_TX
 *   PA3 -> USART2_RX
 *
 * La recepcion se hace byte a byte con HAL_UART_Receive_IT. La transmision usa
 * un buffer circular propio para que las funciones de envio nunca bloqueen.
 */
#ifndef UART_H
#define UART_H

#include "main.h"

#define UART_TX_BUF_SIZE  512U   /* potencia de 2 para simplificar el modulo */

extern UART_HandleTypeDef huart2;

void uart_init(void);                     /* configura USART2 y arranca RX  */
void uart_send(const char *s);            /* encola una cadena              */
void uart_send_line(const char *s);       /* cadena seguida de CR LF        */

#endif /* UART_H */
