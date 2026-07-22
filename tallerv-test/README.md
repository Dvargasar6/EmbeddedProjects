# Parcial Taller V 2026-1 — STM32 Nucleo-F411RE

**Autor:** Daniel Vargas
**Institución:** Universidad Nacional de Colombia
**Plataforma:** NUCLEO-F411RE (STM32F411RET6, Cortex-M4F)
**Lenguaje:** C (bare-metal con librerías HAL de ST)
**Sistema de construcción:** Makefile + GNU Arm Embedded Toolchain

---

## 1. Descripción general

Firmware modular que integra seis periféricos del STM32F411RE gobernados por
una máquina de estados finitos. Todos los periféricos, salvo el I2C, operan
por interrupción; el núcleo permanece inactivo en el bucle principal salvo
cuando hay eventos pendientes en la cola.

### Funcionalidades

| # | Funcionalidad | Implementación |
|---|---|---|
| 1 | Joystick analógico de 2 ejes + pulsador | ADC1 (IN0/IN1) por interrupción + EXTI5 |
| 2 | RTC con respaldo por batería | RTC interno sobre cristal LSE de 32.768 kHz |
| 3 | LCD 16×4 por I2C | HD44780 + PCF8574 sobre I2C1 (modo sondeo) |
| 4 | Reloj de sistema a 100 MHz | PLL desde HSI, verificable por MCO1 en PA8 |
| 5 | Consola serie con comandos | USART2 a 115200 baudios por interrupción |
| 6 | LED intermitente a 250 ms | TIM3 con interrupción de actualización, PH1 |

---

## 2. Mapa de conexiones

### 2.1 Pines del microcontrolador

| Pin | Etiqueta Nucleo | Función | Conexión |
|---|---|---|---|
| PA0 | A0 (CN8-1) | ADC1_IN0 | **VRX** del joystick |
| PA1 | A1 (CN8-2) | ADC1_IN1 | **VRY** del joystick |
| PA2 | — | USART2_TX | ST-LINK (puerto virtual) |
| PA3 | — | USART2_RX | ST-LINK (puerto virtual) |
| PA8 | D7 (CN5-8) | MCO1 | Punta del osciloscopio |
| PB5 | D4 (CN5-5) | EXTI5 | **SW** del joystick |
| PB8 | D15 (CN5-10) | I2C1_SCL | **SCL** del módulo LCD |
| PB9 | D14 (CN5-9) | I2C1_SDA | **SDA** del módulo LCD |
| PH1 | — | GPIO salida | **LED** con resistencia de 330 Ω a GND |
| PC14 | — | OSC32_IN | Cristal LSE (en placa) |
| PC15 | — | OSC32_OUT | Cristal LSE (en placa) |

### 2.2 Módulo joystick

```
Joystick          Nucleo-F411RE
--------          -------------
  GND      ---->  GND   (CN6-6 o CN7-8)
  +5V      ---->  5V    (CN6-5)          [véase nota 2.2.1]
  VRX      ---->  PA0   (A0, CN8-1)
  VRY      ---->  PA1   (A1, CN8-2)
  SW       ---->  PB5   (D4, CN5-5)
```

#### 2.2.1 Nota sobre la alimentación del joystick

El ADC del STM32F411 tolera como máximo VDDA (3.3 V) en sus entradas
analógicas. Si el joystick se alimenta a 5 V, VRX y VRY pueden alcanzar 5 V
en los extremos del recorrido y **exceder la tensión máxima admisible**,
lo que degrada el conversor de forma permanente.

Dos soluciones válidas:

- **Recomendada:** alimentar el módulo desde el pin 3V3 (CN6-4) en lugar de 5V.
  El divisor resistivo interno entrega entonces 0–3.3 V y el rango del ADC se
  aprovecha íntegramente.
- **Alternativa:** mantener los 5 V e intercalar un divisor de dos resistencias
  iguales (por ejemplo 10 kΩ / 10 kΩ) en cada eje, aceptando que el fondo de
  escala se reduce a la mitad.

El firmware funciona con ambas opciones; únicamente cambia el valor crudo
observado en el centro de la palanca.

### 2.3 Módulo LCD 16×4 con expansor I2C

