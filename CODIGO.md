# CODIGO.md — Manual de programación

**Proyecto:** Parcial Taller V 2026-1 · STM32 Nucleo-F411RE
**Propósito:** documento de estudio para la sustentación oral.

Cada apartado sigue la misma plantilla:

1. **Qué hace** el archivo o la función.
2. **De dónde sale** cada valor, con la referencia exacta al documento de ST.
3. **El código**, comentado línea a línea.
4. **Alternativas**: qué otros parámetros o funciones existen y cuándo usarlos.
5. **Reutilización**: qué se copia tal cual y qué debe cambiarse.

---

# Parte 0 — Cómo trabajar con la documentación de ST

Antes de estudiar el código conviene entender **el método**, porque en la
sustentación la pregunta habitual no es «qué valor pusiste» sino «de dónde
sacaste ese valor».

## 0.1 Los cinco documentos y qué contiene cada uno

| Documento | Código | Se consulta para… | Ejemplo concreto en este proyecto |
|---|---|---|---|
| **Datasheet** | DS10314 | Qué función alternativa tiene cada pin; límites eléctricos; frecuencias máximas | Averiguar que I2C1_SCL está en AF4 sobre PB8 |
| **Reference Manual** | RM0383 | Qué hace cada bit de cada registro; diagramas de bloques; fórmulas | Deducir la fórmula del prescalador del RTC |
| **User Manual de la placa** | UM1724 | Puentes de soldadura, conectores, qué está cableado a qué | Saber que USART2 va al ST-LINK y no a los conectores |
| **HAL Description** | UM1725 | Firma de cada función HAL, campos de cada estructura | Conocer los valores admitidos por `ADC_InitTypeDef` |
| **Programming Manual** | PM0214 | Núcleo Cortex-M4: NVIC, excepciones, instrucciones | Entender la agrupación de prioridades del NVIC |

Regla práctica: **el datasheet dice *dónde*, el reference manual dice *cómo*,
el user manual dice *qué hay cableado* y UM1725 dice *cómo se llama en la HAL*.**

## 0.2 Procedimiento para configurar cualquier periférico

Este es el flujo que se siguió para los seis periféricos del proyecto.

**Paso 1 — Elegir el pin (Datasheet DS10314, Tabla 9 «Alternate function mapping»).**

La tabla tiene una fila por pin y una columna por función alternativa (AF0 a
AF15). Se busca el periférico deseado y se lee el pin y el número de AF.

> Ejemplo: se busca `I2C1_SCL` y aparece en la fila `PB8`, columna `AF04`.
> De ahí sale `gi.Alternate = GPIO_AF4_I2C1;` y `gi.Pin = GPIO_PIN_8;`.

**Paso 2 — Comprobar que el pin está disponible en la placa (UM1724, Tabla 11 a 17).**

El datasheet describe el chip; el user manual describe **esta** placa. Un pin
puede existir en el chip pero estar ocupado por el ST-LINK, por el LED de
usuario o no salir a ningún conector.

> Ejemplo: PA5 tiene ADC1_IN5, pero en la Nucleo está ocupado por el LED LD2.
> Por eso los ejes del joystick se llevaron a PA0 y PA1.

**Paso 3 — Verificar el bus y la frecuencia máxima (RM0383, Figura 12 «Clock tree»).**

Cada periférico cuelga de APB1 o de APB2, y cada bus tiene su límite. El
diagrama del árbol de relojes indica de dónde viene cada reloj.

> Ejemplo: el ADC cuelga de APB2 (100 MHz) pero su reloj interno no puede
> superar 36 MHz (DS10314, tabla «ADC characteristics»). De ahí el divisor
> `ADC_CLOCK_SYNC_PCLK_DIV4` que deja 25 MHz.

**Paso 4 — Leer el capítulo del periférico en RM0383 y localizar la fórmula.**

Casi toda configuración numérica sale de una fórmula del reference manual.

> Ejemplo: RM0383, sección 13.4.1, da el periodo del temporizador como
> `T = (ARR+1)(PSC+1)/f_TIM`, que es la que se usa en `timer.c`.

**Paso 5 — Traducir a la HAL (UM1725).**

UM1725 documenta cada estructura `XXX_InitTypeDef` campo por campo y cada
función `HAL_XXX_...`. Es el diccionario entre los registros del RM0383 y las
llamadas de C.

**Paso 6 — Configurar el NVIC si el periférico usa interrupciones (PM0214, cap. 4.2).**

Se necesitan tres cosas: habilitar la interrupción en el periférico
(lo hacen las funciones `_IT` de la HAL), asignarle prioridad
(`HAL_NVIC_SetPriority`) y habilitarla en el NVIC (`HAL_NVIC_EnableIRQ`).

## 0.3 Cómo encontrar el nombre exacto de una ISR

El nombre de la rutina de interrupción **no se inventa**: debe coincidir con
el símbolo declarado en `startup_stm32f411xe.s`. Si se escribe mal, el
enlazador no da error: simplemente deja en su lugar el manejador por defecto,
que es un bucle infinito, y el programa se cuelga sin explicación.

```bash
# Lista todos los nombres válidos de ISR para este microcontrolador
grep -o '[A-Za-z0-9_]*_IRQHandler' startup_stm32f411xe.s | sort -u
```

Verificación posterior al enlace: si el símbolo aparece con `T` (texto propio)
en lugar de `W` (weak, el de la tabla por defecto), la ISR está bien nombrada.

```bash
arm-none-eabi-nm build/parcial_tallerV.elf | grep TIM3_IRQHandler
# 08002618 T TIM3_IRQHandler     <- correcto
# 08000451 W TIM3_IRQHandler     <- mal escrito: se usa el manejador por defecto
```

---

# Parte 1 — Arquitectura general

## 1.1 Modelo de ejecución

El firmware sigue el patrón **productor-consumidor**:

```
  ISR (productores)                    Bucle principal (consumidor)
  ─────────────────                    ────────────────────────────
  USART2  → fsm_post_event(EV_CMD_x)
  TIM4    → fsm_post_event(EV_TICK)          fsm_run()
  EXTI5   → fsm_post_event(EV_BUTTON)   ──►    ├─ extrae evento
  RTC     → g_rtc_second_flag = 1              ├─ ejecuta transición
  TIM3    → conmuta el LED (sin cola)          └─ refresca el LCD por I2C
  ADC     → guarda la muestra
```

**Por qué esta separación.** Escribir una línea en el LCD supone unas 40
transacciones I2C con esperas de milisegundos. Si eso ocurriera dentro de una
ISR, se bloquearían todas las interrupciones de igual o menor prioridad
durante decenas de milisegundos y se perderían caracteres de la UART. La regla
es: **en la ISR, solo lo que no puede esperar; todo lo demás, en el bucle.**

## 1.2 Dependencias entre módulos

```
main.c
  ├── system_clock.c   (no depende de nadie; debe ir primero)
  ├── gpio.c
  ├── uart.c ──────────┐
  ├── adc_joystick.c ──┤
  ├── rtc.c ───────────┼──► fsm.c ──► i2c_lcd.c
  ├── i2c_lcd.c        │
  ├── timer.c ─────────┘
  └── fsm.c

stm32f4xx_it.c       depende de todos (contiene sus ISR)
stm32f4xx_hal_msp.c  depende de la HAL (pines y relojes)
```

El orden de inicialización en `main()` **no es arbitrario**:

1. `HAL_Init()` — sin ella no hay SysTick y toda función con timeout se cuelga.
2. `system_clock_init()` — los cálculos de baudios, prescalers y tiempos de
   todos los periféricos se derivan del reloj; configurarlos antes daría
   valores erróneos.
3. Periféricos.
4. `timer_init()` **al final**, porque su ISR llama a `adc_joystick_start()` y
   a `fsm_post_event()`; si arrancase antes, invocaría módulos aún sin
   inicializar.

---

# Parte 2 — Sistema de construcción

## 2.1 Makefile

**Qué hace.** Describe cómo transformar el código fuente en un binario
programable, sin depender de ningún entorno gráfico.

### El proceso completo

```
  .c  ──[arm-none-eabi-gcc -c]──►  .o  ──┐
  .s  ──[arm-none-eabi-gcc -c]──►  .o  ──┼──[ld + linker script]──► .elf
                                          │
                                          └─[objcopy]─► .hex / .bin
```

### Bloques del archivo, con explicación

```make
TARGET = parcial_tallerV        # nombre base de los archivos generados
DEBUG  = 1                      # 1 añade -g: símbolos para gdb
OPT    = -Og                    # optimización compatible con depuración
```

`-Og` es el nivel recomendado durante el desarrollo: optimiza sin reordenar el
código, de modo que los puntos de ruptura caen donde uno espera.
Alternativas: `-O0` (nada, código muy voluminoso), `-O2` (rápido),
`-Os` (mínimo tamaño, el habitual en producción).

```make
CPU       = -mcpu=cortex-m4        # núcleo concreto
FPU       = -mfpu=fpv4-sp-d16      # FPU de precisión simple, 16 registros
FLOAT-ABI = -mfloat-abi=hard       # los float viajan en registros de la FPU
MCU = $(CPU) -mthumb $(FPU) $(FLOAT-ABI)
```

**De dónde sale.** PM0214 §1.1 indica que el Cortex-M4F incorpora una FPU
FPv4-SP de precisión simple. `-mfloat-abi=hard` exige que **todo** el código,
incluidas las librerías, se compile igual; mezclar `hard` y `soft` produce un
error de enlace por incompatibilidad de ABI.

```make
C_DEFS = -DUSE_HAL_DRIVER -DSTM32F411xE
```

`STM32F411xE` selecciona, dentro de `stm32f4xx.h`, la cabecera del dispositivo
concreto (`stm32f411xe.h`), que define los mapas de memoria y los periféricos
realmente presentes. Si se pusiera `STM32F407xx`, el código compilaría pero
accedería a periféricos inexistentes.

