# Parcial - STM32 Nucleo F411RE

Proyecto de parcial en C puro sobre STM32Cube IDE (sin CubeMX), usando la librería HAL de ST, para la placa **Nucleo-F411RE**. Autor: Daniel Felipe Vargas Arias — dedicado a: Dayana Madrid.

## Estado del proyecto

| Paso | Descripción | Estado |
|---|---|---|
| 1 | Blinky en PA5 (TIM10 por interrupción, ~250 ms) | ✅ Completo |
| 2 | Pantalla LCD 20x4 por I2C | ✅ Completo |
| 3 | Comunicación UART (comandos) | ✅ Completo |
| 4 | Joystick por ADC (VRX/VRY) | ✅ Completo |
| 5 | Reloj del sistema a 100 MHz, MCO1, RTC con LSE | ✅ Completo (MCO1 pendiente de verificación con osciloscopio) |

## Funcionalidades

- **LED Blinky**: parpadea a ~250 ms en PA5 (LD2 de la Nucleo), implementado con TIM10 en modo interrupción.
- **Pantalla LCD 20x4 por I2C**: módulo con backpack PCF8574 (modo 4 bits), dirección I2C `0x21`. Muestra hora/fecha real (RTC), el nombre "Dayana Madrid", y las coordenadas X/Y actuales.
- **UART (USART2, 115200 8N1)**: recepción por interrupción de 1 byte. Comandos `+`/`-`/`p`/`m`/`0` controlan las coordenadas X/Y; comandos de 3 letras `HSI`/`LSE`/`PLL` cambian la fuente observable en MCO1.
- **Joystick por ADC1 (VRX/VRY)**: control incremental (no lectura de posición absoluta). Arranca en X=Y=0; mientras el eje esté desviado más allá de una zona muerta central (40%-60%), la coordenada sube o baja 1 unidad cada ~250 ms. Al soltar el joystick, el valor se mantiene donde quedó (igual que los comandos UART, con los que comparte las mismas variables `coordX`/`coordY`).
- **Reloj a 100 MHz**: PLL con fuente HSI (16 MHz interno), verificable en MCO1 (PA8).
- **RTC sobre LSE**: usa el cristal de 32.768 kHz de la Nucleo (X2). La hora inicial se siembra una sola vez con la fecha/hora de compilación del firmware (`__DATE__`/`__TIME__`); en resets posteriores sigue contando desde donde iba (ver nota de VBAT abajo).

## Mapa de conexiones

| Función | Pin(es) | Periférico | Notas |
|---|---|---|---|
| LED Blinky | PA5 | TIM10 (IT) | LD2 en la propia Nucleo, no requiere cableado externo |
| UART comandos | PA2 (TX) / PA3 (RX) | USART2 (IT) | Comparte el VCP del ST-LINK (mismo cable USB), 115200 8N1 |
| Joystick VRX / VRY | PA0 / PA1 | ADC1_IN0 / ADC1_IN1 (IT) | GND y **3V3** (no 5V, ver nota) del joystick a la Nucleo |
| LCD 20x4 I2C | PB8 (SCL) / PB9 (SDA) | I2C1 (polling) | Módulo con backpack PCF8574, dirección `0x21` en este módulo |
| MCO1 (verificación de reloj) | PA8 | MCO1 | Salida observable con osciloscopio/analizador lógico |
| RTC | — | LSE (cristal 32.768 kHz, X2 en la Nucleo) | Requiere alimentación de respaldo (VBAT) para mantener hora sin +5V principal |

**Nota sobre alimentación del joystick**: si el módulo se alimenta a 5V pero el ADC del STM32 mide contra su referencia de 3.3V, el centro del joystick no cae en el 50% del rango del ADC (puede saturar en un extremo). Alimentarlo desde el pin **3V3** de la Nucleo evita el problema (el joystick es solo un potenciómetro resistivo, funciona igual a 3.3V).

**Nota sobre VBAT/RTC**: para que el RTC siga contando tiempo al remover la alimentación principal (como pide el enunciado), el pin VBAT necesita su propia fuente (batería/supercondensador). En la Nucleo, por defecto VBAT está puenteado a VDD (solder bridge SB45): sin modificar esa conexión (o sin una fuente externa en VBAT), el RTC se reinicia al cortar la alimentación igual que el resto del MCU.

**Nota sobre el cristal LSE**: la Nucleo-64 F411RE no siempre trae poblado el cristal X2 (32.768 kHz) de fábrica. En esta placa sí está soldado; si se replica el proyecto en otra Nucleo, hay que verificarlo antes de esperar que el RTC funcione con LSE.

## Lista de comandos (UART, USART2, 115200 baudios 8N1)

| Comando | Acción |
|---|---|
| `+` | Sube la coordenada X en 1 unidad |
| `-` | Baja la coordenada X en 1 unidad |
| `p` | Sube la coordenada Y en 1 unidad |
| `m` | Baja la coordenada Y en 1 unidad |
| `0` | Pone ambas coordenadas en cero |
| `HSI` | Enruta HSI (16 MHz) hacia MCO1, sin prescaler |
| `LSE` | Enruta LSE (32.768 kHz) hacia MCO1, sin prescaler |
| `PLL` | Enruta el PLL (100 MHz) hacia MCO1, con prescaler /4 (25 MHz de salida) |

Los comandos de 3 letras deben enviarse como texto plano sin salto de línea entre caracteres (el firmware evalúa los últimos 3 bytes recibidos en todo momento).

## Máquina de estados finitos

El programa se estructura como una FSM controlada por un `enum` (`SystemState_t`) con 5 estados: `STATE_IDLE`, `STATE_BLINK_TOGGLE`, `STATE_LCD_REFRESH`, `STATE_UART_COMMAND` y `STATE_ADC_UPDATE`. Cada ISR (TIM10, USART2, ADC1, RTC WakeUp Timer) solo levanta una bandera `volatile`; el procesamiento real ocurre en el bucle principal dentro del estado correspondiente, evitando trabajo bloqueante dentro de las interrupciones. El I2C (LCD) es la única excepción permitida por el enunciado: se maneja por sondeo (polling) dentro de `STATE_LCD_REFRESH`.

## Estructura del repositorio

- `Src/main.c`: único archivo fuente de la aplicación (requisito del parcial).
- `Src/stm32f4xx_it.c`: manejadores de interrupción (ISR), reenvían el evento al HAL.
- `Drivers/STM32F4xx_HAL_Driver`: librería HAL de STMicroelectronics (sin modificar).
- `Inc/stm32f4xx_hal_conf.h`: selección de módulos HAL habilitados y valores de oscilador.
- `CODIGO.md`: manual de programación línea por línea para la sustentación oral.

## Cómo compilar y flashear

1. Importar la carpeta del proyecto en STM32CubeIDE (`File > Open Projects from File System`).
2. Compilar con `Project > Build Project` (configuración `Debug`).
3. Conectar la Nucleo por USB y flashear con `Run > Debug` o `Run > Run`.
4. Si el IDE no refleja cambios recientes al correr, refrescar el proyecto (`F5`) o reiniciar STM32CubeIDE.