```
LCD (PCF8574)     Nucleo-F411RE
-------------     -------------
  GND      ---->  GND
  VCC      ---->  5V    (CN6-5)   [el HD44780 no es fiable a 3.3 V]
  SDA      ---->  PB9   (D14, CN5-9)
  SCL      ---->  PB8   (D15, CN5-10)
```

Las líneas SDA y SCL son de drenador abierto y requieren resistencias de
pull-up. Los módulos comerciales las incorporan (típicamente 4.7 kΩ); el
firmware activa además las pull-up internas del STM32 como apoyo.

La dirección I2C se detecta automáticamente entre `0x27` (PCF8574) y `0x3F`
(PCF8574A).

### 2.4 LED

```
PH1 ----[ 330 Ω ]----|>|---- GND
                     LED
```

### 2.5 Cristal LSE

El cristal X2 de 32.768 kHz debe estar montado y los puentes de soldadura
configurados según UM1724, sección 5.6:

- **SB48 y SB49:** cerrados (conectan PC14/PC15 al cristal).
- **SB55 y SB56:** abiertos (desconectan PC14/PC15 de los conectores).

En las revisiones de Nucleo que se sirven sin X2, el firmware quedará
bloqueado en `Error_Handler()` durante `system_clock_init()`, señalizándolo
con un parpadeo rápido del LED de PH1.

### 2.6 Respaldo del RTC

Para que la cuenta sobreviva a la desconexión de la alimentación principal
se requiere una fuente en VBAT. Según UM1724, sección 5.4, hay que retirar el
puente **JP5** de su posición por defecto y alimentar VBAT (CN7-17) desde una
pila de litio de 3 V a través del puente correspondiente.

---

## 3. Requisitos de software

### 3.1 Arch Linux

```bash
sudo pacman -S arm-none-eabi-gcc arm-none-eabi-newlib arm-none-eabi-binutils \
               arm-none-eabi-gdb stlink openocd picocom make
sudo usermod -aG uucp $USER    # acceso a /dev/ttyACM0 sin privilegios
```

Cerrar y reabrir la sesión para que el cambio de grupo surta efecto.

### 3.2 Distribuciones basadas en Debian

```bash
sudo apt install gcc-arm-none-eabi binutils-arm-none-eabi \
                 libnewlib-arm-none-eabi gdb-multiarch \
                 stlink-tools openocd picocom make
sudo usermod -aG dialout $USER   # en Debian el grupo se llama dialout
```

### 3.3 Windows

Instalar *GNU Arm Embedded Toolchain*, *STM32CubeProgrammer* y *GNU Make for
Windows*. En PowerShell, `make flash` no funciona; usar en su lugar:

```
STM32_Programmer_CLI.exe -c port=SWD -w build\parcial_tallerV.bin 0x08000000 -rst
```

---

## 4. Compilación y programación

```bash
make            # compila y genera build/parcial_tallerV.{elf,hex,bin}
make clean      # borra el directorio build
make flash      # programa la placa con st-flash
make size       # informe detallado de uso de memoria
make serial     # abre la consola serie con picocom a 115200
```

Si el toolchain no está en el `PATH`:

```bash
make GCC_PATH=/opt/gcc-arm-none-eabi/bin
```

Programación alternativa con OpenOCD:

```bash
make openocd-flash
```

Consumo de memoria de la versión entregada:

```
   text    data     bss     dec     hex
  22544     112    2896   25552    63d0
```

Esto representa un 4.3 % de la FLASH (512 KB) y un 2.3 % de la RAM (128 KB).

---

## 5. Lista de comandos de la consola serie

Puerto: `/dev/ttyACM0` · 115200 baudios · 8 bits · sin paridad · 1 bit de parada.
Los comandos son de un solo carácter y actúan de inmediato, sin necesidad de
pulsar Enter. Se aceptan mayúsculas y minúsculas.

