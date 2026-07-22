/*
 * i2c_lcd.h
 * Pantalla LCD alfanumerica de 16x4 con controlador HD44780 gobernada a
 * traves de un expansor de puertos PCF8574 sobre el bus I2C1.
 *
 * Conexionado por defecto (LCD_USE_PB6_PB7 sin definir):
 *   SCL -> PB8  (D15 en el conector Arduino de la Nucleo)
 *   SDA -> PB9  (D14)
 *
 * Conexionado alternativo (definir LCD_USE_PB6_PB7 en el Makefile con
 * -DLCD_USE_PB6_PB7): SCL -> PB6 (D10), SDA -> PB7. Es el otro mapeo habitual
 * de I2C1 y el que usan muchos proyectos previos; se incluye para no tener
 * que recablear el montaje.
 *
 *   VCC -> 5V   (el HD44780 no funciona de forma fiable a 3.3 V)
 *   GND -> GND
 *
 * Correspondencia de bits del PCF8574 hacia el HD44780:
 *   P0 -> RS      P4 -> D4
 *   P1 -> RW      P5 -> D5
 *   P2 -> EN      P6 -> D6
 *   P3 -> luz     P7 -> D7
 */
#ifndef I2C_LCD_H
#define I2C_LCD_H

#include "main.h"

#define LCD_COLS   20U
#define LCD_ROWS    4U

extern I2C_HandleTypeDef hi2c1;

void     lcd_init(void);                            /* bus + controlador     */
void     lcd_clear(void);                           /* borra la pantalla     */
void     lcd_set_cursor(uint8_t row, uint8_t col);  /* posiciona el cursor   */
void     lcd_print(const char *s);                  /* escribe una cadena    */
void     lcd_print_line(uint8_t row, const char *s);/* linea completa        */

/* --- Diagnostico -------------------------------------------------------- */
uint8_t  lcd_is_present(void);   /* 1 si algun expansor respondio al sondeo  */
uint8_t  lcd_address(void);      /* direccion de 7 bits detectada            */
uint32_t lcd_error_count(void);  /* transacciones I2C fallidas acumuladas    */
void     lcd_bus_scan_report(void); /* recorre el bus e informa por UART     */

#endif /* I2C_LCD_H */
