# CODIGO.md — Manual de programación (guía de sustentación oral)

Este documento explica **todo el código** de `Src/main.c` y `Src/stm32f4xx_it.c` función por función, línea por línea donde aporta valor, indicando qué bloques son genéricos/reutilizables en cualquier proyecto STM32F4 con HAL y cuáles son específicos de este parcial.

---

## 1. Descripción general

El proyecto es un firmware bare-metal en C puro para una **Nucleo-F411RE**, escrito directamente sobre la librería **HAL** de STMicroelectronics (sin CubeMX: todos los `_InitTypeDef` se llenan a mano). Todo vive en **un único `main.c`**, tal como exige el enunciado.

### 1.1 Arquitectura: máquina de estados finitos (FSM)

El programa NO es un `while(1)` con `if`s sueltos: es una FSM explícita con un `enum`:

```c
typedef enum {
    STATE_IDLE = 0,      /* Esperando eventos de las ISR */
    STATE_BLINK_TOGGLE,  /* Atender el evento del TIM10 (toggle del LED) */
    STATE_LCD_REFRESH,   /* Redibujar el contenido de la pantalla LCD */
    STATE_UART_COMMAND,  /* Procesar el byte recibido por USART2 */
    STATE_ADC_UPDATE     /* Convertir el par VRX/VRY recien muestreado */
} SystemState_t;
```

**Patrón usado en todo el proyecto (reutilizable en cualquier firmware con FSM + interrupciones):**

1. Una interrupción ocurre (timer, UART, ADC, RTC).
2. El `IRQHandler` en `stm32f4xx_it.c` reenvía el evento al HAL (`HAL_TIM_IRQHandler`, etc.).
3. El HAL, internamente, invoca un **callback** (`HAL_TIM_PeriodElapsedCallback`, `HAL_UART_RxCpltCallback`, etc.), que nosotros implementamos.
4. El callback **solo levanta una bandera `volatile`** (ej. `blinkEventFlag = 1`). Nunca hace trabajo pesado ni bloqueante dentro de una ISR.
5. El bucle principal, en `STATE_IDLE`, revisa las banderas en orden de prioridad y transiciona al estado correspondiente.
6. Ese estado ejecuta el trabajo real, limpia su bandera, y vuelve a `STATE_IDLE`.

Esto se aplica a **todos los periféricos excepto el I2C** (LCD), que el enunciado permite manejar por sondeo (polling) porque el protocolo I2C del HD44780/PCF8574 es inherentemente una secuencia bloqueante corta.

### 1.2 Variables de estado global (banderas y datos compartidos)

```c
static volatile SystemState_t systemState = STATE_IDLE;
static volatile uint8_t blinkEventFlag = 0;
static volatile uint8_t lcdRefreshFlag = 0;
static volatile uint8_t uartRxByte = 0;
static volatile uint8_t uartRxFlag = 0;
static volatile int16_t coordX = 0;
static volatile int16_t coordY = 0;
static volatile uint16_t adcRawX = 0;
static volatile uint16_t adcRawY = 0;
static volatile uint8_t adcSampleFlag = 0;
```

**Por qué `volatile`:** todas estas variables se escriben dentro de una ISR (o de una función invocada por una ISR) y se leen en el bucle principal. Sin `volatile`, el compilador podría optimizar la lectura asumiendo que el valor no cambia entre iteraciones del `while(1)`, y el programa se "colgaría" esperando una bandera que en los registros ya cambió pero que el compilador cacheó en un registro de CPU.

**Reutilizable:** este patrón bandera-volatile-ISR-a-bucle-principal es universal en cualquier firmware embebido basado en interrupciones sin RTOS.

### 1.3 Handles de periféricos (globales, no `static`)

```c
TIM_HandleTypeDef htim10;
I2C_HandleTypeDef hi2c1;
UART_HandleTypeDef huart2;
ADC_HandleTypeDef hadc1;
RTC_HandleTypeDef hrtc;
```

Se declaran **globales** (sin `static`) porque `stm32f4xx_it.c` los necesita vía `extern` para poder llamar a `HAL_TIM_IRQHandler(&htim10)`, etc. Este es el patrón estándar que usa CubeMX también, así que es 100% reutilizable.

---

## 2. `SystemClock_Config()` — reloj del sistema a 100 MHz + LSE

```c
static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef oscInit = { 0 };
    RCC_ClkInitTypeDef clkInit = { 0 };

    oscInit.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSE;
    oscInit.HSIState = RCC_HSI_ON;
    oscInit.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscInit.LSEState = RCC_LSE_ON;

    oscInit.PLL.PLLState = RCC_PLL_ON;
    oscInit.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscInit.PLL.PLLM = 8;
    oscInit.PLL.PLLN = 100;
    oscInit.PLL.PLLP = RCC_PLLP_DIV2;
    oscInit.PLL.PLLQ = 4;
    HAL_RCC_OscConfig(&oscInit);

    clkInit.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
            RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clkInit.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clkInit.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clkInit.APB1CLKDivider = RCC_HCLK_DIV2;
    clkInit.APB2CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clkInit, FLASH_LATENCY_3);
}
```