| Carácter | Acción | Efecto en la línea 2 del LCD |
|---|---|---|
| `c` | Lleva ambas coordenadas a cero | Sin cambio |
| `x` | Activa el modo X | `MODO X` |
| `y` | Activa el modo Y | `MODO Y` |
| `+` | Incrementa la coordenada del modo activo | `ACTIVAR MODO X O Y` si no hay modo |
| `-` | Decrementa la coordenada del modo activo | `ACTIVAR MODO X O Y` si no hay modo |
| `h` | MCO1 (PA8) = HSI, prescaler /1 → 16 MHz | Sin cambio |
| `l` | MCO1 (PA8) = LSE, prescaler /1 → 32.768 kHz | Sin cambio |
| `p` | MCO1 (PA8) = PLL, prescaler /5 → 20 MHz | Sin cambio |
| `d` | Informe de diagnóstico del sistema | Sin cambio |
| `s` | Sondeo completo del bus I2C | Sin cambio |
| `?` | Imprime la ayuda | Sin cambio |

El pulsador SW del joystick recorre cíclicamente los modos:
`sin modo → MODO X → MODO Y → sin modo`.

---

## 6. Distribución de la pantalla

```
+----------------+
|14:35:07 21-07  |   Línea 1: hora y fecha del RTC, refresco cada segundo
|VARGAS - PARCIA |   Línea 2: texto móvil, o MODO X / MODO Y, o aviso
|X:+025 ADC:2048 |   Línea 3: coordenada X y muestra cruda del eje X
|Y:-010 ADC:1024 |   Línea 4: coordenada Y y muestra cruda del eje Y
+----------------+
```

La línea 2 se desplaza un carácter cada 300 ms cuando el texto excede las 16
columnas. El mecanismo es genérico: también desplaza el aviso
`ACTIVAR MODO X O Y`, que mide 18 caracteres.

Las coordenadas se mueven por dos vías que actúan sobre la misma variable:

- **Joystick:** cada 100 ms, si la palanca está fuera de la banda muerta, la
  coordenada correspondiente varía en una unidad. Mantener la palanca desviada
  produce un barrido continuo de 10 unidades por segundo.
- **Consola serie:** los comandos `+` y `-` aplican un paso unitario a la
  coordenada del modo activo.

Rango de las coordenadas: −999 a +999, con saturación en los extremos.

---

## 7. Procedimiento de verificación

Cada apartado puede comprobarse de forma independiente.

### 7.1 Verificación del reloj a 100 MHz (MCO1)

**Instrumento:** osciloscopio o analizador lógico en **PA8** (D7, CN5-8),
referencia en GND.

| Paso | Acción | Resultado esperado |
|---|---|---|
| 1 | Alimentar la placa | Señal cuadrada de 20 MHz en PA8 (valor por defecto) |
| 2 | Enviar `h` por la consola | La frecuencia pasa a **16 MHz** (HSI directo) |
| 3 | Enviar `l` | La frecuencia pasa a **32.768 kHz** (LSE directo) |
| 4 | Enviar `p` | La frecuencia vuelve a **20 MHz** |

**Interpretación del paso 4:** 20 MHz en PA8 con prescaler /5 implica
20 MHz × 5 = **100 MHz** de salida del PLL, que es el SYSCLK. El prescaler no
se elige a /1 porque la salida directa de 100 MHz degrada el flanco del pin
y dificulta la medida con instrumentación de laboratorio.

**Verificación cruzada sin osciloscopio:** el LED de PH1 parpadea a 250 ms
gobernado por TIM3, cuyo periodo se calcula a partir de los 100 MHz. Si el
reloj no fuese el previsto, el periodo del LED se desviaría en la misma
proporción y sería medible con un cronómetro sobre 20 ciclos.

### 7.2 Verificación del RTC y su respaldo

| Paso | Acción | Resultado esperado |
|---|---|---|
| 1 | Compilar y programar | La línea 1 muestra la hora del momento de compilación |
| 2 | Observar 10 segundos | Los segundos avanzan de uno en uno, sin saltos |
| 3 | Pulsar el botón negro RESET | La hora **continúa**, no se reinicia |
| 4 | Desconectar el USB durante 1 minuto (con pila en VBAT) | Al reconectar, la hora refleja el tiempo transcurrido |
| 5 | Retirar la pila de VBAT y desconectar | Al reconectar, la hora vuelve al valor de compilación |

El paso 5 confirma que el testigo del registro de respaldo `RTC_BKP_DR0`
funciona: al perder el dominio de respaldo se borra la firma y el firmware
vuelve a poner el RTC en hora.

**Verificación de la fuente de reloj:** enviar `l` y comprobar que PA8 entrega
32.768 kHz demuestra que el LSE está oscilando; es la misma señal que alimenta
el RTC.