```make
LDFLAGS = $(MCU) -specs=nano.specs -T$(LDSCRIPT) $(LIBDIR) $(LIBS) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref -Wl,--gc-sections
```

| Opción | Efecto |
|---|---|
| `-specs=nano.specs` | Usa newlib-nano; ahorra unos 30 KB frente a newlib completa |
| `-T$(LDSCRIPT)` | Impone el mapa de memoria del F411RE |
| `-Wl,-Map=...` | Genera el archivo `.map`, indispensable para saber qué ocupa memoria |
| `-Wl,--gc-sections` | Elimina funciones no referenciadas; junto a `-ffunction-sections` reduce el binario a la mitad |

```make
CFLAGS += -MMD -MP -MF"$(@:%.o=%.d)"
```

Genera archivos `.d` con las dependencias de cabeceras. Sin ellos, modificar
un `.h` **no** provocaría la recompilación de los `.c` que lo incluyen, y se
obtendrían binarios incoherentes con el código fuente.

### Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| Todo el bloque de toolchain (`PREFIX`, `CC`, `AS`, `CP`, `SZ`) | `TARGET` |
| Reglas de compilación, `vpath`, gestión de `.d` | `C_SOURCES` y `ASM_SOURCES` |
| Objetivos `clean`, `size`, `flash` | `LDSCRIPT` y el archivo de arranque |
| `-Wall -fdata-sections -ffunction-sections` | `MCU` si el núcleo es distinto (M0, M3, M7) |
| `-Wl,--gc-sections`, `-specs=nano.specs` | `C_DEFS`: la macro del dispositivo |

Para un STM32F103 (Cortex-M3, sin FPU) bastaría con:

```make
CPU = -mcpu=cortex-m3
FPU =                       # vacío: no hay FPU
FLOAT-ABI = -mfloat-abi=soft
C_DEFS = -DUSE_HAL_DRIVER -DSTM32F103xB
```

## 2.2 Linker script (`STM32F411RETX_FLASH.ld`)

**Qué hace.** Indica al enlazador qué memorias existen y en cuál colocar cada
sección del programa.

**De dónde salen los números.** RM0383 §2.3 «Memory map»: la FLASH empieza en
`0x08000000` y la SRAM en `0x20000000`. Los tamaños (512 KB y 128 KB)
corresponden al sufijo `RE` del STM32F411**RE**T6, según DS10314 §1.

```ld
_estack = 0x20020000;    /* 0x20000000 + 128K: la pila crece hacia abajo */

MEMORY
{
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
}
```

Las secciones, en orden de aparición en la FLASH:

| Sección | Contenido | Por qué está donde está |
|---|---|---|
| `.isr_vector` | Tabla de vectores | El núcleo lee `0x08000000` (pila) y `0x08000004` (Reset) al arrancar; **debe** ir primero |
| `.text` | Código | Se ejecuta desde FLASH |
| `.rodata` | `const` y cadenas literales | No cambian: no tiene sentido gastar RAM |
| `.data` | Globales inicializadas | Viven en RAM, pero su valor inicial se guarda en FLASH (`AT> FLASH`) |
| `.bss` | Globales a cero | Solo ocupan RAM; el arranque las pone a cero |

La línea clave y la más difícil de entender:

```ld
.data : { ... } >RAM AT> FLASH
```

`>RAM` fija la dirección **de ejecución** (VMA) y `AT> FLASH` la dirección **de
carga** (LMA). El símbolo `_sidata = LOADADDR(.data)` da la posición en FLASH
desde donde `Reset_Handler` copia los valores iniciales a RAM.

**Reutilización.** Solo cambian las dos líneas de `MEMORY` y el valor de
`_estack`. El resto es idéntico para cualquier Cortex-M con GCC.

## 2.3 Archivo de arranque (`startup_stm32f411xe.s`)

**Qué hace.** Es lo primero que ejecuta el microcontrolador. Contiene la tabla
de vectores y `Reset_Handler`, que:

1. Copia `.data` desde FLASH a RAM usando `_sidata`, `_sdata` y `_edata`.
2. Pone `.bss` a cero usando `_sbss` y `_ebss`.
3. Llama a `SystemInit()` (ajustes básicos de reloj y de la FPU).
4. Llama a `__libc_init_array()` (constructores).
5. Llama a `main()`.

**Este archivo no se modifica nunca.** Se toma tal cual del paquete STM32Cube,
en `Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/`. Lo único que hay
que verificar es que corresponde al dispositivo exacto: la tabla de vectores de
un F407 tiene interrupciones que el F411 no posee, y viceversa.

Todos los `IRQHandler` se declaran **débiles** (`.weak`) y apuntan a un bucle
infinito. Al definir una función con el mismo nombre en `stm32f4xx_it.c`, el
enlazador sustituye la débil por la propia. De ahí que un nombre mal escrito
no genere error, sino un cuelgue silencioso.

## 2.4 `stm32f4xx_hal_conf.h`

**Qué hace.** Es el interruptor general de la HAL: qué módulos se compilan y
qué constantes usa la librería.

```c
#define HAL_ADC_MODULE_ENABLED       /* módulos usados */
#define HAL_I2C_MODULE_ENABLED
/* #define HAL_SPI_MODULE_ENABLED */ /* módulos no usados: comentados */

#define HSE_VALUE     8000000U   /* MCO del ST-LINK, UM1724 §5.7 */
#define HSI_VALUE    16000000U   /* fijo por diseño del chip, DS10314 */
#define LSE_VALUE       32768U   /* cristal X2 de la placa */
```

**Importante.** `HSE_VALUE` viene con valor 25000000 en la plantilla de ST,
porque esa es la frecuencia de las placas Discovery. En la Nucleo-F411RE el
HSE procede del MCO del ST-LINK y vale **8 MHz** (UM1724 §5.7). Dejar el valor
incorrecto haría que `HAL_RCC_GetSysClockFreq()` mintiera y que todos los
baudios y temporizaciones salieran mal si se usara el HSE.

**Reutilización.** Se copia el archivo y solo se ajustan: la lista de módulos
habilitados y los tres valores de frecuencia de oscilador.

---

# Parte 3 — Reloj del sistema (`system_clock.c`)

## 3.1 Qué hace

Lleva el SYSCLK de los 16 MHz del HSI a los 100 MHz máximos del F411, arranca
el cristal LSE y expone por PA8 la señal de reloj que se quiera verificar.

## 3.2 De dónde salen los valores

**Fórmula del PLL — RM0383 §6.3.2:**

```
f_VCO_in  = f_PLL_in / PLLM
f_VCO_out = f_VCO_in * PLLN
f_PLLP    = f_VCO_out / PLLP        (este es el SYSCLK)
f_PLLQ    = f_VCO_out / PLLQ        (rama para USB y SDIO)
```

Restricciones del mismo apartado:

| Magnitud | Rango admitido | Valor elegido |
|---|---|---|
| `f_VCO_in` | 1 a 2 MHz | **2 MHz** (16 / 8) |
| `f_VCO_out` | 100 a 432 MHz | **200 MHz** (2 × 100) |
| `PLLM` | 2 a 63 | **8** |
| `PLLN` | 50 a 432 | **100** |
| `PLLP` | 2, 4, 6, 8 | **2** |
| `PLLQ` | 2 a 15 | 4 |

Se toma `f_VCO_in = 2 MHz`, el máximo del rango, porque RM0383 advierte que
un valor bajo aumenta el ruido de fase del PLL.

**Latencia de FLASH — RM0383 Tabla 6:** a 2.7–3.6 V la memoria admite 30 MHz
por estado de espera.

| Estados de espera | Frecuencia máxima de HCLK |
|---|---|
| 0 WS | 30 MHz |
| 1 WS | 60 MHz |
| 2 WS | 90 MHz |
| **3 WS** | **100 MHz** |

Poner menos estados de los necesarios produce lecturas de instrucción
erróneas: el síntoma típico es un `HardFault` inmediato tras el cambio de reloj.

**Escala de voltaje — RM0383 §5.1.4:** por encima de 84 MHz es obligatoria la
escala 1 (`PWR_REGULATOR_VOLTAGE_SCALE1`).

**Límites de bus — DS10314 Tabla 3:** AHB y APB2 hasta 100 MHz, APB1 hasta
50 MHz. De ahí `RCC_HCLK_DIV2` en APB1.

## 3.3 Código comentado

```c
void system_clock_init(void)
{
    RCC_OscInitTypeDef osc = {0};   /* {0} inicializa TODOS los campos a cero;
                                       omitirlo deja basura de pila y produce
                                       fallos intermitentes muy difíciles de
                                       reproducir */
    RCC_ClkInitTypeDef clk = {0};

    __HAL_RCC_PWR_CLK_ENABLE();     /* sin reloj en PWR, escribir en PWR->CR
                                       no tiene efecto ni genera error */

    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    /* SCALE1 = hasta 100 MHz.  SCALE2 = hasta 84 MHz.  SCALE3 = hasta 64 MHz.
       Bajar de escala reduce el consumo del regulador interno. */

    HAL_PWR_EnableBkUpAccess();     /* levanta la protección del dominio de
                                       respaldo (bit DBP de PWR->CR); sin esto
                                       el LSE no arranca y el RTC no se puede
                                       configurar */

    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSE;
    /* Las máscaras se combinan con OR: una sola llamada configura ambos
       osciladores. Los que no se nombren quedan intactos. */

    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    /* Valor de ajuste fino del HSI grabado en fábrica (16). Se puede
       modificar para compensar la deriva térmica, pero rara vez es necesario. */

    osc.LSEState = RCC_LSE_ON;
    /* Alternativa: RCC_LSE_BYPASS, cuando en lugar de un cristal se inyecta
       una señal de reloj ya generada por un oscilador externo. */

    osc.PLL.PLLState  = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_HSI;   /* alternativa: RCC_PLLSOURCE_HSE */
    osc.PLL.PLLM      = 8U;                  /* 16 MHz / 8   = 2 MHz  */
    osc.PLL.PLLN      = 100U;                /* 2 MHz * 100  = 200 MHz */
    osc.PLL.PLLP      = RCC_PLLP_DIV2;       /* 200 MHz / 2  = 100 MHz */
    osc.PLL.PLLQ      = 4U;                  /* 200 MHz / 4  = 50 MHz  */

    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        Error_Handler();   /* el cristal LSE no oscila, o el PLL no engancha */
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK  | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;    /* HCLK  = 100 MHz */
    clk.APB1CLKDivider = RCC_HCLK_DIV2;      /* PCLK1 =  50 MHz */
    clk.APB2CLKDivider = RCC_HCLK_DIV1;      /* PCLK2 = 100 MHz */

    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_3) != HAL_OK) {
        Error_Handler();
    }

    SystemCoreClockUpdate();        /* recalcula la variable global leyendo los
                                       registros; la usan la HAL y las
                                       librerías para todos sus cálculos */
    HAL_InitTick(TICK_INT_PRIORITY);/* reprograma el SysTick: con el reloj
                                       nuevo, el valor de recarga anterior
                                       daría un tick de 0.16 ms en vez de 1 ms */

    system_clock_mco_set(MCO_SRC_PLL);
}
```