**Línea por línea:**

- `oscInit.OscillatorType = ... | ...`: se pueden combinar varios osciladores en una sola llamada a `HAL_RCC_OscConfig` porque HSI y LSE son físicamente independientes (no comparten registros de configuración que se pisen entre sí). Esto ahorra una segunda llamada.
- `PLLM = 8, PLLN = 100, PLLP = DIV2`: la matemática del PLL (documentada también en el comentario del código) es:
  - `VCO_in = HSI / PLLM = 16 MHz / 8 = 2 MHz` (ST recomienda 1-2 MHz de entrada al VCO para minimizar jitter).
  - `VCO_out = VCO_in * PLLN = 2 MHz * 100 = 200 MHz` (debe estar en el rango 100-432 MHz).
  - `SYSCLK = VCO_out / PLLP = 200 MHz / 2 = 100 MHz`.
- `PLLQ = 4`: solo importa si se usa USB/SDIO/RNG (no es el caso), pero el campo es obligatorio y debe quedar en un valor válido (2-15).
- `APB1CLKDivider = RCC_HCLK_DIV2`: el bus APB1 del F411 tiene un límite de **50 MHz**; a 100 MHz de HCLK hay que dividir entre 2. Si se deja en DIV1 aquí, la USART y el I2C (que cuelgan de APB1) funcionarían con un reloj fuera de especificación.
- `APB2CLKDivider = RCC_HCLK_DIV1`: APB2 sí soporta 100 MHz en el F411, no hace falta dividir.
- `FLASH_LATENCY_3`: a 100 MHz y 3.3V, la memoria flash necesita 3 ciclos de espera por acceso (tabla de la Reference Manual RM0383). Poner un valor menor causa lecturas de instrucción corruptas a esa frecuencia; ponerlo más alto solo penaliza rendimiento sin beneficio.

**Qué es reutilizable:** la fórmula del PLL y los prescalers de bus son el bloque más reutilizable de todo el proyecto — sirve para cualquier proyecto STM32F411 que necesite subir de reloj. **Qué es específico:** los valores concretos de `PLLM/PLLN/PLLP` dependen de la frecuencia de entrada (HSI 16 MHz aquí) y de la frecuencia objetivo (100 MHz aquí); para otro MCU u otra frecuencia deseada hay que recalcularlos.

**Alternativa no usada:** se pudo haber usado el HSE (si la Nucleo lo trae a 8 MHz vía el propio ST-LINK) en vez de HSI como fuente del PLL — cambiaría `PLLSource` a `RCC_PLLSOURCE_HSE` y recalcularía `PLLM`. Se prefirió HSI porque no depende de que el puente/solder bridge del HSE esté en la posición correcta en la placa.

---

## 3. Blinky — `GPIO_Blinky_Init()` + `Timer_Blinky_Init()`

```c
static void GPIO_Blinky_Init(void)
{
    GPIO_InitTypeDef gpioInit = { 0 };
    __HAL_RCC_GPIOA_CLK_ENABLE();
    gpioInit.Pin = GPIO_PIN_5;
    gpioInit.Mode = GPIO_MODE_OUTPUT_PP;
    gpioInit.Pull = GPIO_NOPULL;
    gpioInit.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOA, &gpioInit);
}
```

- `__HAL_RCC_GPIOA_CLK_ENABLE()`: **regla de oro de todo periférico STM32**: antes de tocar los registros de un periférico hay que habilitar su reloj en el bus correspondiente (aquí AHB1 para GPIOA), o toda lectura/escritura a sus registros no tiene efecto (el periférico está "apagado"). Esto se repite para cada periférico usado en el proyecto (`__HAL_RCC_GPIOB_CLK_ENABLE`, `__HAL_RCC_I2C1_CLK_ENABLE`, etc.) — **100% reutilizable, es siempre lo primero que se hace con cualquier periférico nuevo**.
- `GPIO_MODE_OUTPUT_PP`: salida push-pull (puede manejar activamente alto y bajo; contraste con open-drain, usado en I2C).
- `GPIO_SPEED_FREQ_LOW`: el slew rate del pin no necesita ser alto porque solo cambia 4 veces por segundo.

```c
static void Timer_Blinky_Init(void)
{
    __HAL_RCC_TIM10_CLK_ENABLE();
    htim10.Instance = TIM10;
    htim10.Init.Prescaler = 99999;
    htim10.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim10.Init.Period = 250;
    htim10.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim10.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    HAL_TIM_Base_Init(&htim10);

    HAL_NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);

    HAL_TIM_Base_Start_IT(&htim10);
}
```