### 7.3 Verificación del LCD

| Paso | Acción | Resultado esperado |
|---|---|---|
| 1 | Alimentar | Las cuatro líneas muestran contenido; retroiluminación encendida |
| 2 | Observar la línea 2 | El texto se desplaza hacia la izquierda de forma continua |
| 3 | Enviar `x` | La línea 2 pasa a mostrar `MODO X` fijo |
| 4 | Enviar `y` | La línea 2 pasa a mostrar `MODO Y` fijo |

**Si la pantalla muestra una fila de bloques negros:** el controlador no
recibió la secuencia de inicialización. Comprobar el conexionado de SDA/SCL y
ajustar el potenciómetro de contraste del módulo.

**Si la pantalla queda en blanco con retroiluminación:** la dirección I2C no
coincide. Verificar con `i2cdetect` en un adaptador externo o revisar los
puentes A0/A1/A2 del PCF8574.

### 7.4 Verificación del joystick

| Paso | Acción | Resultado esperado |
|---|---|---|
| 1 | Palanca en reposo | `ADC` en ambas líneas próximo a 2048 (±200) |
| 2 | Palanca a la derecha | `ADC` del eje X sube hacia 4095; la coordenada X crece |
| 3 | Palanca a la izquierda | `ADC` del eje X baja hacia 0; la coordenada X decrece |
| 4 | Repetir en vertical | Mismo comportamiento en el eje Y |
| 5 | Soltar la palanca | Las coordenadas dejan de variar y conservan su valor |
| 6 | Pulsar SW | La línea 2 alterna entre `MODO X`, `MODO Y` y el texto móvil |

Si en el paso 1 el valor crudo se aproxima a 4095 con la palanca centrada, el
módulo está alimentado a 5 V; consultar la nota 2.2.1.

### 7.5 Verificación de la UART

| Paso | Comando | Resultado esperado |
|---|---|---|
| 1 | `?` | Se imprime la lista completa de comandos |
| 2 | `+` (sin modo previo) | Consola: `ERROR: seleccione modo...`; LCD línea 2: `ACTIVAR MODO X O Y` durante 3 s |
| 3 | `x` seguido de `+` cinco veces | La coordenada X aumenta en 5 unidades |
| 4 | `y` seguido de `-` tres veces | La coordenada Y disminuye en 3 unidades |
| 5 | `c` | Ambas coordenadas pasan a `+000` |

### 7.6 Verificación del LED

| Paso | Acción | Resultado esperado |
|---|---|---|
| 1 | Observar PH1 | Parpadeo simétrico, 250 ms encendido y 250 ms apagado |
| 2 | Cronometrar 20 conmutaciones | 5.0 s ± 0.1 s |
| 3 | Enviar comandos por consola | El parpadeo **no** se altera |

El paso 3 demuestra que el parpadeo lo genera el hardware del temporizador y
no un retardo en el bucle principal.

### 7.7 Verificación de la arquitectura por interrupciones

Colocar un punto de ruptura en `fsm_run()` con GDB y comprobar que, mientras
el programa está detenido, el LED **sigue parpadeando**: la ISR de TIM3 se
ejecuta con independencia del hilo principal.

```bash
openocd -f interface/stlink.cfg -f target/stm32f4x.cfg &
arm-none-eabi-gdb build/parcial_tallerV.elf
(gdb) target extended-remote localhost:3333
(gdb) break fsm_run
(gdb) continue
```

---

## 8. Estructura del proyecto

