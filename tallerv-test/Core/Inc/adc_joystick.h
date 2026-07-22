/*
 * adc_joystick.h
 * Lectura del modulo joystick analogico mediante ADC1 por interrupcion.
 *
 * Conexionado del modulo:
 *   GND -> GND de la placa
 *   +5V -> 5V de la placa (el divisor resistivo del joystick entrega como
 *          maximo la tension de alimentacion; ver nota en el README sobre
 *          la alimentacion a 3.3 V para no exceder VDDA del ADC)
 *   VRX -> PA0  (ADC1_IN0)
 *   VRY -> PA1  (ADC1_IN1)
 *   SW  -> PB5  (entrada digital con interrupcion EXTI)
 */
#ifndef ADC_JOYSTICK_H
#define ADC_JOYSTICK_H

#include "main.h"

/* Umbrales sobre la escala de 12 bits (0..4095) del ADC.
   El centro mecanico del joystick queda proximo a 2048. La banda muerta
   evita que el ruido y la tolerancia del potenciometro generen movimiento. */
#define JOY_CENTER      2048U
#define JOY_DEADZONE     600U   /* +-600 cuentas alrededor del centro */

extern ADC_HandleTypeDef hadc1;

void     adc_joystick_init(void);   /* configura ADC1 y sus dos canales    */
void     adc_joystick_start(void);  /* lanza una conversion (desde TIM4)   */
uint16_t adc_joystick_raw_x(void);  /* ultima muestra cruda de VRX (0..4095) */
uint16_t adc_joystick_raw_y(void);  /* ultima muestra cruda de VRY (0..4095) */
int8_t   adc_joystick_dir_x(void);  /* -1 izquierda, 0 centro, +1 derecha  */
int8_t   adc_joystick_dir_y(void);  /* -1 abajo, 0 centro, +1 arriba       */

#endif /* ADC_JOYSTICK_H */