- **Matemática del timer** (patrón reutilizable para *cualquier* timer del proyecto — se repite igual en `ADC`/`RTC` conceptualmente): el reloj del contador es `TIMCLK / (Prescaler+1)`. Aquí `TIMCLK = PCLK2 = 100 MHz` (TIM10 cuelga de APB2, y como `APB2CLKDivider = DIV1`, no hay multiplicador x2 — la regla del F4 es: *si el prescaler de un bus APB es 1, el reloj de sus timers es igual al PCLK; si el prescaler es mayor a 1, el reloj de los timers es 2×PCLK*). Con `Prescaler=99999` el contador cuenta a `100 MHz / 100000 = 1 kHz` (1 tick = 1 ms). Con `Period=250`, la interrupción de "update" ocurre cada 251 ticks ≈ 250 ms.
- `HAL_NVIC_SetPriority(..., 5, 0)`: prioridad de interrupción en el NVIC (0 = más urgente, 15 = menos urgente en este MCU con 4 bits de prioridad). El valor exacto (5) no es crítico aquí porque ningún handler se anida con otro de forma problemática, pero mantener prioridades explícitas y distintas por periférico (`TIM10=5, USART2=6, ADC=7, RTC_WKUP=8`) documenta la intención y facilita ajustar urgencias reales en un proyecto más complejo.
- `HAL_TIM_Base_Start_IT(...)`: arranca el contador **y** habilita la interrupción de update. La `_IT` al final es la convención HAL para "esta función arranca el periférico en modo interrupción" (existen también variantes `_DMA` y sin sufijo = polling).

**Callback:**

```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM10) {
        blinkEventFlag = 1;
        ADC_StartChannel(ADC_CHANNEL_0);
    }
}
```

`HAL_TIM_PeriodElapsedCallback` es una función **débil (`__weak`)** definida en la librería HAL — al redefinirla nosotros (sin `__weak`), el linker usa nuestra versión en vez de la de la librería. Esto se repite con `HAL_UART_RxCpltCallback`, `HAL_ADC_ConvCpltCallback` y `HAL_RTCEx_WakeUpTimerEventCallback` — **es el mecanismo central de "hook" que expone la librería HAL para todos sus periféricos, totalmente reutilizable**.

El `if (htim->Instance == TIM10)` es necesario porque, si el proyecto tuviera más de un timer con interrupción de update habilitada, **todos** comparten la misma función de callback; hay que filtrar por instancia.

Aquí también se aprovecha el mismo tick de 250 ms para arrancar el muestreo del joystick (`ADC_StartChannel`), evitando declarar un timer adicional solo para eso.

---

## 4. LCD 20x4 por I2C (backpack PCF8574)

### 4.1 `I2C1_Init()`

```c
static void I2C1_Init(void)
{
    GPIO_InitTypeDef gpioInit = { 0 };
    __HAL_RCC_GPIOB_CLK_ENABLE();

    gpioInit.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpioInit.Mode = GPIO_MODE_AF_OD;
    gpioInit.Pull = GPIO_PULLUP;
    gpioInit.Speed = GPIO_SPEED_FREQ_HIGH;
    gpioInit.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(GPIOB, &gpioInit);

    __HAL_RCC_I2C1_CLK_ENABLE();

    hi2c1.Instance = I2C1;
    hi2c1.Init.ClockSpeed = 100000;
    hi2c1.Init.DutyCycle = I2C_DUTYCYCLE_2;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    HAL_I2C_Init(&hi2c1);
}
```

- `GPIO_MODE_AF_OD`: **open-drain**, no push-pull. El bus I2C solo puede tirar a GND activamente; el nivel alto lo dan las resistencias pull-up (aquí se activan también las internas del STM32 con `Pull = GPIO_PULLUP`, aunque casi todos los módulos backpack ya traen sus propias pull-ups externas). Usar push-pull en un bus I2C causaría cortocircuitos si dos dispositivos intentan poner niveles distintos al mismo tiempo.
- `Alternate = GPIO_AF4_I2C1`: cada pin del STM32F4 puede enrutarse a distintas funciones de periférico mediante un número de "función alternativa" (AF0-AF15); ese número específico (AF4 para I2C1 en PB8/PB9) sale de la tabla de "Alternate function mapping" del datasheet del F411 — **hay que consultarla para cada pin/periférico nuevo, no es un valor memorizable**.
- `ClockSpeed = 100000`: modo estándar I2C (100 kHz). El HAL calcula automáticamente los registros de temporización (`CCR`, `TRISE`) a partir de esto y del `PCLK1` real, así que sigue funcionando aunque cambie la frecuencia de bus (ver Paso 5).

### 4.2 Driver del LCD (protocolo HD44780 en modo 4 bits, sobre el expansor PCF8574)

```c
#define LCD_I2C_ADDR   (0x21 << 1)
#define LCD_BIT_RS         0x01
#define LCD_BIT_RW         0x02
#define LCD_BIT_EN         0x04
#define LCD_BIT_BACKLIGHT  0x08
```