```
.
├── Makefile                      Reglas de compilación y programación
├── STM32F411RETX_FLASH.ld        Mapa de memoria del enlazador
├── startup_stm32f411xe.s         Tabla de vectores y Reset_Handler
├── README.md                     Este archivo
├── CODIGO.md                     Manual de programación para la sustentación
├── Core
│   ├── Inc
│   │   ├── main.h                Cabecera común
│   │   ├── stm32f4xx_hal_conf.h  Módulos HAL habilitados
│   │   ├── stm32f4xx_it.h        Declaración de las ISR
│   │   ├── system_clock.h        Relojes y MCO1
│   │   ├── gpio.h                LED y pulsador
│   │   ├── timer.h               TIM3 y TIM4
│   │   ├── adc_joystick.h        Ejes analógicos
│   │   ├── rtc.h                 Reloj de tiempo real
│   │   ├── i2c_lcd.h             Pantalla HD44780
│   │   ├── uart.h                Consola serie
│   │   └── fsm.h                 Estados y eventos
│   └── Src
│       ├── main.c                Orquestación de la inicialización
│       ├── system_clock.c        PLL a 100 MHz y conmutación de MCO1
│       ├── gpio.c                Configuración de PH1 y PB5
│       ├── timer.c               Bases de tiempo por hardware
│       ├── adc_joystick.c        Conversión encadenada de dos canales
│       ├── rtc.c                 RTC con LSE y respaldo
│       ├── i2c_lcd.c             Protocolo HD44780 en 4 bits
│       ├── uart.c                Buffer circular y traducción de comandos
│       ├── fsm.c                 Máquina de estados y presentación
│       ├── stm32f4xx_it.c        Todas las rutinas de interrupción
│       ├── stm32f4xx_hal_msp.c   Relojes, pines y NVIC de cada periférico
│       └── syscalls.c            Stubs de newlib
└── Drivers
    ├── CMSIS                     Cabeceras del núcleo y del dispositivo
    └── STM32F4xx_HAL_Driver      Librerías HAL de ST
```

---

## 9. Máquina de estados

```
                    ┌──────────┐
                    │ ST_INIT  │
                    └────┬─────┘
                         │ fsm_init()
                         ▼
        ┌───────────►┌──────────┐◄───────────┐
        │            │ ST_IDLE  │            │
        │            └────┬─────┘            │
        │            'x'  │  'y'              │ SW
        │       ┌─────────┴─────────┐         │
        │       ▼                   ▼         │
        │  ┌──────────┐        ┌──────────┐   │
        └──┤ST_MODE_X ├───SW──►│ST_MODE_Y ├───┘
           └────┬─────┘        └────┬─────┘
                │  '+' / '-' sin modo activo
                ▼                   ▼
           ┌─────────────────────────────┐
           │        ST_MESSAGE           │
           │  aviso temporal de 3 s      │
           └──────────┬──────────────────┘
                      │ vencimiento
                      ▼ regreso al estado previo
```

`ST_ERROR` es un estado terminal reservado a fallos de periférico; se alcanza
a través de `Error_Handler()`, que además señaliza con el LED.

---

## 10. Reparto de prioridades de interrupción

Grupo de prioridad `NVIC_PRIORITYGROUP_4`: cuatro bits de expropiación y
ninguno de subprioridad. Un número menor implica mayor urgencia.

| Prioridad | Interrupción | Justificación |
|---|---|---|
| 5 | `USART2_IRQn` | Perder un carácter es irrecuperable; sin FIFO en el F411 |
| 6 | `ADC_IRQn` | Encadena la segunda conversión sin retardo apreciable |
| 7 | `TIM3_IRQn`, `TIM4_IRQn` | Bases de tiempo; toleran algunos microsegundos de jitter |
| 8 | `EXTI9_5_IRQn` | Acción humana; la latencia es irrelevante |
| 9 | `RTC_WKUP_IRQn` | Un segundo de periodo; totalmente insensible al retardo |
| 15 | `SysTick` | Contador de milisegundos de la HAL |

---

## 11. Documentación de referencia de ST

| Documento | Código | Contenido utilizado |
|---|---|---|
| Reference Manual | RM0383 | Registros de RCC, ADC, RTC, TIM, I2C, USART, EXTI |
| Datasheet | DS10314 | Funciones alternativas de cada pin, límites eléctricos |
| Nucleo-64 User Manual | UM1724 | Puentes de soldadura, conectores, ST-LINK |
| HAL Description | UM1725 | Firmas y semántica de las funciones HAL |
| Programming Manual | PM0214 | NVIC, prioridades, excepciones del Cortex-M4 |
| Application Note | AN2867 | Diseño del oscilador de cuarzo y arranque del LSE |
| Application Note | AN4759 | Uso del RTC y de los registros de respaldo |

---

## 11.bis Diagnóstico rápido de averías

### Paso 1 — Leer el LED de PH1

El LED es el primer indicador y no depende de ningún periférico externo.