**Orden crítico.** `HAL_RCC_ClockConfig` sube primero la latencia de FLASH y
después conmuta el reloj cuando se aumenta la frecuencia, y al revés cuando se
reduce. Hacerlo manualmente en el orden inverso provoca un `HardFault`.

## 3.4 La salida MCO1

**Qué es.** MCO (*Microcontroller Clock Output*) enruta un reloj interno a un
pin físico. Es el único método directo para demostrar con instrumentación que
el árbol de relojes quedó como se pretendía.

**De dónde sale.** RM0383 §6.2.10 y registro `RCC_CFGR`, campos `MCO1`
(bits 22–21, fuente) y `MCO1PRE` (bits 26–24, prescalador de 1 a 5).
DS10314 confirma que MCO1 está en **PA8**, función alternativa **AF0**.

```c
void system_clock_mco_set(mco_src_t src)
{
    switch (src) {

    case MCO_SRC_HSI:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_HSI, RCC_MCODIV_1);
        s_mco_freq = HSI_VALUE;              /* 16 MHz sin dividir */
        break;

    case MCO_SRC_LSE:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_LSE, RCC_MCODIV_1);
        s_mco_freq = LSE_VALUE;              /* 32768 Hz sin dividir */
        break;

    case MCO_SRC_PLL:
    default:
        HAL_RCC_MCOConfig(RCC_MCO1, RCC_MCO1SOURCE_PLLCLK, RCC_MCODIV_5);
        s_mco_freq = HAL_RCC_GetSysClockFreq() / 5U;   /* 20 MHz */
        src = MCO_SRC_PLL;
        break;
    }
    s_mco_src = src;
}
```

`HAL_RCC_MCOConfig` hace tres cosas por sí sola: habilita el reloj de GPIOA,
configura PA8 como AF0 a `GPIO_SPEED_FREQ_VERY_HIGH` y escribe los campos de
`RCC_CFGR`. No hace falta tocar el GPIO manualmente.

### Justificación del prescalador

| Fuente | Frecuencia | Prescalador | Salida | Razonamiento |
|---|---|---|---|---|
| HSI | 16 MHz | /1 | 16 MHz | Medible sin dificultad; dividir ocultaría el valor real |
| LSE | 32.768 kHz | /1 | 32.768 kHz | Debe verse el valor nominal del cristal |
| PLL | 100 MHz | /5 | 20 MHz | /5 es el máximo divisor disponible; reduce el *slew rate* exigido al pin |

La pregunta previsible en la sustentación es «¿por qué no /1 para el PLL?».
La respuesta tiene dos partes: **eléctrica**, porque una salida cuadrada de
100 MHz en un pin de propósito general con una punta de osciloscopio y su
capacidad parásita degrada el flanco hasta hacerlo casi senoidal; y
**metrológica**, porque muchos osciloscopios de laboratorio docente tienen un
ancho de banda de 100 MHz, justo el límite, con lo que la amplitud medida sería
falsa. A 20 MHz la medida es limpia y la relación con el SYSCLK es exacta:
20 × 5 = 100 MHz.

## 3.5 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| Estructura completa de la función | `PLLM`, `PLLN`, `PLLP` según la frecuencia deseada |
| `__HAL_RCC_PWR_CLK_ENABLE()` + escala de voltaje | `FLASH_LATENCY_x` según la frecuencia final |
| `HAL_PWR_EnableBkUpAccess()` si se usa RTC | Divisores de APB1/APB2 según los límites del chip |
| `SystemCoreClockUpdate()` + `HAL_InitTick()` | `PLLSource` si se usa HSE |
| El bloque completo de MCO | El pin, si el chip no es de la serie F4 |

Ejemplo: el mismo código a 84 MHz solo necesita `PLLN = 84` y
`FLASH_LATENCY_2`.

---

# Parte 4 — Temporizadores (`timer.c`)

## 4.1 Qué hace

Genera dos bases de tiempo independientes del núcleo: 250 ms para el LED y
100 ms para el resto del sistema.

## 4.2 De dónde salen los valores

**El multiplicador ×2 del reloj de los temporizadores — RM0383 §6.2:**

> Si el prescalador de APBx es 1, el reloj de los temporizadores es igual al
> de APBx. En caso contrario, es **el doble**.

Como APB1 usa `RCC_HCLK_DIV2`, el reloj de TIM2..TIM5 no es 50 MHz sino
**100 MHz**. Ignorar esta regla es el error más frecuente al calcular
temporizaciones, y produce periodos exactamente a la mitad de lo esperado.

**Fórmula del periodo — RM0383 §13.4.1:**

```
f_contador = f_TIM / (PSC + 1)
T          = (ARR + 1) / f_contador
```

Despejando para TIM3:

```
PSC + 1 = 10000  ->  f_contador = 100 MHz / 10000 = 10 kHz  (paso de 100 µs)
ARR + 1 = 2500   ->  T = 2500 * 100 µs = 250 ms
```

**Criterio de reparto entre PSC y ARR.** Ambos son de 16 bits (0 a 65535).
Existen muchas parejas válidas; se elige la que deja `ARR` lo mayor posible,
porque la resolución con la que se puede ajustar el periodo es un paso del
contador. Con `PSC = 9999` cada paso vale 100 µs; con `PSC = 49999` valdría
500 µs y el periodo solo podría ajustarse de medio en medio milisegundo.

## 4.3 Código comentado

```c
htim3.Instance           = TIM3;
htim3.Init.Prescaler     = 10000U - 1U;  /* el hardware suma 1 automáticamente */
htim3.Init.CounterMode   = TIM_COUNTERMODE_UP;
/* UP: cuenta 0 -> ARR y desborda.  DOWN: cuenta ARR -> 0.
   CENTERALIGNED1/2/3: sube y baja; se usa en PWM de motores para
   evitar el ruido de conmutación simultánea. */
htim3.Init.Period        = 2500U - 1U;
htim3.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
/* No divide el contador: es el divisor del reloj de muestreo de los filtros
   digitales de las entradas de captura. Irrelevante en modo base de tiempo. */
htim3.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
/* DISABLE: escribir ARR tiene efecto inmediato.
   ENABLE: el nuevo ARR se aplica en el siguiente desbordamiento, evitando
   periodos truncados si se cambia la frecuencia sobre la marcha. */

HAL_TIM_Base_Init(&htim3);

HAL_NVIC_SetPriority(TIM3_IRQn, 7, 0);
HAL_NVIC_EnableIRQ(TIM3_IRQn);

HAL_TIM_Base_Start_IT(&htim3);
/* La variante sin _IT arranca el contador pero NO habilita la interrupción,
   obligando a sondear el flag con __HAL_TIM_GET_FLAG. El enunciado exige
   interrupciones, de modo que _IT es obligatoria aquí. */
```

## 4.4 El callback compartido

La HAL define **un solo** callback para todos los temporizadores. Se distingue
el origen comparando el puntero de instancia:

```c
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM3) {
        gpio_led_toggle();               /* cada 250 ms */
    }
    else if (htim->Instance == TIM4) {
        adc_joystick_start();            /* cada 100 ms: lanza el ADC */
        fsm_post_event(EV_TICK_100MS);   /* y avisa a la FSM          */
    }
}
```

**Detalle importante.** `HAL_InitTick()` usa el SysTick, no un temporizador, de
modo que TIM3 y TIM4 quedan libres. En proyectos donde la HAL se configura para
usar un TIM como base de tiempo, ese temporizador aparecería también en este
callback.

## 4.5 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| Estructura de `HAL_TIM_Base_Init` y del callback | `Prescaler` y `Period` según el periodo deseado |
| La secuencia NVIC + `Start_IT` | La instancia (`TIM2`, `TIM3`, …) y su `IRQn` |
| El patrón `if (htim->Instance == ...)` | El reloj base, si el timer cuelga de APB2 |

Recordatorio: **TIM1 y TIM11 cuelgan de APB2**; con APB2 sin dividir, su reloj
es 100 MHz, no 200 MHz, porque la regla del ×2 solo aplica cuando el
prescalador del bus es distinto de 1.

---

# Parte 5 — ADC y joystick (`adc_joystick.c`)

## 5.1 Qué hace

Digitaliza los dos ejes del joystick sin bloquear el núcleo y sin usar DMA.

## 5.2 El problema y la solución adoptada

El ADC del STM32F4 tiene **un único registro de datos** (`ADC_DR`). Con dos
canales existen tres estrategias:

| Estrategia | Ventaja | Inconveniente |
|---|---|---|
| Sondeo con `HAL_ADC_PollForConversion` | La más simple | Bloquea el núcleo; prohibida por el enunciado |
| Modo *scan* + DMA | La más eficiente | Añade un periférico que el enunciado no pide |
| **Conversión encadenada por interrupción** | Sin bloqueo y sin DMA | Requiere gestionar el canal en el callback |

