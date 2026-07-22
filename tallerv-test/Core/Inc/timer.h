/*
 * timer.h
 * Bases de tiempo por hardware del proyecto.
 *
 *   TIM3 -> 250 ms. Su interrupcion de actualizacion conmuta el LED de PH1.
 *   TIM4 -> 100 ms. Base de tiempo del sistema: lanza la conversion del ADC
 *           y genera el evento periodico que consume la maquina de estados
 *           (refresco de LCD y desplazamiento de la linea movil).
 *
 * Ambos temporizadores cuelgan del bus APB1. Con APB1 = 50 MHz y prescaler
 * de bus distinto de 1, el reloj que llega a los timers es el doble: 100 MHz.
 */
#ifndef TIMER_H
#define TIMER_H

#include "main.h"

extern TIM_HandleTypeDef htim3;   /* manejador del temporizador del LED    */
extern TIM_HandleTypeDef htim4;   /* manejador de la base de tiempo comun  */

void timer_init(void);            /* configura y arranca TIM3 y TIM4       */

#endif /* TIMER_H */