| Comportamiento del LED | Significado | Acción |
|---|---|---|
| Parpadeo a 250 ms | El firmware corre y el reloj es correcto | El fallo está en un periférico concreto; ir al paso 2 |
| Parpadeo muy rápido (~10 Hz) | `Error_Handler()` activo | Falló el PLL o el I2C1; conectar la consola serie |
| Apagado o fijo | El firmware no arranca | Reprogramar; verificar la conexión del ST-LINK |

### Paso 2 — Leer el informe de arranque por la consola

Al encender, el firmware imprime automáticamente un informe. También puede
solicitarse en cualquier momento con el comando `d`:

```
--- Diagnostico del sistema ---
 SYSCLK        : 100000000 Hz
 MCO1 (PA8)    : MCO1:PLL 20MHz
 Cristal LSE   : OK (32768 Hz)
 LCD I2C       : OK en 0x27, 0 fallos
 Joystick ADC  : X=2043  Y=2051
-------------------------------
```

Interpretación de cada línea:

| Línea | Valor anómalo | Diagnóstico |
|---|---|---|
| `SYSCLK` | Distinto de 100000000 | El PLL no enganchó |
| `Cristal LSE` | `NO ARRANCA` | X2 ausente o SB48/SB49 abiertos. El RTC pasa a LSI y **la hora no sobrevivirá al corte de alimentación** |
| `LCD I2C` | `NO DETECTADO` | Ningún expansor respondió; ir al paso 3 |
| `LCD I2C` | `OK` pero con fallos crecientes | Contacto intermitente o pull-up insuficiente |
| `Joystick ADC` | Ambos 0 o ambos 4095 | VRX/VRY sin conectar o alimentación incorrecta |

### Paso 3 — Sondear el bus I2C

El comando `s` recorre las 112 direcciones válidas e informa de las que
responden:

```
--- Sondeo del bus I2C1 ---
  dispositivo en 0x27
  total: 1 dispositivo(s)
---------------------------
```

| Resultado | Causa | Solución |
|---|---|---|
| Ningún dispositivo | Cableado, alimentación o GND | Verificar VCC del módulo, GND común y que SDA/SCL no estén cruzados |
| Ningún dispositivo, pero el proyecto anterior funcionaba | El montaje previo usaba **PB6/PB7** | Recompilar con `make CFLAGS_EXTRA=-DLCD_USE_PB6_PB7`, o mover los cables a PB8/PB9 |
| Responde una dirección inesperada | Puentes A0/A1/A2 del PCF8574 soldados | Ninguna: el firmware detecta el rango 0x20–0x27 y 0x38–0x3F automáticamente |

### Paso 4 — La pantalla conserva texto de un firmware anterior

Síntoma característico: el LCD muestra contenido antiguo y **no cambia nada**.
El HD44780 conserva su memoria DDRAM mientras esté alimentado, de modo que
este síntoma significa que el firmware **no está escribiendo en absoluto**.

Causas en orden de probabilidad:

1. El firmware quedó detenido antes de llegar a `lcd_init()`. Confirmar con el
   paso 1: si el LED parpadea rápido, es esto.
2. Ningún dispositivo responde en el bus. Confirmar con el paso 3.
3. El mapa de bits del módulo PCF8574 difiere del habitual (RS y EN
   intercambiados). Es infrecuente pero existe; en ese caso el sondeo I2C
   encuentra el dispositivo pero la pantalla no reacciona.

## 12. Resolución de problemas frecuentes

| Síntoma | Causa probable | Solución |
|---|---|---|
| El LED parpadea muy rápido y nada responde | `Error_Handler()` activo | El PLL falló o el I2C1 no inicializó; conectar la consola |
| El LCD conserva texto de un proyecto anterior | El firmware no escribe en el bus | Seguir el diagnóstico rápido de la sección 11.bis |
| `Cristal LSE: NO ARRANCA` | X2 no montado en la placa | El sistema sigue funcionando con LSI; para cumplir el enunciado hay que montar X2 |
| La consola no responde | Puerto o velocidad erróneos | `ls /dev/ttyACM*`; confirmar 115200 |
| Caracteres corruptos en la consola | Velocidad mal ajustada o SYSCLK incorrecto | Verificar 20 MHz en PA8 con el comando `p` |
| El LCD muestra bloques negros | Falló la inicialización del HD44780 | Revisar SDA/SCL y el contraste |
| El ADC marca 4095 en reposo | Joystick alimentado a 5 V | Alimentar a 3.3 V; véase la nota 2.2.1 |
| La hora se reinicia en cada arranque | Sin respaldo en VBAT | Montar la pila y reconfigurar JP5 |
| `st-flash` no encuentra la placa | Permisos de USB | Añadir el usuario al grupo `uucp` |