El backpack PCF8574 es un expansor de 8 bits de I/O controlado por I2C: cada uno de sus 8 pines de salida se mapea a una señal del LCD. El mapeo `P0=RS, P1=RW, P2=EN, P3=Backlight, P4-P7=D4-D7` es el **estándar de facto** de estos módulos genéricos (no es una decisión de este proyecto, viene fijo en el hardware del backpack).

`LCD_I2C_ADDR = (0x21 << 1)`: la HAL de STM32 espera la dirección I2C ya desplazada un bit a la izquierda (el bit 0 lo usa internamente para R/W). `0x21` es la dirección de 7 bits que se determinó por prueba en este módulo particular (los backpacks PCF8574 comunes vienen en `0x20`-`0x27` según sus pines de dirección A0-A2).

```c
static void LCD_WriteNibble(uint8_t value, uint8_t rs)
{
    uint8_t base = value | LCD_BIT_BACKLIGHT | (rs ? LCD_BIT_RS : 0);
    LCD_I2C_Write(base | LCD_BIT_EN);
    LCD_I2C_Write(base);
}
```

El HD44780 lee sus líneas de datos **en el flanco de bajada de EN** (Enable). Por eso cada nibble se envía en dos escrituras I2C: una con `EN=1` (los datos ya están estables en las líneas) y otra con `EN=0` inmediatamente después (dispara la lectura interna del LCD). Como el modo es de 4 bits, cada byte real (`LCD_WriteByte`) se parte en nibble alto + nibble bajo.

```c
static void LCD_SetCursor(uint8_t row, uint8_t col)
{
    static const uint8_t rowOffset[4] = { 0x00, 0x40, 0x14, 0x54 };
    LCD_SendCommand(0x80 | (rowOffset[row] + col));
}
```

Los offsets `0x00, 0x40, 0x14, 0x54` son las direcciones de memoria DDRAM donde empieza cada una de las 4 filas en un LCD de 20 columnas con controlador HD44780 — **son un dato fijo del controlador, no del proyecto** (para un LCD 16x2 serían solo `0x00` y `0x40`).

`LCD_Init()` reproduce la secuencia de arranque en modo 4 bits del datasheet del HD44780: 3 pulsos de "reset por software" (`0x30`), luego selección de modo 4 bits (`0x20`), luego function set / display on / entry mode / clear, cada uno con su tiempo de espera mínimo documentado.

**Reutilizable:** todo este bloque (`LCD_I2C_Write` hasta `LCD_Init`) es un driver genérico de LCD HD44780+PCF8574 por I2C, se puede copiar tal cual a cualquier otro proyecto STM32 que use el mismo módulo, solo ajustando `LCD_I2C_ADDR` a la dirección real del backpack usado.

**`LCD_Refresh()`** es la única función específica del proyecto en esta sección: decide *qué* contenido va en cada una de las 4 líneas (hora/fecha del RTC, nombre fijo, y las coordenadas).

---

## 5. UART (USART2) — comandos

### 5.1 `UART2_Init()`

```c
static void UART2_Init(void)
{
    GPIO_InitTypeDef gpioInit = { 0 };
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpioInit.Pin = GPIO_PIN_2 | GPIO_PIN_3;
    gpioInit.Mode = GPIO_MODE_AF_PP;
    gpioInit.Pull = GPIO_PULLUP;
    gpioInit.Speed = GPIO_SPEED_FREQ_HIGH;
    gpioInit.Alternate = GPIO_AF7_USART2;
    HAL_GPIO_Init(GPIOA, &gpioInit);

    __HAL_RCC_USART2_CLK_ENABLE();

    huart2.Instance = USART2;
    huart2.Init.BaudRate = 115200;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX_RX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    HAL_UART_Init(&huart2);

    HAL_NVIC_SetPriority(USART2_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);

    HAL_UART_Receive_IT(&huart2, (uint8_t *) &uartRxByte, 1);
}
```

- USART2 en PA2/PA3 (`AF7`) es, en las Nucleo-64, el mismo puerto serie que sale por el cable USB a través del chip ST-LINK (VCP: Virtual COM Port) — no requiere ningún cable adicional, solo abrir un terminal serial en la PC.
- `HAL_UART_Receive_IT(&huart2, &uartRxByte, 1)`: arma la recepción de **un solo byte** por interrupción. Cuando llega, dispara la interrupción de USART2, el HAL invoca el callback, y ahí se debe volver a llamar `HAL_UART_Receive_IT` para "rearmar" la siguiente recepción (si no se rearma, solo se recibe un byte en toda la vida del programa).

