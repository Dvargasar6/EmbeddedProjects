/**
 * lcd_i2c.c - LCD1602 (3.3 V variant) behind a PCF8574 I2C backpack.
 *
 * Wiring (Nucleo-F411RE): SCL = PB8 (D15, AF4), SDA = PB9 (D14, AF4).
 * The module owns its own hardware bring-up (LCD_HW_Init): GPIO clocks,
 * open-drain alternate-function pins, I2C1 configuration, and a bus scan
 * that resolves the 0x27-vs-0x3F backpack address at runtime.
 *
 * PCF8574 -> HD44780 mapping (the de-facto standard backpack):
 *   P7..P4 = D7..D4   (data nibble)
 *   P3     = backlight
 *   P2     = E  (enable strobe)
 *   P1     = RW (kept 0 = write)
 *   P0     = RS (0 = command, 1 = data)
 */

#include "lcd_i2c.h"
#include <string.h>

I2C_HandleTypeDef hi2c1;              /* exported: the handle lives here */

static uint8_t lcd_addr8 = 0;         /* detected address, already shifted for HAL (8-bit form) */
static const uint8_t BACKLIGHT = 0x08;

/* ------------------------------------------------------------------ */
/* Hardware bring-up (what CubeMX would have generated, written by hand) */
/* ------------------------------------------------------------------ */
HAL_StatusTypeDef LCD_HW_Init(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();     /* pin port clock */
    __HAL_RCC_I2C1_CLK_ENABLE();      /* peripheral clock (APB1) */

    /* PB8 = SCL, PB9 = SDA: alternate function 4, open-drain as I2C requires.
       Internal pull-ups enabled as a safety net; the backpack normally has 4.7k. */
    g.Pin       = GPIO_PIN_8 | GPIO_PIN_9;
    g.Mode      = GPIO_MODE_AF_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &g);

    hi2c1.Instance             = I2C1;
    hi2c1.Init.ClockSpeed      = 100000;                  /* standard mode 100 kHz */
    hi2c1.Init.DutyCycle       = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1     = 0;                       /* master only */
    hi2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK) return HAL_ERROR;

    /* Address detection: try the two common backpack addresses first,
       then sweep the whole 7-bit range as a fallback. */
    const uint8_t candidates[] = { 0x27, 0x3F };
    for (unsigned i = 0; i < sizeof(candidates); i++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(candidates[i] << 1), 2, 10) == HAL_OK) {
            lcd_addr8 = (uint8_t)(candidates[i] << 1);
            return HAL_OK;
        }
    }
    for (uint8_t a = 0x08; a < 0x78; a++) {
        if (HAL_I2C_IsDeviceReady(&hi2c1, (uint16_t)(a << 1), 1, 5) == HAL_OK) {
            lcd_addr8 = (uint8_t)(a << 1);
            return HAL_OK;
        }
    }
    return HAL_ERROR;                 /* nothing on the bus: check wiring/power */
}

uint8_t LCD_GetAddr7(void) { return (uint8_t)(lcd_addr8 >> 1); }

/* ------------------------------------------------------------------ */
/* HD44780 protocol over the PCF8574                                   */
/* ------------------------------------------------------------------ */

/* Send one nibble with the Enable pulse required by the HD44780 */
static void nib(uint8_t nibble, uint8_t rs)
{
    uint8_t d = (uint8_t)((nibble & 0xF0) | BACKLIGHT | (rs ? 0x01 : 0x00));
    uint8_t seq[2] = { (uint8_t)(d | 0x04), d };   /* E high, then E low latches the nibble */
    HAL_I2C_Master_Transmit(&hi2c1, lcd_addr8, &seq[0], 1, 10);
    HAL_I2C_Master_Transmit(&hi2c1, lcd_addr8, &seq[1], 1, 10);
}

/* Full byte as two nibbles: high first, then low */
static void byte(uint8_t b, uint8_t rs)
{
    nib(b & 0xF0, rs);
    nib((uint8_t)(b << 4), rs);
    HAL_Delay(1);                     /* generous execution time; fine for a status display */
}

void LCD_Cmd(uint8_t c)  { byte(c, 0); }   /* RS=0: instruction register */
void LCD_Data(uint8_t d) { byte(d, 1); }   /* RS=1: character data */

void LCD_Init(void)
{
    HAL_Delay(50);                    /* HD44780 power-on wait */
    nib(0x30, 0); HAL_Delay(5);       /* wake-up sequence, three times per datasheet */
    nib(0x30, 0); HAL_Delay(1);
    nib(0x30, 0); HAL_Delay(1);
    nib(0x20, 0); HAL_Delay(1);       /* switch to 4-bit interface */
    LCD_Cmd(0x28);                    /* function set: 4-bit, 2 lines, 5x8 font */
    LCD_Cmd(0x08);                    /* display off */
    LCD_Clear();
    LCD_Cmd(0x06);                    /* entry mode: increment, no display shift */
    LCD_Cmd(0x0C);                    /* display on, cursor off, blink off */
}

void LCD_Clear(void)
{
    LCD_Cmd(0x01);
    HAL_Delay(2);                     /* clear needs ~1.5 ms */
}

void LCD_SetCursor(uint8_t row, uint8_t col)
{
    /* DDRAM addresses: line 0 starts at 0x00, line 1 at 0x40 */
    LCD_Cmd((uint8_t)(0x80 | (row ? 0x40 : 0x00) | col));
}

void LCD_Print(const char *s)
{
    while (*s) LCD_Data((uint8_t)*s++);
}

/* Clear a full 16-char line and write s (truncated to 16) */
void LCD_Line(uint8_t row, const char *s)
{
    char buf[17];
    size_t n = strlen(s);
    if (n > 16) n = 16;
    memset(buf, ' ', 16);
    buf[16] = 0;
    memcpy(buf, s, n);
    LCD_SetCursor(row, 0);
    LCD_Print(buf);
}
