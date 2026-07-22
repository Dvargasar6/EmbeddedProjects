/*
 * fsm.h
 * Maquina de estados finitos del proyecto.
 *
 * Toda la logica de aplicacion se concentra aqui. Las interrupciones no
 * ejecutan logica: unicamente encolan eventos con fsm_post_event(), y el
 * bucle principal los procesa con fsm_run(). De esta forma las rutinas de
 * interrupcion son cortas y las operaciones lentas (I2C hacia el LCD) se
 * ejecutan siempre en contexto de tarea.
 */
#ifndef FSM_H
#define FSM_H

#include "main.h"

/* --- Estados -------------------------------------------------------------
 * ST_INIT     : arranque; se muestra el mensaje de bienvenida.
 * ST_IDLE     : sin modo seleccionado; la linea 2 muestra el texto movil.
 * ST_MODE_X   : modo X activo; la linea 2 muestra "MODO X".
 * ST_MODE_Y   : modo Y activo; la linea 2 muestra "MODO Y".
 * ST_MESSAGE  : mensaje temporal en la linea 2; al expirar se vuelve al
 *               estado anterior guardado en s_prev.
 * ST_ERROR    : fallo irrecuperable de un periferico.
 * ------------------------------------------------------------------------ */
typedef enum {
    ST_INIT = 0,
    ST_IDLE,
    ST_MODE_X,
    ST_MODE_Y,
    ST_MESSAGE,
    ST_ERROR,
    ST_COUNT
} fsm_state_t;

/* --- Eventos ------------------------------------------------------------- */
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
    EV_CMD_DIAG,       /* 'd': informe de estado de los perifericos   */
    EV_CMD_SCAN,       /* 's': sondeo completo del bus I2C            */
    EV_BUTTON,         /* pulsador del joystick (EXTI)               */
    EV_TICK_100MS,     /* desbordamiento de TIM4                     */
    EV_SECOND,         /* despertador del RTC, una vez por segundo   */
    EV_COUNT
} fsm_event_t;

void        fsm_init(void);                 /* estado inicial y bienvenida  */
void        fsm_post_event(fsm_event_t ev); /* encola un evento (apto ISR)   */
void        fsm_run(void);                  /* procesa la cola; bucle main   */
fsm_state_t fsm_get_state(void);            /* estado actual                 */

#endif /* FSM_H */
