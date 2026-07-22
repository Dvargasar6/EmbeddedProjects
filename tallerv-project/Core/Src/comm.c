/*
 * comm.c — Enlace UART con el PC via USART2 / ST-Link VCP (Fase 2)
 *
 * Diseno:
 *  - Recepcion por interrupcion, byte a byte (HAL_UART_Receive_IT).
 *    La ISR acumula caracteres hasta '\n' o '\r' y publica la linea
 *    completa mediante una bandera; el lazo principal la consume con
 *    Comm_Poll(). Nada bloquea: el resto del firmware sigue corriendo.
 *  - Transmision bloqueante corta (HAL_UART_Transmit). A 115200 bps una
 *    respuesta de 32 bytes tarda ~2.8 ms; aceptable para este protocolo.
 *  - Reloj: PCLK1 = 16 MHz (HSI, sin PLL, prescalers /1). Con OVER16 el
 *    divisor para 115200 es 8.6875 -> error de baudios ~0.02 %, despreciable.
 */
#include "comm.h"
#include "board.h"    /* Error_Handler() */
#include <stdarg.h>   /* va_list, va_start, va_end */
#include <stdio.h>    /* vsnprintf */

UART_HandleTypeDef huart2;              /* handle global del USART2 */

#define LINE_MAX 64U                    /* longitud maxima de una linea de comando */

static uint8_t  rx_byte;                /* buffer de 1 byte para la recepcion IT */
static char     line[LINE_MAX];         /* linea en construccion (escrita en ISR) */
static uint8_t  idx = 0;                /* indice de escritura dentro de line[] */
static volatile uint8_t line_ready = 0; /* 1 = hay linea completa pendiente de leer */
static volatile uint8_t link_seen = 0;  /* 1 = ya llego al menos una linea */

void Comm_Init(void)
{
    /* --- 1. Relojes de los perifericos usados --- */
    __HAL_RCC_GPIOA_CLK_ENABLE();       /* PA2/PA3 viven en el puerto A */
    __HAL_RCC_USART2_CLK_ENABLE();      /* USART2 cuelga de APB1 (16 MHz) */

    /* --- 2. GPIO: PA2 = USART2_TX, PA3 = USART2_RX, funcion alternativa 7 --- */
    GPIO_InitTypeDef g = {0};
    g.Pin       = GPIO_PIN_2 | GPIO_PIN_3;
    g.Mode      = GPIO_MODE_AF_PP;      /* alternativa push-pull */
    g.Pull      = GPIO_NOPULL;          /* el ST-Link ya mantiene niveles definidos */
    g.Speed     = GPIO_SPEED_FREQ_LOW;  /* 115200 bps esta muy por debajo del limite "low" */
    g.Alternate = GPIO_AF7_USART2;      /* AF7 = USART1..3 en el F411 */
    HAL_GPIO_Init(GPIOA, &g);

    /* --- 3. USART2: 115200 8N1, TX+RX, sobremuestreo x16 --- */
    huart2.Instance          = USART2;
    huart2.Init.BaudRate     = 115200U;
    huart2.Init.WordLength   = UART_WORDLENGTH_8B;
    huart2.Init.StopBits     = UART_STOPBITS_1;
    huart2.Init.Parity       = UART_PARITY_NONE;
    huart2.Init.Mode         = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl    = UART_HWCONTROL_NONE;   /* el VCP no cablea RTS/CTS */
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();                /* definido en board.c: senaliza por el LED PH1 */
    }

    /* --- 4. NVIC: interrupcion global del USART2 ---
       Prioridad 6: por debajo (menos urgente) que el latido TIM5 (prioridad 5).
       La ISR solo mueve un byte a un buffer; su latencia no es critica. */
    HAL_NVIC_SetPriority(USART2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    /* --- 5. Armar la recepcion del primer byte; se rearma en el callback --- */
    HAL_UART_Receive_IT(&huart2, &rx_byte, 1);
}

/* Puente para stm32f4xx_it.c: el vector USART2_IRQHandler llama aqui y este
   delega en el manejador generico de HAL, que gestiona banderas y errores y
   termina invocando HAL_UART_RxCpltCallback(). */
void Comm_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart2);
}

/* Callback de HAL al completarse la recepcion de 1 byte (contexto de ISR).
   Es un callback debil (weak) de HAL: esta definicion lo reemplaza.
   Si en fases futuras otro UART usa recepcion IT (Huskylens), este callback
   debera discriminar por huart->Instance. */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        if (rx_byte == '\n' || rx_byte == '\r') {
            /* Fin de linea: publicar solo si hay contenido (ignora CRLF vacios) */
            if (idx > 0U && !line_ready) {
                line[idx] = '\0';       /* terminar la cadena */
                line_ready = 1U;        /* el lazo principal la recogera */
                link_seen  = 1U;
            }
            idx = 0U;                   /* preparar la siguiente linea */
        } else if (idx < (LINE_MAX - 1U) && !line_ready) {
            /* Acumular; si hay una linea pendiente sin consumir, se descarta
               la entrada nueva en lugar de corromper la pendiente */
            line[idx++] = (char)rx_byte;
        }
        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);   /* rearmar la recepcion */
    }
}

/* Callback de error de HAL (overrun, framing, ruido). Sin el, un error de
   overrun deja la recepcion IT muerta en silencio: aqui se limpia y rearma. */
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        idx = 0U;                                    /* descartar linea parcial */
        HAL_UART_Receive_IT(&huart2, &rx_byte, 1);   /* reanudar la recepcion */
    }
}

const char *Comm_Poll(void)
{
    if (!line_ready) {
        return NULL;
    }
    /* La ISR no toca line[] mientras line_ready == 1, de modo que el
       consumidor puede leer la cadena con seguridad antes de liberar. */
    line_ready = 0U;
    return line;
}

uint8_t Comm_LinkSeen(void)
{
    return link_seen;
}

void Comm_Printf(const char *fmt, ...)
{
    char buf[96];                       /* suficiente para las respuestas del protocolo */
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);   /* formatea con limite: nunca desborda */
    va_end(ap);
    if (n > 0) {
        if (n > (int)sizeof(buf) - 1) {
            n = (int)sizeof(buf) - 1;   /* vsnprintf devuelve lo que HABRIA escrito */
        }
        HAL_UART_Transmit(&huart2, (uint8_t *)buf, (uint16_t)n, 100U);
    }
}