### 5.2 Callback + procesamiento

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        uartRxFlag = 1;
        HAL_UART_Receive_IT(&huart2, (uint8_t *) &uartRxByte, 1);
    }
}
```

Nótese que el rearme (`HAL_UART_Receive_IT`) ocurre **dentro de la ISR/callback**, no en el bucle principal — esto es intencional: si se rearmara solo en `STATE_UART_COMMAND`, un segundo byte que llegara mientras el primero aún no fue procesado por la FSM se perdería (el periférico no tendría un buffer armado esperándolo).

```c
static void UART_ProcessCommand(void)
{
    MCO_HandleCommand((char) uartRxByte);

    switch (uartRxByte) {
    case '+': coordX += COORD_STEP; break;
    case '-': coordX -= COORD_STEP; break;
    case 'p': coordY += COORD_STEP; break;
    case 'm': coordY -= COORD_STEP; break;
    case '0': coordX = 0; coordY = 0; break;
    default: break;
    }

    uartRxFlag = 0;
    lcdRefreshFlag = 1;
}
```

Cada byte recibido se evalúa por **dos mecanismos independientes y no excluyentes**: el `switch` de comandos de una letra, y `MCO_HandleCommand` (comandos de 3 letras, ver sección 7). Un mismo carácter (p. ej. `'p'`) puede disparar el comando de una letra ("sube Y") sin afectar el detector de 3 letras, y viceversa.

**Reutilizable:** el patrón "1 byte por interrupción + rearme en el callback + bandera al bucle principal" es el estándar para cualquier protocolo de UART orientado a comandos cortos. **Específico del proyecto:** el significado de cada tecla.

**Alternativa no usada:** `HAL_UART_Receive_IT` con un buffer de tamaño >1 (para recibir un comando completo de una vez) — no se usó porque los comandos de un solo carácter deben reaccionar de inmediato sin esperar un delimitador, y los comandos de 3 letras se resolvieron con una ventana deslizante de 3 bytes en vez de un buffer con terminador.

---

## 6. Joystick por ADC1 (VRX = PA0, VRY = PA1)

### 6.1 `ADC_Joystick_Init()`

```c
static void ADC_Joystick_Init(void)
{
    GPIO_InitTypeDef gpioInit = { 0 };
    __HAL_RCC_GPIOA_CLK_ENABLE();

    gpioInit.Pin = GPIO_PIN_0 | GPIO_PIN_1;
    gpioInit.Mode = GPIO_MODE_ANALOG;
    gpioInit.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpioInit);

    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.ScanConvMode = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.DMAContinuousRequests = DISABLE;
    HAL_ADC_Init(&hadc1);

    HAL_NVIC_SetPriority(ADC_IRQn, 7, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);
}
```

- `GPIO_MODE_ANALOG`: desconecta el pin de toda la lógica digital del GPIO (buffers Schmitt-trigger, pull-ups) para minimizar consumo y ruido en la medición analógica. Es obligatorio para cualquier pin usado como entrada de ADC.
- `ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4`: el ADC del F4 tiene un límite de **36 MHz**. Con `PCLK2 = 100 MHz` (Paso 5), dividir entre 4 da 25 MHz (seguro); dividir entre 2 (lo usado antes del Paso 5, cuando `PCLK2` era menor) daría 50 MHz, **fuera de especificación** — es un detalle que hay que re-revisar cada vez que cambia el reloj del sistema.
- `ExternalTrigConv = ADC_SOFTWARE_START`: la conversión se dispara por software (`HAL_ADC_Start_IT`), no por un evento de timer. Se decidió así para controlar manualmente el "ping-pong" entre los dos canales (ver abajo) sin la complejidad de DMA/multicanal en modo scan.

### 6.2 Muestreo alternado sin DMA ("ping-pong")

El ADC1 del F411 tiene un solo conversor físico compartido entre canales: para leer dos señales (VRX y VRY) sin DMA, se alternan las conversiones:

```c
static void ADC_StartChannel(uint32_t channel)
{
    ADC_ChannelConfTypeDef sConfig = { 0 };
    adcSamplingX = (channel == ADC_CHANNEL_0) ? 1 : 0;
    sConfig.Channel = channel;
    sConfig.Rank = 1;
    sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
    HAL_ADC_ConfigChannel(&hadc1, &sConfig);
    HAL_ADC_Start_IT(&hadc1);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;
    if (adcSamplingX) {
        adcRawX = HAL_ADC_GetValue(hadc);
        ADC_StartChannel(ADC_CHANNEL_1);   /* Encadena VRY */
    } else {
        adcRawY = HAL_ADC_GetValue(hadc);
        adcSampleFlag = 1;                  /* Par completo */
    }
}
```

Cada 250 ms (desde `HAL_TIM_PeriodElapsedCallback`), se arranca VRX (`ADC_CHANNEL_0`). Cuando esa conversión termina, el callback la guarda y **inmediatamente reconfigura y arranca VRY** (`ADC_CHANNEL_1`) — encadenando dos conversiones por interrupción sin bloquear el bucle principal en ningún momento. Cuando VRY también termina, se marca el par como listo con `adcSampleFlag`.

**Alternativa no usada:** modo *scan* con DMA (`hadc1.Init.ScanConvMode = ENABLE`, más un `DMA_HandleTypeDef`), que dispararía ambas conversiones automáticamente en cada trigger y las depositaría en un arreglo en RAM sin intervención de la CPU. Es la opción "de libro" para leer varios canales ADC, pero se evitó aquí para no introducir un periférico adicional (DMA) por solo 2 canales de una señal que se muestrea 4 veces por segundo — el ping-pong por interrupción cumple igual sin esa complejidad.

### 6.3 Zona muerta y control incremental

```c
#define JOYSTICK_DEADZONE_LOW_PCT   40
#define JOYSTICK_DEADZONE_HIGH_PCT  60
#define JOYSTICK_STEP               1