### 12.1 Síntoma: el LCD conserva el contenido de un proyecto anterior

Es el fallo más informativo de todos. Si la pantalla muestra texto antiguo,
significa que **su DDRAM nunca fue reescrita**: el firmware no llegó a
`lcd_init()`, o el bus I2C falla en cada transacción. El módulo conserva lo que
tenía porque el HD44780 mantiene su memoria mientras esté alimentado.

Procedimiento de aislamiento, en este orden:

**Paso 1 — Observar el LED de PH1.**

| Comportamiento | Significado |
|---|---|
| Parpadeo a 250 ms | El firmware llegó al bucle principal. Ir al paso 2 |
| Parpadeo muy rápido | `Error_Handler()`: falló el PLL. Reprogramar la placa |
| Apagado o fijo | El firmware no se está ejecutando. Verificar la programación |

**Paso 2 — Abrir la consola serie antes de alimentar la placa.**

```bash
picocom -b 115200 /dev/ttyACM0
```

Al arrancar, el firmware emite un informe de diagnóstico automático:

```
--- Diagnostico del sistema ---
 SYSCLK        : 100000000 Hz
 MCO1 (PA8)    : MCO1:PLL 20MHz
 Cristal LSE   : OK (32768 Hz)
 LCD I2C       : OK en 0x27, 0 fallos
 Joystick ADC  : X=2048  Y=2050
-------------------------------
```

Interpretación de la línea `LCD I2C`:

| Mensaje | Diagnóstico | Acción |
|---|---|---|
| `OK en 0x27, 0 fallos` | El bus funciona. El problema es de contraste o de mapeo de bits del PCF8574 | Ajustar el potenciómetro azul del módulo |
| `OK en 0x27, N fallos` con N creciente | El expansor responde pero las escrituras fallan | Revisar continuidad de SDA/SCL y masa común |
| `NO DETECTADO` | Ningún dispositivo responde en el bus | Ir al paso 3 |

**Paso 3 — Sondear el bus completo con el comando `s`.**

Recorre las direcciones de 0x08 a 0x77, igual que `i2cdetect` en Linux:

```
--- Sondeo del bus I2C1 (PB8=SCL, PB9=SDA) ---
 Dispositivo hallado: 0x3F
```

| Resultado | Causa | Solución |
|---|---|---|
| Se halla una dirección distinta de 0x27 y 0x3F | Puentes A0/A1/A2 del PCF8574 soldados | Ninguna: el firmware la adopta automáticamente |
| No se halla ninguna | Cableado, alimentación o pull-ups | Ver la lista siguiente |

**Si el sondeo no encuentra nada, comprobar en este orden:**

1. **Masa común.** El error más frecuente. La masa del módulo LCD debe ir a un
   pin GND de la Nucleo, no solo a la fuente externa.
2. **Alimentación del módulo.** El HD44780 necesita 5 V; con 3.3 V el
   expansor puede responder pero la pantalla queda en blanco.
3. **SDA y SCL intercambiados.** SCL en PB8 (D15), SDA en PB9 (D14). Es el
   orden inverso al de la serigrafía de muchos módulos.
4. **Pull-ups.** Si el módulo no las incorpora, añadir 4.7 kΩ de SDA y de SCL
   a 3.3 V. Sin ellas las líneas nunca alcanzan el nivel alto.
5. **Contraste.** Girar el potenciómetro azul del módulo hasta ver la fila de
   bloques. Sin contraste la pantalla parece muerta aunque funcione.

**Paso 4 — Si el informe no aparece en absoluto por la consola.**

El firmware no llegó a `fsm_init()`. Con el LED parpadeando a 250 ms esto es
contradictorio y apunta a la consola, no al firmware: verificar el puerto
(`ls /dev/ttyACM*`), la velocidad (115200) y la pertenencia al grupo `uucp`.