Se eligió la tercera:

```
TIM4 (100 ms) ──► adc_joystick_start()
                     │ selecciona IN0, HAL_ADC_Start_IT
                     ▼
              [interrupción EOC]
                     │ guarda VRX, selecciona IN1, HAL_ADC_Start_IT
                     ▼
              [interrupción EOC]
                     │ guarda VRY, secuencia terminada
                     ▼
                  (reposo hasta el siguiente TIM4)
```

El núcleo solo interviene durante los pocos microsegundos de cada
interrupción; el resto del tiempo queda libre.

## 5.3 De dónde salen los valores

**Frecuencia máxima del ADC — DS10314, tabla «ADC characteristics»:** f_ADC no
puede superar **36 MHz**. Como el ADC cuelga de APB2 (100 MHz), el menor
divisor válido es 4:

```
100 MHz / 4 = 25 MHz  ✔   (DIV2 daría 50 MHz, fuera de especificación)
```

**Tiempo de muestreo — RM0383 §11.5:** el condensador de muestreo se carga a
través de la impedancia de la fuente. La fórmula del tiempo mínimo aparece en
DS10314 como `t_S >= (R_AIN + R_ADC) × C_ADC × ln(2^(N+2))`. Los potenciómetros
de los joysticks comerciales rondan los 10 kΩ, valor alto, por lo que se toma
un tiempo generoso: **84 ciclos**.

Alternativas disponibles: 3, 15, 28, 56, 84, 112, 144 y 480 ciclos. Un tiempo
insuficiente produce lecturas sistemáticamente bajas y ruidosas; es la causa
más habitual de que un potenciómetro «no llegue» al fondo de escala.

**Resolución — RM0383 §11.3:** 12 bits dan 0 a 4095. El centro mecánico del
joystick queda cerca de 2048.

## 5.4 Código comentado

```c
static void adc_select_channel(uint8_t idx)
{
    ADC_ChannelConfTypeDef ch = {0};

    ch.Channel      = (idx == 0) ? ADC_CHANNEL_0 : ADC_CHANNEL_1;
    ch.Rank         = 1;                        /* única posición de la secuencia */
    ch.SamplingTime = ADC_SAMPLETIME_84CYCLES;  /* fuente de alta impedancia */

    HAL_ADC_ConfigChannel(&hadc1, &ch);
}

void adc_joystick_init(void)
{
    __HAL_RCC_ADC1_CLK_ENABLE();

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;   /* 25 MHz */
    hadc1.Init.Resolution     = ADC_RESOLUTION_12B;
    /* Alternativas: 10B, 8B, 6B. Menos bits = conversión más rápida. */

    hadc1.Init.ScanConvMode          = DISABLE;  /* un canal por secuencia */
    hadc1.Init.ContinuousConvMode    = DISABLE;  /* conversión bajo demanda */
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    /* Alternativa: disparo directo por el TRGO de un temporizador, que elimina
       la necesidad de llamar a Start_IT desde la ISR de TIM4. Se descartó por
       ser menos legible para el propósito docente del proyecto. */

    hadc1.Init.DataAlign       = ADC_DATAALIGN_RIGHT;
    /* RIGHT: el valor se lee tal cual.  LEFT: desplazado 4 bits, útil cuando
       se quiere tratar el resultado como 16 bits con signo. */

    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.EOCSelection    = ADC_EOC_SINGLE_CONV;
    /* La bandera EOC se activa al terminar CADA conversión. La alternativa
       ADC_EOC_SEQ_CONV solo la activa al final de toda la secuencia. */

    HAL_ADC_Init(&hadc1);
    adc_select_channel(0);

    HAL_NVIC_SetPriority(ADC_IRQn, 6, 0);
    HAL_NVIC_EnableIRQ(ADC_IRQn);
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance != ADC1) return;

    if (s_channel == 0) {
        s_raw_x   = (uint16_t)HAL_ADC_GetValue(hadc);  /* leer DR limpia EOC */
        s_channel = 1;
        adc_select_channel(1);
        HAL_ADC_Start_IT(hadc);        /* encadena la segunda conversión */
    } else {
        s_raw_y   = (uint16_t)HAL_ADC_GetValue(hadc);
        s_channel = 0;
    }
}
```

**Sobre `volatile`.** `s_raw_x`, `s_raw_y` y `s_channel` se declaran `volatile`
porque se escriben en una interrupción y se leen en el hilo principal. Sin ese
calificador el compilador puede mantener el valor en un registro y no releer
nunca la memoria; con `-O2` el programa dejaría de actualizar la pantalla.

## 5.5 La banda muerta

```c
#define JOY_CENTER    2048U
#define JOY_DEADZONE   600U

static int8_t adc_dir(uint16_t raw)
{
    if (raw > (JOY_CENTER + JOY_DEADZONE)) return  1;
    if (raw < (JOY_CENTER - JOY_DEADZONE)) return -1;
    return 0;
}
```

**Por qué es necesaria.** El centro mecánico de un joystick barato no coincide
con el centro eléctrico: la tolerancia típica es de ±10 %, y a eso se suma el
ruido del ADC. Sin banda muerta, las coordenadas cambiarían solas con la
palanca en reposo. Los 600 puntos equivalen a un ±15 % del fondo de escala.

## 5.6 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| La máquina de encadenamiento del callback | Los canales (`ADC_CHANNEL_x`) y sus pines |
| `ClockPrescaler` (si APB2 sigue a 100 MHz) | `JOY_CENTER` y `JOY_DEADZONE` según el sensor |
| El patrón `volatile` + accesores | `SamplingTime` según la impedancia de la fuente |
| La lógica de la banda muerta | El número de canales, si son más de dos |

Para tres o más canales, la extensión natural es sustituir el `if/else` por un
índice que recorra un vector de canales.

---

# Parte 6 — RTC (`rtc.c`)

## 6.1 Qué hace

Mantiene fecha y hora incluso sin alimentación principal, y genera una
interrupción cada segundo para refrescar la pantalla.

## 6.2 De dónde salen los valores

**Fórmula de los prescaladores — RM0383 §16.3.3:**

```
f_ck_spre = f_RTCCLK / ((PREDIV_A + 1) × (PREDIV_S + 1))
```

Se busca `f_ck_spre = 1 Hz` partiendo de 32768 Hz:

```
32768 / ((127 + 1) × (255 + 1)) = 32768 / 32768 = 1 Hz   ✔
```

RM0383 recomienda **maximizar PREDIV_A** (rama asíncrona, 7 bits, máximo 127)
porque es la que más contribuye al ahorro de consumo del contador. De ahí la
pareja 127/255 y no, por ejemplo, 255/127.

**Dominio de respaldo — RM0383 §5.1.2 y AN4759:** el RTC, sus 20 registros de
respaldo y el LSE residen en un dominio alimentado desde VBAT cuando VDD
desaparece. Ese dominio **no se reinicia con el reset del núcleo**, lo que es
precisamente lo que permite conservar la hora.

**Escritura protegida — RM0383 §5.1.2:** el bit `DBP` de `PWR_CR` protege el
dominio de respaldo contra escrituras accidentales. Sin
`HAL_PWR_EnableBkUpAccess()` toda configuración del RTC se descarta en
silencio.

## 6.3 El problema central y su solución

Si el firmware pusiera el RTC en hora en cada arranque, la funcionalidad
exigida sería imposible. Hay dos cosas que **no** deben repetirse tras el
primer arranque:

1. **Seleccionar la fuente de reloj del RTC.** Cambiar los bits `RTCSEL` de
   `RCC_BDCR` obliga a un reset del dominio de respaldo, que borra la hora.
2. **Cargar fecha y hora.**

La solución estándar, descrita en AN4759, usa un **registro de respaldo como
testigo**:

```c
/* 1. Solo se toca RTCSEL si la fuente actual no es ya el LSE */
if (__HAL_RCC_GET_RTC_SOURCE() != RCC_RTCCLKSOURCE_LSE) {
    pclk.PeriphClockSelection = RCC_PERIPHCLK_RTC;
    pclk.RTCClockSelection    = RCC_RTCCLKSOURCE_LSE;
    HAL_RCCEx_PeriphCLKConfig(&pclk);   /* esta llamada resetea el backup */
}

__HAL_RCC_RTC_ENABLE();

hrtc.Instance          = RTC;
hrtc.Init.HourFormat   = RTC_HOURFORMAT_24;  /* alternativa: _12 con AM/PM */
hrtc.Init.AsynchPrediv = 127U;               /* 32768 / 128 = 256 Hz */
hrtc.Init.SynchPrediv  = 255U;               /* 256   / 256 = 1 Hz   */
hrtc.Init.OutPut       = RTC_OUTPUT_DISABLE; /* sin salida por PC13  */
HAL_RTC_Init(&hrtc);

/* 2. La firma en el registro de respaldo decide si hay que poner en hora */
if (HAL_RTCEx_BKUPRead(&hrtc, RTC_BKP_DR0) != RTC_BACKUP_SIGNATURE) {
    ... HAL_RTC_SetTime(...);
    ... HAL_RTC_SetDate(...);
    HAL_RTCEx_BKUPWrite(&hrtc, RTC_BKP_DR0, RTC_BACKUP_SIGNATURE);
}
```

Si la pila mantiene el dominio, la firma sobrevive y el bloque se omite: la
hora continúa. Si se retira la pila, el dominio se pierde, la firma se borra y
el firmware vuelve a poner el RTC en hora automáticamente.

## 6.4 La hora inicial desde el compilador

En lugar de una constante fija, se usan las macros estándar de C `__DATE__` y
`__TIME__`, que el preprocesador sustituye por el instante de la compilación.
Así la primera puesta en hora es automáticamente la hora real.