static void ADC_ApplyAxis(volatile int16_t *coord, uint16_t adcRaw)
{
    uint32_t percent = (uint32_t) adcRaw * 100U / 4095U;
    if (percent < JOYSTICK_DEADZONE_LOW_PCT) {
        *coord -= JOYSTICK_STEP;
    } else if (percent > JOYSTICK_DEADZONE_HIGH_PCT) {
        *coord += JOYSTICK_STEP;
    }
}
```

**Decisión de diseño importante:** el joystick **no** reporta su posición absoluta (no se muestra "raw*100/4095" directamente). Se decidió que se comportara como control incremental, igual que los comandos UART, porque ambos escriben las mismas variables `coordX`/`coordY` mostradas en el LCD. La "zona muerta" (40%-60%) existe porque, en la práctica, el reposo mecánico/eléctrico de un joystick analógico barato casi nunca cae exactamente en el 50% del rango del ADC; sin zona muerta, el valor derivaría solo por ruido incluso con el joystick quieto.

**Reutilizable:** el patrón zona-muerta + control incremental es estándar para cualquier joystick/potenciómetro usado como control relativo (por ejemplo, volumen, brillo) en vez de posición absoluta. **Específico:** los porcentajes exactos de la zona muerta y el paso (`JOYSTICK_STEP`) se ajustaron por prueba con el módulo físico usado.

---

## 7. RTC (LSE) + MCO1

### 7.1 `RTC_Init()` — inicialización y siembra de fecha/hora

```c
static void RTC_Init(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();

    if (__HAL_RCC_GET_RTC_SOURCE() != RCC_RTCCLKSOURCE_LSE) {
        __HAL_RCC_BACKUPRESET_FORCE();
        __HAL_RCC_BACKUPRESET_RELEASE();
        __HAL_RCC_RTC_CONFIG(RCC_RTCCLKSOURCE_LSE);
    }
    __HAL_RCC_RTC_ENABLE();

    hrtc.Instance = RTC;
    hrtc.Init.HourFormat = RTC_HOURFORMAT_24;
    hrtc.Init.AsynchPrediv = 127;
    hrtc.Init.SynchPrediv = 255;
    ...
    HAL_RTC_Init(&hrtc);
    ...
}
```

- `HAL_PWR_EnableBkUpAccess()`: por defecto, el "dominio de respaldo" (RTC + registros BKP) está protegido contra escritura para evitar corrupción accidental de la hora. Hay que habilitar explícitamente el acceso antes de tocar cualquier registro del RTC — **paso obligatorio y siempre igual, 100% reutilizable**.
- `__HAL_RCC_GET_RTC_SOURCE() != LSE`: el dominio de respaldo, una vez configurado, sobrevive a resets normales del MCU (NRST) porque tiene su propio dominio de alimentación. Si ya estaba en LSE (de un boot anterior con la placa siempre energizada), **no** hay que resetearlo — hacerlo borraría la hora que ya llevaba corriendo. Solo se fuerza el reset del dominio de respaldo (`BACKUPRESET_FORCE/RELEASE`) la primera vez o después de una pérdida real de alimentación (cuando la fuente ya está en "ninguna").
- `AsynchPrediv = 127, SynchPrediv = 255`: el LSE da 32768 Hz; el RTC necesita dividir eso a exactamente 1 Hz para que el calendario avance en segundos reales. La división es `(AsynchPrediv+1) * (SynchPrediv+1) = 128 * 256 = 32768`. Estos valores son los recomendados por ST específicamente para una fuente de 32.768 kHz — **si se cambiara a LSI (~32 kHz pero impreciso) los valores serían distintos y el reloj no sería exacto**.

### 7.2 Siembra de hora inicial (solo primera vez)

```c
if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_INIT_MAGIC) {
    ...
    sscanf(__DATE__, "%3s %d %d", monStr, &day, &year);
    sscanf(__TIME__, "%d:%d:%d", &hour, &minute, &second);
    ...
    timeStatus = HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    dateStatus = HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
    if (timeStatus == HAL_OK && dateStatus == HAL_OK) {
        HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_INIT_MAGIC);
    }
}
```

El MCU no tiene ninguna forma de conocer la hora real por sí mismo (no hay red, GPS, ni RTC ya corriendo en el primer arranque). La solución usada es sembrar la hora con **`__DATE__`/`__TIME__`**, dos macros que el preprocesador de C reemplaza automáticamente con la fecha y hora de compilación del archivo (ej. `"Jul 22 2026"`, `"21:58:03"`). Es una aproximación (la hora real de flasheo es unos segundos/minutos después de la compilación), pero es la mejor disponible sin hardware adicional (un módulo RTC con batería propia, GPS, o que el usuario teclee la hora por UART).

`RTC_BKP_DR0` es uno de los **registros de respaldo** del RTC (memoria de unos pocos bytes que sobrevive resets mientras el dominio de respaldo tenga alimentación). Se usa como bandera de "¿ya sembré la hora alguna vez?": se escribe un valor "mágico" (`0x32F3`) después de sembrar con éxito, y en cada arranque se compara — si coincide, **no** se vuelve a sembrar (el RTC ya viene corriendo con la hora real acumulada); si no coincide (primer arranque, o el dominio de respaldo se reinició por pérdida total de alimentación), se vuelve a sembrar con la hora de compilación.

**Importante:** la siembra solo se marca como exitosa (`BKUPWrite`) si **ambas** escrituras (`SetTime` y `SetDate`) devolvieron `HAL_OK`. Esto evita quedar "marcado como sembrado" con una hora que en realidad nunca se escribió (por ejemplo, si el LSE aún no estaba completamente estable en un arranque en frío).

### 7.3 WakeUp Timer del RTC — refresco del LCD por interrupción, no por sondeo

```c
HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 8, 0);
HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);
```

```c
void HAL_RTCEx_WakeUpTimerEventCallback(RTC_HandleTypeDef *hrtc_cb)
{
    if (hrtc_cb->Instance == RTC) {
        lcdRefreshFlag = 1;
    }
}
```

`RTC_WAKEUPCLOCK_CK_SPRE_16BITS` con contador en `0` es un modo especial del RTC en el que el WakeUp Timer se sincroniza con el propio prescaler del calendario (`ck_spre`, que ya es 1 Hz por construcción, ver 7.1): el resultado es una interrupción **exactamente cada 1 segundo**, sin tener que calcular manualmente ningún prescaler adicional. Esto permite refrescar la hora en el LCD reaccionando a una interrupción real del RTC (cumpliendo el requisito de "todos los periféricos por interrupción") en vez de sondear `HAL_RTC_GetTime` constantemente desde el timer de blinky.

### 7.4 Lectura de hora/fecha — `RTC_FormatString()`

```c
static void RTC_FormatString(char *buf, size_t len)
{
    RTC_TimeTypeDef sTime = { 0 };
    RTC_DateTypeDef sDate = { 0 };
    HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN);
    snprintf(buf, len, "%02d:%02d:%02d %02d-%02d-%02d",
            sTime.Hours, sTime.Minutes, sTime.Seconds,
            sDate.Date, sDate.Month, sDate.Year);
}
```

**Detalle no obvio y fácil de olvidar:** el RTC del STM32F4 tiene registros "shadow" (copias sincronizadas de los registros de calendario reales, para que una lectura no capture un instante intermedio de un segundo cambiando a otro). La HAL exige leer **siempre** `HAL_RTC_GetDate` inmediatamente después de `HAL_RTC_GetTime` (aunque no se necesite la fecha en ese momento) porque es la llamada a `GetDate` la que desbloquea/libera esos registros shadow para la siguiente lectura. Omitir la llamada a `GetDate` puede hacer que la hora se quede "congelada" en las siguientes lecturas.

### 7.5 MCO1 — verificación de reloj en PA8

```c
static void MCO1_Init(void)
{
    ...
    gpioInit.Alternate = GPIO_AF0_MCO;
    HAL_GPIO_Init(GPIOA, &gpioInit);
    __HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_4);
}

