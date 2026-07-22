/*
 * uart.c
 * Comunicacion serie por interrupciones y traduccion de comandos a eventos.
 *
 * Comandos aceptados (un solo caracter, sin necesidad de pulsar Enter):
 *   c   pone ambas coordenadas a cero
 *   x   activa el modo X
 *   y   activa el modo Y
 *   +   incrementa la coordenada del modo activo
 *   -   decrementa la coordenada del modo activo
 *   h   enruta el HSI a la salida MCO1 (PA8)
 *   l   enruta el LSE a la salida MCO1
 *   p   enruta el PLL a la salida MCO1
 *   d   informe de diagnostico de los perifericos
 *   s   sondeo completo del bus I2C
 *   ?   imprime la ayuda
 */
#include "uart.h"
#include "fsm.h"

UART_HandleTypeDef huart2;

/* Byte donde la HAL deposita cada caracter recibido */
static volatile uint8_t s_rx_byte;

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
