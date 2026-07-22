#ifndef LCD_I2C_H
#define LCD_I2C_H

#include "stm32f4xx_hal.h"

/* Hardware bring-up: I2C1 on PB8/PB9 + address auto-detection.
   Returns HAL_OK if an LCD backpack answered on the bus. */
HAL_StatusTypeDef LCD_HW_Init(void);

void LCD_Init(void);                          /* HD44780 init sequence, 4-bit mode */
void LCD_Cmd(uint8_t c);
void LCD_Data(uint8_t d);
void LCD_SetCursor(uint8_t row, uint8_t col);
void LCD_Print(const char *s);
void LCD_Line(uint8_t row, const char *s);    /* clear-and-write one 16-char line */
void LCD_Clear(void);
uint8_t LCD_GetAddr7(void);                   /* detected 7-bit address, for diagnostics */

#endif