```c
static uint8_t build_field(uint8_t offset)
{
    const char *t = __TIME__;    /* formato fijo "HH:MM:SS" */
    return (uint8_t)((t[offset] - '0') * 10 + (t[offset + 1] - '0'));
}
/* Restar '0' a un carácter ASCII da su valor numérico: '7' - '0' = 7 */

static uint8_t build_month(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *d = __DATE__;    /* formato fijo "Jul 21 2026" */
    for (uint8_t i = 0; i < 12U; i++) {
        if (d[0] == months[i*3] && d[1] == months[i*3+1] && d[2] == months[i*3+2]) {
            return (uint8_t)(i + 1U);   /* el RTC numera los meses de 1 a 12 */
        }
    }
    return 1U;
}
```

La posición 4 de `__DATE__` es un espacio para los días de un solo dígito
(`"Jul  5 2026"`), caso que `build_day()` contempla explícitamente.

## 6.5 El despertador de un segundo

**De dónde sale.** RM0383 §16.3.6: el *wakeup timer* puede alimentarse del
reloj `ck_spre` de 1 Hz. Con un contador de recarga igual a 0, el evento se
produce cada segundo.

```c
HAL_RTCEx_DeactivateWakeUpTimer(&hrtc);
/* Obligatorio: el dominio de respaldo no se reinicia con el reset del núcleo,
   de modo que el despertador puede seguir activo de la ejecución anterior. */

HAL_NVIC_SetPriority(RTC_WKUP_IRQn, 9, 0);
HAL_NVIC_EnableIRQ(RTC_WKUP_IRQn);

HAL_RTCEx_SetWakeUpTimer_IT(&hrtc, 0U, RTC_WAKEUPCLOCK_CK_SPRE_16BITS);
/* Alternativa: RTC_WAKEUPCLOCK_RTCCLK_DIV16, que permite periodos inferiores
   al segundo a costa de calcular el contador a partir de 32768/16 = 2048 Hz. */
```

El RTC despierta al núcleo a través de la **línea 22 de EXTI**; la HAL la
configura internamente. Sin ella, la interrupción no llegaría al NVIC.

## 6.6 Lectura correcta de la hora

```c
void rtc_get_time(RTC_TimeTypeDef *t, RTC_DateTypeDef *d)
{
    HAL_RTC_GetTime(&hrtc, t, RTC_FORMAT_BIN);
    HAL_RTC_GetDate(&hrtc, d, RTC_FORMAT_BIN);   /* obligatorio y en este orden */
}
```

**Por qué el orden importa.** RM0383 §16.3.5: al leer `RTC_TR` (hora) el
hardware **congela** los registros sombra para garantizar coherencia entre
hora y fecha, y no los libera hasta que se lee `RTC_DR` (fecha). Si se omite la
lectura de la fecha, la hora se queda congelada para siempre: el síntoma es un
reloj que muestra la hora correcta pero no avanza.

`RTC_FORMAT_BIN` permite trabajar con decimales normales. La alternativa
`RTC_FORMAT_BCD` entrega cada dígito en un nibble, formato en el que el RTC
almacena los datos internamente.

## 6.7 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| Todo el patrón de la firma en `RTC_BKP_DR0` | La firma, si se quiere distinguir versiones de firmware |
| `DeactivateWakeUpTimer` antes de reconfigurar | Los prescaladores, si `RTCCLK` no es 32768 Hz |
| La pareja `GetTime` + `GetDate` | El periodo del despertador |
| Las funciones de `__DATE__` / `__TIME__` | El formato de la cadena mostrada |

---

# Parte 7 — LCD por I2C (`i2c_lcd.c`)

## 7.1 Qué hace

Gobierna una pantalla HD44780 de 16×4 a través de un expansor PCF8574,
usando únicamente dos hilos.

## 7.2 Por qué este periférico sí puede ir por sondeo

El enunciado exime al I2C de la obligación de usar interrupciones, y la razón
técnica es sólida: el HD44780 impone esperas de milisegundos entre comandos
que no se pueden acortar. Convertir eso en una máquina de estados asíncrona
multiplicaría la complejidad sin ganar nada, porque el LCD se refresca en el
bucle principal, donde bloquear unos milisegundos es inocuo.

## 7.3 El protocolo de tres capas

```
  lcd_print_line()      ← capa de aplicación: una línea completa
        │
  lcd_send(byte, mode)  ← capa HD44780: parte el byte en dos nibbles
        │
  lcd_pulse(nibble)     ← capa de señalización: pulso en EN
        │
  lcd_write_byte()      ← capa física: una transacción I2C al PCF8574
```

**Correspondencia de bits del PCF8574** (montaje de los módulos comerciales):

| Bit | Señal HD44780 | Función |
|---|---|---|
| P0 | RS | 0 = comando, 1 = dato |
| P1 | RW | siempre 0: solo se escribe |
| P2 | EN | validación: el dato se captura en el flanco de bajada |
| P3 | luz | retroiluminación |
| P4–P7 | D4–D7 | bus de datos de 4 bits |

## 7.4 Código comentado

```c
static void lcd_pulse(uint8_t nibble)
{
    lcd_write_byte(nibble | 0x04U);    /* EN = 1 (bit P2) */
    HAL_Delay(1);                      /* el HD44780 exige 450 ns mínimo;
                                          1 ms es holgadamente suficiente */
    lcd_write_byte(nibble & ~0x04U);   /* EN = 0: el dato queda capturado */
    HAL_Delay(1);
}

static void lcd_send(uint8_t value, uint8_t mode)
{
    uint8_t high = (value & 0xF0U)        | mode | s_backlight;
    uint8_t low  = ((value << 4) & 0xF0U) | mode | s_backlight;
    /* El nibble alto ya está en la posición correcta (bits 7..4).
       El bajo hay que desplazarlo 4 posiciones a la izquierda.
       A ambos se les añaden RS y el estado de la retroiluminación. */

    lcd_pulse(high);   /* primero el nibble alto: lo exige el HD44780 */
    lcd_pulse(low);
}
```

### La secuencia de arranque

Está tomada literalmente de la hoja de datos del HD44780, figura
«4-bit interface initialization», y **no admite atajos**:

```c
HAL_Delay(50);                              /* espera a que VCC se estabilice */

lcd_pulse(0x30U | s_backlight);  HAL_Delay(5);  /* «modo 8 bits», intento 1 */
lcd_pulse(0x30U | s_backlight);  HAL_Delay(1);  /* intento 2                */
lcd_pulse(0x30U | s_backlight);  HAL_Delay(1);  /* intento 3                */
lcd_pulse(0x20U | s_backlight);  HAL_Delay(1);  /* ahora sí: 4 bits         */

lcd_command(0x28U);   /* Function set: 4 bits, 2 líneas lógicas, fuente 5x8 */
lcd_command(0x08U);   /* Display off                                        */
lcd_command(0x01U);   /* Clear display                                      */
HAL_Delay(2);         /* el borrado tarda 1.52 ms                           */
lcd_command(0x06U);   /* Entry mode: incrementa el cursor, sin desplazar    */
lcd_command(0x0CU);   /* Display on, cursor oculto                          */
```

**Por qué tres veces `0x30`.** El controlador puede arrancar en modo de 8 o de
4 bits según cómo se haya comportado la alimentación. Los tres comandos «8
bits» lo llevan a un estado conocido desde cualquier situación de partida; solo
entonces se le pide conmutar a 4 bits. Omitir esta secuencia es la causa
número uno de la pantalla que muestra una fila de bloques negros.

**Alternativas de `0x0C`:** `0x0E` muestra el cursor, `0x0F` lo hace parpadear.

### Las direcciones de fila

```c
static const uint8_t s_row_offset[LCD_ROWS] = { 0x00U, 0x40U, 0x10U, 0x50U };
```

En los módulos de 4 filas y 16 columnas la memoria **no es contigua**: el
controlador solo maneja dos líneas lógicas de 40 caracteres, y las filas 3 y 4
son la continuación física de las filas 1 y 2. Por eso la fila 3 empieza en
`0x10` (16 = final de la fila 1) y la fila 4 en `0x50` (`0x40` + 16).

**Este vector cambia con el formato de la pantalla:**

| Formato | Offsets |
|---|---|
| 16×2 | `{0x00, 0x40}` |
| 20×4 | `{0x00, 0x40, 0x14, 0x54}` |
| **16×4** | `{0x00, 0x40, 0x10, 0x50}` |

### Escritura de una línea sin parpadeo

```c
void lcd_print_line(uint8_t row, const char *s)
{
    lcd_set_cursor(row, 0U);
    for (uint8_t i = 0U; i < LCD_COLS; i++) {
        ...
        /* rellena con espacios hasta completar las 16 columnas */
    }
}
```

Se rellena con espacios en lugar de borrar la pantalla porque `Clear display`
tarda 1.52 ms y afecta a las cuatro filas, produciendo un parpadeo visible en
cada refresco.

### Detección automática de la dirección

```c
if (HAL_I2C_IsDeviceReady(&hi2c1, LCD_ADDR_PRIMARY, 3U, 100U) == HAL_OK) {
    s_addr = LCD_ADDR_PRIMARY;          /* 0x27 << 1 : PCF8574  */
} else if (HAL_I2C_IsDeviceReady(&hi2c1, LCD_ADDR_SECONDARY, 3U, 100U) == HAL_OK) {
    s_addr = LCD_ADDR_SECONDARY;        /* 0x3F << 1 : PCF8574A */
}
```

**El desplazamiento de un bit es obligatorio.** La HAL espera la dirección de
8 bits, con el bit 0 reservado al sentido de la transferencia. Pasar `0x27` en
lugar de `0x4E` es el error más común al portar código de Arduino, donde la
librería `Wire` espera la dirección de 7 bits.