static void MCO_HandleCommand(char newChar)
{
    static char cmdBuf[3] = { 0, 0, 0 };
    cmdBuf[0] = cmdBuf[1];
    cmdBuf[1] = cmdBuf[2];
    cmdBuf[2] = newChar;

    if (strncmp(cmdBuf, "HSI", 3) == 0) {
        __HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
    } else if (strncmp(cmdBuf, "LSE", 3) == 0) {
        __HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_LSE, RCC_MCODIV_1);
    } else if (strncmp(cmdBuf, "PLL", 3) == 0) {
        __HAL_RCC_MCO1_CONFIG(RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_4);
    }
}
```

`PA8` es, en el STM32F4, el **único** pin físico conectado a la salida MCO1 (Microcontroller Clock Output 1) — no es una elección del proyecto, es fija por el silicio del chip.

**Detector de comandos de 3 letras sin buffer de línea completa:** en vez de acumular bytes hasta un delimitador (como `\n`), se mantiene una **ventana deslizante** de los últimos 3 caracteres recibidos (`cmdBuf`), que se compara contra `"HSI"`, `"LSE"`, `"PLL"` después de cada byte nuevo. Esto significa que el usuario puede escribir el comando en cualquier terminal, sin necesitar configurarlo para enviar un terminador de línea — apenas el tercer carácter coincide, se ejecuta.

**Prescaler de MCO1 por fuente:** HSI (16 MHz) y LSE (32.768 kHz) se enrutan directo (`RCC_MCODIV_1`) porque ya son frecuencias bajas y seguras para el pin. El PLL, en cambio, corre a 100 MHz — muy por encima de lo recomendable para una salida de propósito general — así que se usa `RCC_MCODIV_4` (100 MHz / 4 = 25 MHz), el mejor equilibrio entre "seguir viendo una señal claramente distinguible del PLL" y "no exceder el ancho de banda de salida típico de un pin GPIO estándar del F4".

**Reutilizable:** todo el patrón de PLL/MCO es genérico; **específico del proyecto** es el mapeo exacto de comandos de texto a fuentes de reloj.

---

## 8. `stm32f4xx_it.c` — manejadores de interrupción

```c
extern TIM_HandleTypeDef htim10;
extern UART_HandleTypeDef huart2;
extern ADC_HandleTypeDef hadc1;
extern RTC_HandleTypeDef hrtc;

