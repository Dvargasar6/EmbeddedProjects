/*
 * rtc.h
 * Reloj de tiempo real interno del STM32F411, alimentado por el LSE.
 *
 * El RTC reside en el dominio de respaldo (Backup Domain), que se alimenta
 * desde VBAT cuando desaparece VDD. Si se coloca una pila de litio en VBAT
 * (jumper JP5 de la Nucleo en posicion E5V/VBAT segun el montaje), la cuenta
 * de fecha y hora sobrevive a la desconexion de la alimentacion principal.
 *
 * Para no reinicializar la hora en cada arranque se emplea un registro de
 * respaldo (RTC_BKP_DR0) como testigo: si contiene la firma esperada, el RTC
 * ya estaba en marcha y solo se reanuda la lectura.
 */
#ifndef RTC_H
#define RTC_H

#include "main.h"

/* Firma arbitraria escrita en el registro de respaldo tras la primera
   inicializacion. Cualquier valor distinto de 0 sirve; se elige uno poco
   probable de aparecer por azar tras un borrado. */
#define RTC_BACKUP_SIGNATURE   0x32F2A5C3U

extern RTC_HandleTypeDef hrtc;

void rtc_init(void);                          /* configura RTC y wakeup 1 s */
void rtc_get_time(RTC_TimeTypeDef *t, RTC_DateTypeDef *d); /* lectura atomica */
void rtc_format_line(char *dst, uint8_t len); /* "HH:MM:SS DD-MM"           */

#endif /* RTC_H */