## 7.5 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| Las cuatro capas del protocolo | `s_row_offset` según el formato |
| La secuencia de arranque completa | `LCD_COLS` y `LCD_ROWS` |
| El mapa de bits del PCF8574 | La dirección I2C si los puentes A0/A1/A2 están soldados |
| `lcd_print_line` con relleno | La instancia I2C y sus pines |

El mapa de bits del PCF8574 **no es un estándar**: existen módulos con RS y EN
intercambiados. Si la pantalla no responde con el conexionado correcto, ese es
el siguiente sospechoso.

---

# Parte 8 — UART (`uart.c`)

## 8.1 Qué hace

Consola serie bidireccional totalmente asíncrona: recibe comandos carácter a
carácter y transmite sin bloquear nunca al llamante.

## 8.2 De dónde salen los valores

**El cableado — UM1724 §6.8:** en la Nucleo-F411RE, PA2 y PA3 están conectados
al ST-LINK, que los expone como puerto serie virtual. Los puentes SB13/SB14
(por defecto cerrados) permiten esa conexión; SB62/SB63 permiten llevar la
USART2 a los conectores en su lugar.

**La función alternativa — DS10314, Tabla 9:** USART2 está en **AF7** sobre PA2
y PA3.

**El registro de baudios — RM0383 §19.3.4:**

```
USARTDIV = f_PCLK / (8 × (2 − OVER8) × BaudRate)
```

Con `OVER8 = 0` (sobremuestreo de 16), PCLK1 = 50 MHz y 115200 baudios:

```
USARTDIV = 50e6 / (16 × 115200) = 27.13
```

La HAL calcula y escribe este valor automáticamente a partir de
`huart2.Init.BaudRate`; el error resultante es del 0.5 %, muy por debajo del
2 % que tolera el protocolo. **Este cálculo depende de PCLK1**: si se cambia el
reloj del sistema sin reinicializar la UART, la comunicación se corrompe.

## 8.3 Recepción

```c
HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
```

Esta llamada **arma** una recepción de un byte y retorna de inmediato. Cuando
llega el carácter, la HAL lo deposita en `s_rx_byte` e invoca
`HAL_UART_RxCpltCallback`.

```c
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;

    switch (s_rx_byte) {
    case 'c': case 'C': fsm_post_event(EV_CMD_CLEAR);  break;
    case 'x': case 'X': fsm_post_event(EV_CMD_MODE_X); break;
    ...
    }

    HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
    /* ↑ IMPRESCINDIBLE: rearma la recepción.
       Omitir esta línea deja la UART sorda tras el primer carácter.
       Es el error más frecuente con HAL_UART_Receive_IT. */
}
```

**Nótese que el callback no ejecuta lógica**: solo traduce el carácter a un
evento y lo encola. Toda la aplicación vive en `fsm.c`.

## 8.4 Transmisión sin bloqueo

`HAL_UART_Transmit` es bloqueante: enviar la ayuda completa (unos 400
caracteres) a 115200 baudios detendría el programa durante 35 ms. La solución
es un **buffer circular** propio:

```c
static volatile uint8_t  s_tx_buf[UART_TX_BUF_SIZE];
static volatile uint16_t s_tx_head = 0;   /* escribe el productor */
static volatile uint16_t s_tx_tail = 0;   /* lee el consumidor    */
static volatile uint8_t  s_tx_busy = 0;

static void uart_start_tx(void)
{
    if (s_tx_busy || (s_tx_head == s_tx_tail)) return;  /* ocupado o vacío */

    s_tx_current = s_tx_buf[s_tx_tail];
    s_tx_tail    = (s_tx_tail + 1U) % UART_TX_BUF_SIZE;
    s_tx_busy    = 1U;
    HAL_UART_Transmit_IT(&huart2, (uint8_t *)&s_tx_current, 1U);
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != USART2) return;
    s_tx_busy = 0U;
    uart_start_tx();      /* encadena el siguiente byte pendiente */
}
```

**Convenio del buffer circular:** `head == tail` significa vacío; se considera
lleno cuando el avance de `head` alcanzaría a `tail`. Esto desperdicia una
posición, pero evita tener que mantener un contador adicional que habría que
proteger contra accesos concurrentes.

**Sobre `s_tx_current`.** La HAL guarda el *puntero* al dato, no el dato. Si se
pasara una variable local, esta dejaría de existir al retornar la función y la
HAL transmitiría basura. Por eso el byte en curso vive en una variable
estática.

## 8.5 Gestión de errores

```c
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART2) {
        __HAL_UART_CLEAR_OREFLAG(huart);   /* limpia el desbordamiento */
        HAL_UART_Receive_IT(&huart2, (uint8_t *)&s_rx_byte, 1U);
    }
}
```

El error de desbordamiento (*overrun*) se produce si llega un carácter antes de
que el anterior se haya procesado. Sin limpiar el flag y rearmar la recepción,
la UART queda muda de forma permanente: es un fallo que solo aparece cuando el
operador escribe rápido o pega texto, y por eso resulta difícil de reproducir.

## 8.6 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| El buffer circular completo | La instancia (`USART1`, `USART2`, `USART6`) y su `IRQn` |
| El patrón de rearme en los tres callbacks | La tabla de comandos del `switch` |
| `HAL_UART_ErrorCallback` con `CLEAR_OREFLAG` | `BaudRate` y `UART_TX_BUF_SIZE` |
| La estructura `UART_InitTypeDef` | Los pines y su AF, si se usa otra USART |

---

# Parte 9 — Máquina de estados (`fsm.c`)

## 9.1 Qué hace

Concentra toda la lógica de aplicación. Es el único módulo que conoce el
significado de las cosas; los demás solo manejan hardware.

## 9.2 Estados y eventos

```c
typedef enum {
    ST_INIT = 0,   /* arranque */
    ST_IDLE,       /* sin modo: la línea 2 muestra el texto móvil */
    ST_MODE_X,     /* modo X: la línea 2 muestra "MODO X" */
    ST_MODE_Y,     /* modo Y: la línea 2 muestra "MODO Y" */
    ST_MESSAGE,    /* aviso temporal de 3 s; guarda el estado anterior */
    ST_ERROR,      /* fallo de periférico (terminal) */
    ST_COUNT       /* centinela: número de estados */
} fsm_state_t;
```

**Por qué un `enum` y no constantes `#define`.** El compilador conoce el
conjunto completo de valores, de modo que avisa si un `switch` deja algún
estado sin tratar (`-Wswitch`, incluido en `-Wall`). Además el depurador
muestra `ST_MODE_X` en lugar de `2`. El centinela `ST_COUNT` permite dimensionar
vectores y validar índices sin duplicar el número de estados.

La misma disciplina se aplica a los eventos, que incluyen tanto órdenes
externas (`EV_CMD_*`, `EV_BUTTON`) como estímulos temporales (`EV_TICK_100MS`,
`EV_SECOND`).

## 9.3 La cola de eventos

```c
#define EVQ_SIZE 32U
static volatile fsm_event_t s_evq[EVQ_SIZE];
static volatile uint8_t s_evq_head = 0;   /* escriben las ISR */
static volatile uint8_t s_evq_tail = 0;   /* lee el bucle principal */

void fsm_post_event(fsm_event_t ev)       /* llamable desde una ISR */
{
    uint8_t next = (s_evq_head + 1U) % EVQ_SIZE;
    if (next == s_evq_tail) return;       /* cola llena: se descarta */
    s_evq[s_evq_head] = ev;
    s_evq_head = next;
}
```

**Por qué esta construcción es segura sin deshabilitar interrupciones.** Hay un
único productor (las ISR, que no se interrumpen entre sí en los puntos
críticos) y un único consumidor (el bucle principal). Cada índice lo modifica
**un solo lado**: la ISR solo toca `head`, el bucle solo toca `tail`. Con
índices de 8 bits, cuya escritura es atómica en el Cortex-M4, no puede haber
estado intermedio observable.

**Por qué se descarta el evento en lugar de esperar.** Una ISR nunca debe
bloquearse: hacerlo retrasaría a todas las interrupciones de igual o menor
prioridad. Perder una pulsación es preferible a perder caracteres de la UART.

## 9.4 El bucle de despacho

```c
void fsm_run(void)
{
    if (g_rtc_second_flag) {              /* bandera, no evento: ver 9.5 */
        g_rtc_second_flag = 0U;
        fsm_post_event(EV_SECOND);
    }

    while ((ev = fsm_get_event()) != EV_NONE) {
        switch (ev) {
        case EV_CMD_MODE_X:
            s_state = ST_MODE_X;
            restore_line2();
            uart_send_line("OK: modo X");
            break;
        ...
        }
    }
}
```

Se vacía la cola entera en cada pasada. Si solo se procesara un evento por
llamada y llegasen eventos más rápido de lo que se consumen, la cola crecería
sin límite hasta desbordar.

## 9.5 Por qué el segundo del RTC usa una bandera y no la cola

`EV_SECOND` llega una vez por segundo y es **idempotente**: refrescar la hora
dos veces produce el mismo resultado. Si el bucle principal se retrasara (por
ejemplo escribiendo cuatro líneas en el LCD), varios eventos `EV_SECOND` se
acumularían en la cola y provocarían refrescos redundantes. Una bandera
booleana colapsa de forma natural los avisos repetidos en uno solo.

Los eventos de comando, en cambio, **no** son idempotentes: pulsar `+` tres
veces debe incrementar tres unidades. Por eso van a la cola.

## 9.6 El efecto de marquesina

```c
static void render_line2(void)
{
    char win[LCD_COLS + 1U];
    uint16_t len = strlen(s_line2);

    if (len <= LCD_COLS) {
        lcd_print_line(1U, s_line2);   /* cabe entero: sin desplazamiento */
        return;
    }

    uint16_t total = len + 3U;         /* separador de 3 espacios al dar la vuelta */

    for (uint16_t i = 0U; i < LCD_COLS; i++) {
        uint16_t idx = (s_scroll + i) % total;   /* aritmética modular = ciclo */
        win[i] = (idx < len) ? s_line2[idx] : ' ';
    }
    win[LCD_COLS] = '\0';
    lcd_print_line(1U, win);
}
```