void SysTick_Handler(void)          { HAL_IncTick(); }
void TIM1_UP_TIM10_IRQHandler(void) { HAL_TIM_IRQHandler(&htim10); }
void USART2_IRQHandler(void)        { HAL_UART_IRQHandler(&huart2); }
void ADC_IRQHandler(void)           { HAL_ADC_IRQHandler(&hadc1); }
void RTC_WKUP_IRQHandler(void)      { HAL_RTCEx_WakeUpTimerIRQHandler(&hrtc); }
```

Este archivo es deliberadamente **delgado**: cada `..._IRQHandler` (nombre fijo, definido en el archivo de arranque `startup_stm32f411retx.s` como parte de la tabla de vectores) solo reenvía el evento a la función genérica del HAL correspondiente (`HAL_xxx_IRQHandler`), que internamente decodifica qué bandera de estado se activó y llama al callback débil apropiado (los que están en `main.c`).

**Detalles no obvios:**
- `SysTick_Handler`: es requerido por **toda** aplicación HAL, sin excepción — `HAL_IncTick()` incrementa el contador de milisegundos que usan `HAL_Delay()` y todos los timeouts internos del HAL (incluyendo, por ejemplo, el timeout de arranque del LSE). Si se omite, `HAL_Delay` nunca retorna y cualquier función HAL con timeout falla.
- `TIM1_UP_TIM10_IRQHandler`: el nombre del vector no es `TIM10_IRQHandler` — en el STM32F411, TIM10 **comparte** vector de interrupción con la actualización de TIM1 (`TIM1_UP_TIM10_IRQn`), porque son timers pequeños de propósito específico y ST decidió ahorrar líneas de interrupción en el NVIC. Este es un detalle que **hay que verificar en la tabla de vectores del `startup_*.s`** para cada timer usado, no es un patrón universal (TIM2, por ejemplo, sí tiene su propio vector `TIM2_IRQHandler`).
- `RTC_WKUP_IRQHandler`: internamente asociado a la línea 22 de EXTI; `HAL_RTCEx_WakeUpTimerIRQHandler` se encarga de limpiar tanto el flag del RTC como el de EXTI, no hace falta tocar el EXTI manualmente.

**100% reutilizable:** la estructura completa de este archivo (un `IRQHandler` mínimo por periférico, reenviando al HAL) es el patrón estándar en cualquier proyecto STM32+HAL, generado también automáticamente por CubeMX. Lo único que cambia de un proyecto a otro es *qué* periféricos aparecen aquí.

---

## 9. Resumen: qué copiar igual vs. qué adaptar en un proyecto nuevo

| Bloque | Reutilizable tal cual | Hay que adaptar |
|---|---|---|
| Patrón FSM + banderas volatile | Sí, la estructura completa | Los estados y qué evento dispara cada uno |
| `__HAL_RCC_xxx_CLK_ENABLE()` antes de cada periférico | Sí | Cuál periférico/puerto según el proyecto |
| Fórmulas de PLL y prescalers de timer | Sí, la fórmula | Los valores numéricos (dependen de reloj fuente y objetivo) |
| Driver LCD HD44780+PCF8574 (`LCD_*`) | Sí, el driver completo | Solo `LCD_I2C_ADDR` (dirección del backpack usado) |
| UART 1 byte + rearme en callback | Sí | El significado de cada comando |
| Ping-pong ADC sin DMA | Sí, la técnica | Qué canales y qué se hace con cada valor |
| RTC: siembra con `__DATE__`/`__TIME__` + magic en BKP_DR0 | Sí | — |
| MCO1 + comandos de texto | Sí, la técnica de ventana deslizante | Qué fuentes/comandos se soportan |
| `stm32f4xx_it.c` | Sí, la estructura | Los `IRQHandler` concretos según periféricos usados (revisar `startup_*.s`) |
| Zona muerta / control incremental del joystick | Sí, la técnica | Umbrales y paso, específicos del hardware físico |