El mecanismo es **genérico**: cualquier texto de más de 16 caracteres se
desplaza automáticamente. Eso resuelve de paso el aviso
`ACTIVAR MODO X O Y`, que mide 18 caracteres y tampoco cabe.

El separador de tres espacios evita que el final del texto quede pegado a su
propio principio, lo que haría el mensaje ilegible en el punto de empalme.

## 9.7 Estado temporal y regreso

```c
static void show_message(const char *txt)
{
    if (s_state != ST_MESSAGE) {
        s_prev = s_state;              /* memoriza a dónde volver */
    }
    s_state       = ST_MESSAGE;
    s_msg_timeout = MSG_DURATION_TICKS;   /* 30 ticks × 100 ms = 3 s */
    set_line2(txt);
    render_line2();
}
```

La guarda `if (s_state != ST_MESSAGE)` impide que dos avisos consecutivos
sobrescriban el estado de regreso con `ST_MESSAGE`, lo que dejaría la máquina
atrapada en el aviso para siempre. Es el tipo de detalle que distingue una
máquina de estados correcta de una que «casi» funciona.

El vencimiento se cuenta en el evento periódico:

```c
case EV_TICK_100MS:
    apply_joystick();
    render_coords();

    if (++s_tick_scroll >= 3U) {       /* desplaza cada 300 ms */
        s_tick_scroll = 0U;
        s_scroll++;
        render_line2();
    }

    if ((s_state == ST_MESSAGE) && (s_msg_timeout > 0U)) {
        if (--s_msg_timeout == 0U) {
            s_state = s_prev;          /* regresa al estado anterior */
            restore_line2();
        }
    }
    break;
```

**Un solo temporizador para varias cadencias.** En lugar de crear un TIM por
cada ritmo, se usa TIM4 a 100 ms y se dividen por software los periodos
mayores. Es la práctica habitual en sistemas embebidos, donde los
temporizadores son un recurso escaso.

## 9.8 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| La cola circular de eventos completa | Los `enum` de estados y eventos |
| El patrón bandera / cola según idempotencia | El `switch` de despacho |
| El algoritmo de marquesina | Los textos y el formato de las líneas |
| La división por software de cadencias | Las cadencias concretas |
| El patrón `s_prev` para estados temporales | La duración del mensaje |

---

# Parte 10 — Interrupciones (`stm32f4xx_it.c`)

## 10.1 Qué hace

Reúne todas las rutinas de interrupción en un solo archivo. Ninguna contiene
lógica de aplicación: todas delegan en el manejador genérico de la HAL, que a
su vez invoca el callback correspondiente en el módulo del periférico.

```
Hardware → NVIC → TIM3_IRQHandler()          [stm32f4xx_it.c]
                    └─► HAL_TIM_IRQHandler()  [driver HAL: identifica la causa]
                          └─► HAL_TIM_PeriodElapsedCallback()  [timer.c]
```

**Ventaja de este esquema.** La ISR queda ligada al vector (nombre impuesto por
el archivo de arranque), mientras que la lógica queda en el módulo del
periférico. Se puede reorganizar el código sin tocar la tabla de vectores.

## 10.2 Reparto de prioridades

**De dónde sale — PM0214 §4.2.7:** el Cortex-M4 tiene 8 bits de prioridad,
pero STM32F4 solo implementa los 4 superiores, de modo que hay 16 niveles.
`NVIC_PRIORITYGROUP_4` dedica los 4 bits a expropiación y ninguno a
subprioridad, lo que simplifica el razonamiento: **menor número = más urgente**.

```c
HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);   /* en main(), antes de todo */
```

| Prioridad | Interrupción | Criterio |
|---|---|---|
| 5 | `USART2_IRQn` | Un carácter perdido es irrecuperable: el F411 no tiene FIFO |
| 6 | `ADC_IRQn` | Debe encadenar la segunda conversión sin retardo apreciable |
| 7 | `TIM3_IRQn`, `TIM4_IRQn` | Bases de tiempo: toleran microsegundos de fluctuación |
| 8 | `EXTI9_5_IRQn` | Acción humana: la latencia es irrelevante |
| 9 | `RTC_WKUP_IRQn` | Periodo de un segundo: insensible al retardo |
| 15 | `SysTick` | Contador de milisegundos de la HAL |

**Regla que hay que saber justificar:** una ISR solo puede llamar a
`HAL_GetTick()` con seguridad si su prioridad es **numéricamente mayor** (menos
urgente) que la del SysTick, o si no depende de que el contador avance. En este
proyecto el SysTick tiene prioridad 15, la menos urgente, y ninguna ISR espera
a que el tiempo avance: solo lo consulta, cosa que es segura.

## 10.3 Antirrebote del pulsador

```c
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    static uint32_t last = 0;
    uint32_t now = HAL_GetTick();

    if (pin != JOY_SW_PIN) return;
    if ((now - last) < 200U) return;   /* rebote mecánico: se descarta */
    last = now;

    fsm_post_event(EV_BUTTON);
}
```

**Por qué hace falta.** Los contactos metálicos de un pulsador no cierran
limpiamente: durante 1 a 20 ms generan decenas de transiciones. Sin filtrado,
una sola pulsación produciría varios eventos y el modo cambiaría de forma
errática.

**Por qué `(now - last)` y no `now > last + 200`.** La resta de enteros sin
signo es correcta incluso cuando `HAL_GetTick()` desborda a los 49.7 días,
porque la aritmética modular de C da igualmente la diferencia correcta. La
comparación directa fallaría en ese instante.

**Alternativas al antirrebote por software:** un condensador de 100 nF en
paralelo con el pulsador, o un disparador de Schmitt. La solución por software
no cuesta componentes y es suficiente para una interfaz humana.

## 10.4 El vector compartido de EXTI

```c
void EXTI9_5_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(JOY_SW_PIN);
}
```

**De dónde sale — RM0383 §10.2:** las líneas EXTI 5 a 9 comparten un único
vector, igual que las líneas 10 a 15. Solo las líneas 0 a 4 tienen vector
propio. `HAL_GPIO_EXTI_IRQHandler` comprueba qué línea está pendiente, limpia
su bandera e invoca el callback.

**Consecuencia práctica del multiplexor de EXTI:** una línea EXTI la comparten
todos los puertos con el mismo número de pin. PA5, PB5 y PC5 compiten por
EXTI5, y solo uno puede estar activo a la vez. Al elegir el pin del pulsador
hay que verificar que su número no esté ya ocupado.

## 10.5 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| El patrón `XXX_IRQHandler` → `HAL_XXX_IRQHandler` | Qué ISR se implementan |
| El antirrebote con `HAL_GetTick()` | El pin y la ventana de rebote |
| Los manejadores de excepción del núcleo | El reparto de prioridades |
| `HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4)` | — |

---

# Parte 11 — MSP (`stm32f4xx_hal_msp.c`)

## 11.1 Qué hace

MSP significa *MCU Support Package*. La HAL separa deliberadamente dos cosas:

- **Configuración lógica** (formato, velocidad, modo) → en el módulo del
  periférico, y es portable entre placas.
- **Configuración física** (reloj de bus, pines, NVIC) → aquí, y depende de la
  placa concreta.

`HAL_<PPP>_Init()` invoca automáticamente a `HAL_<PPP>_MspInit()`. No hay que
llamarla a mano.

**Ventaja de centralizar.** Todo el mapa de pines queda en un archivo, lo que
permite detectar de un vistazo si dos periféricos se disputan un recurso. Es el
error más común al ampliar un proyecto.

## 11.2 Los cuatro modos de pin que aparecen

```c
/* USART2: función alternativa push-pull */
gi.Mode      = GPIO_MODE_AF_PP;
gi.Pull      = GPIO_PULLUP;      /* línea en reposo a 1: evita tramas espurias */
gi.Alternate = GPIO_AF7_USART2;  /* DS10314 Tabla 9 */

/* ADC: modo analógico */
gi.Mode = GPIO_MODE_ANALOG;      /* desconecta las etapas digitales del pin,
                                    reduciendo fugas y ruido inyectado */
gi.Pull = GPIO_NOPULL;           /* cualquier pull formaría un divisor con el
                                    potenciómetro y falsearía la lectura */

/* I2C: función alternativa en drenador abierto */
gi.Mode      = GPIO_MODE_AF_OD;  /* obligatorio en I2C: varios nodos comparten
                                    la línea y ninguno puede forzarla a 1 */
gi.Pull      = GPIO_PULLUP;      /* apoyo a las pull-up del módulo LCD */
gi.Alternate = GPIO_AF4_I2C1;

/* LED: salida push-pull ordinaria */
gi.Mode  = GPIO_MODE_OUTPUT_PP;
gi.Speed = GPIO_SPEED_FREQ_LOW;  /* 2 MHz basta para 4 Hz; velocidades mayores
                                    solo aumentan el ruido conducido */
```

**Sobre `Speed`.** Controla el *slew rate* de la salida, no la frecuencia a la
que se puede conmutar. Un valor alto en una señal lenta genera armónicos
innecesarios; un valor bajo en una señal rápida redondea los flancos. Para el
I2C y la UART se usa `VERY_HIGH`; para el LED, `LOW`.

## 11.3 El caso del RTC

```c
void HAL_RTC_MspInit(RTC_HandleTypeDef *hrtc)
{
    (void)hrtc;    /* el cast a void silencia el aviso de parámetro no usado */
}
```

El RTC no necesita configuración GPIO: PC14 y PC15 quedan bajo control
exclusivo del dominio de respaldo en cuanto se habilita el LSE, y el reloj del
bloque ya se habilitó en `rtc_init()` con `__HAL_RCC_RTC_ENABLE()`.

## 11.4 Reutilización

| Se copia idéntico | Debe cambiarse |
|---|---|
| La estructura `HAL_<PPP>_MspInit` con su guarda de instancia | Los puertos, pines y AF |
| Los cuatro modos de pin como plantilla | El mapa completo de pines |
| `HAL_MspInit` con SYSCFG y PWR | — |

Al portar a otra placa, este archivo y el `README` son casi lo único que
cambia: los módulos de periférico permanecen intactos.

---

# Parte 12 — `main.c` y `syscalls.c`

## 12.1 El orden de inicialización

```c
int main(void)
{
    HAL_Init();                                  /* 1 */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);  /* 2 */
    system_clock_init();                         /* 3 */

    gpio_init();                                 /* 4 */
    uart_init();
    adc_joystick_init();
    rtc_init();
    lcd_init();
    timer_init();                                /* 5: el último */

    fsm_init();                                  /* 6 */

    while (1) {
        fsm_run();
    }
}
```

| Paso | Por qué va ahí |
|---|---|
| 1 | Sin `HAL_Init()` no hay SysTick, y toda función de la HAL con tiempo máximo de espera se bloquea indefinidamente |
| 2 | Debe preceder a cualquier `HAL_NVIC_SetPriority`, o las prioridades se interpretarían con otro reparto de bits |
| 3 | Baudios, prescalers y latencias se derivan del reloj: configurarlos antes daría valores erróneos |
| 4 | Orden libre entre periféricos |
| 5 | Su ISR llama a `adc_joystick_start()` y a `fsm_post_event()`; arrancar antes invocaría módulos sin inicializar |
| 6 | Necesita el LCD y la UART ya operativos para pintar la bienvenida |

## 12.2 El manejador de errores

```c
void Error_Handler(void)
{
    __disable_irq();

    while (1) {
        HAL_GPIO_TogglePin(LED_GPIO_PORT, LED_PIN);
        for (volatile uint32_t i = 0; i < 500000U; i++) {
            __NOP();
        }
    }
}
```

**Por qué el retardo es un bucle y no `HAL_Delay`.** `HAL_Delay` depende del
SysTick, que acaba de deshabilitarse junto con el resto de interrupciones. El
`volatile` en el contador impide que el optimizador elimine el bucle vacío.

El parpadeo rápido es un diagnóstico visible sin depurador: si al encender la
placa el LED va mucho más rápido de 250 ms, algún periférico falló al
inicializarse. La causa más probable en este proyecto es que el cristal LSE no
oscile.

## 12.3 `syscalls.c`

Newlib espera un sistema operativo debajo. En bare-metal hay que proporcionar
implementaciones vacías de sus llamadas al sistema, o el enlazador falla.

```c
extern int _end;     /* símbolo del linker script: fin de la memoria estática */

void *_sbrk(int incr)
{
    static unsigned char *heap = NULL;
    unsigned char *prev;

    if (heap == NULL) heap = (unsigned char *)&_end;   /* primera llamada */
    prev  = heap;
    heap += incr;
    return (void *)prev;
}
```

`_sbrk` es la única con implementación real: `malloc` la usa para crecer, y
`snprintf` de newlib puede reservar memoria interna. Devolver siempre `-1`
haría fallar el formateo de las cadenas del LCD.

**Alternativa:** enlazar con `--specs=nosys.specs`, que aporta stubs
predefinidos. Se prefirió el archivo propio para tener control sobre `_sbrk` y
para que el comportamiento sea explícito y explicable.

---

# Parte 13 — Resumen de reutilización

## 13.1 Copiable sin cambios a cualquier proyecto STM32F4

| Elemento | Ubicación |
|---|---|
| Bloque de toolchain y reglas del Makefile | `Makefile` |
| Estructura completa del linker script salvo `MEMORY` | `STM32F411RETX_FLASH.ld` |
| `syscalls.c` íntegro | `Core/Src/syscalls.c` |
| Cola circular de eventos | `fsm.c` |
| Buffer circular de transmisión de la UART | `uart.c` |
| Antirrebote por `HAL_GetTick()` | `stm32f4xx_it.c` |
| Las cuatro capas del protocolo HD44780 | `i2c_lcd.c` |
| Patrón de la firma en registro de respaldo del RTC | `rtc.c` |
| Cadena de conversión encadenada del ADC | `adc_joystick.c` |
| Patrón `XXX_IRQHandler` → `HAL_XXX_IRQHandler` → callback | `stm32f4xx_it.c` |
| `Error_Handler` con parpadeo de diagnóstico | `main.c` |

## 13.2 Exclusivo de este proyecto

| Elemento | Motivo |
|---|---|
| `PLLM/PLLN/PLLP` y `FLASH_LATENCY_3` | Dependen de la frecuencia objetivo |
| `MEMORY` del linker y `_estack` | Dependen del tamaño de FLASH y RAM |
| Todo `stm32f4xx_hal_msp.c` | Es el mapa de pines de esta placa |
| `Prescaler` y `Period` de TIM3 y TIM4 | Dependen de los 100 MHz y del periodo pedido |
| `s_row_offset` del LCD | Depende del formato 16×4 |
| `JOY_CENTER` y `JOY_DEADZONE` | Dependen del joystick concreto |
| Los `enum` de estados y eventos | Son la especificación de la aplicación |
| El `switch` de comandos de la UART | Es la interfaz pedida en el enunciado |
| Los textos y el formato de las cuatro líneas | Es el enunciado |

## 13.3 Ejemplo: portar a un STM32F103 a 72 MHz

1. **Makefile:** `-mcpu=cortex-m3`, sin FPU, `-mfloat-abi=soft`,
   `-DSTM32F103xB`; cambiar la lista de fuentes HAL a las del F1.
2. **Linker:** `FLASH 128K`, `RAM 20K`, `_estack = 0x20005000`.
3. **Reloj:** el F1 no tiene PLLM/PLLN/PLLP sino un multiplicador único;
   HSE 8 MHz × 9 = 72 MHz, `FLASH_LATENCY_2`.
4. **MSP:** el F1 no usa el campo `Alternate`; el remapeo se hace con
   `__HAL_AFIO_REMAP_*`.
5. **Timers:** con APB1 a 36 MHz el reloj de TIM es 72 MHz; `PSC = 7199`
   da los mismos 10 kHz.
6. **El resto del código —FSM, LCD, buffers, antirrebote— se copia sin tocar.**

---

# Parte 14 — Preguntas probables de la sustentación

**¿Por qué el PLL parte del HSI y no del HSE?**
Porque el LED debe ir en PH1, que es el pin OSC_OUT del HSE. Usando el
oscilador interno el puerto GPIOH queda libre. Además, en la Nucleo el HSE
depende de la configuración de los puentes SB54/SB55.

**¿Por qué 3 estados de espera en la FLASH?**
RM0383 Tabla 6: a 3.3 V la memoria admite 30 MHz por estado de espera. A
100 MHz hacen falta 3. Con menos, el núcleo lee instrucciones corruptas y se
produce un `HardFault` en cuanto se conmuta el reloj.

**¿Por qué el reloj de los temporizadores es 100 MHz si APB1 va a 50 MHz?**
RM0383 §6.2: cuando el prescalador de un bus APB es distinto de 1, el reloj de
sus temporizadores se multiplica por dos. Es un mecanismo del árbol de relojes
pensado precisamente para no perder resolución temporal.

**¿Por qué MCO1 con prescalador /5 y no /1?**
Por integridad de señal y por metrología. Una salida de 100 MHz en un pin de
propósito general con la capacidad parásita de una punta de osciloscopio
degrada el flanco; además muchos osciloscopios docentes tienen 100 MHz de
ancho de banda, justo el límite. A 20 MHz la medida es limpia y la relación es
exacta: 20 × 5 = 100.

**¿Cómo sobrevive la hora a la pérdida de alimentación?**
El RTC, el LSE y los 20 registros de respaldo están en el dominio de respaldo,
alimentado desde VBAT. El firmware escribe una firma en `RTC_BKP_DR0`; si al
arrancar la firma sigue ahí, no vuelve a poner el reloj en hora ni toca
`RTCSEL`, cuyo cambio forzaría un reset del dominio.

**¿Por qué hay que leer la fecha después de la hora?**
RM0383 §16.3.5: leer `RTC_TR` congela los registros sombra para garantizar
coherencia, y solo se liberan al leer `RTC_DR`. Si se omite, el reloj muestra
una hora fija.

**¿Por qué el I2C puede ir por sondeo y los demás no?**
Porque el HD44780 impone esperas de milisegundos que no se pueden acortar, y
el refresco ocurre en el bucle principal, donde bloquear es inocuo. El
enunciado lo exime explícitamente.

**¿Por qué el ADC no usa DMA?**
Porque con dos canales muestreados cada 100 ms el coste de dos interrupciones
es despreciable, y encadenar conversiones en el callback muestra explícitamente
el mecanismo de interrupción que pide el enunciado. Con muchos canales o
frecuencias altas, el DMA sería la elección correcta.

**¿Qué pasa si se olvida rearmar `HAL_UART_Receive_IT` en el callback?**
La UART recibe exactamente un carácter y queda sorda para siempre. No hay
mensaje de error.

**¿Por qué `volatile` en las variables compartidas con las ISR?**
Sin ese calificador el compilador puede cachear el valor en un registro y no
releer nunca la memoria. Con optimización activada, la pantalla dejaría de
actualizarse aunque la ISR escribiera correctamente.

**¿Cómo se garantiza que la cola de eventos es segura sin deshabilitar interrupciones?**
Hay un único productor y un único consumidor, y cada uno modifica solo su
propio índice. La escritura de un `uint8_t` es atómica en el Cortex-M4, de modo
que no existe estado intermedio observable.

**¿Por qué el aviso `ACTIVAR MODO X O Y` también se desplaza?**
Porque mide 18 caracteres y la pantalla tiene 16 columnas. El algoritmo de
marquesina es genérico: se aplica a cualquier texto que no quepa, sin código
especial.
