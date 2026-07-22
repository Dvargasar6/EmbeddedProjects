# Fase 2 — Integración del enlace UART (USART2 / ST-Link VCP)

Archivos nuevos (copiarlos tal cual al árbol `acceso-stm32`):

- `Core/Inc/comm.h`
- `Core/Src/comm.c`

Ambos compilan sin advertencias con `-Wall -Wextra` contra los encabezados
oficiales de ST (verificado con arm-none-eabi-gcc 13.2).

## 1. `stm32f4xx_hal_conf.h`

Descomentar (o añadir) la habilitación del módulo UART:

```c
#define HAL_UART_MODULE_ENABLED
```

Sin esto, `stm32f4xx_hal.h` no incluye `stm32f4xx_hal_uart.h` y `comm.c` no
compila.

## 2. Makefile

Añadir dos fuentes a la lista de compilación:

```make
Core/Src/comm.c
Drivers/HAL/Src/stm32f4xx_hal_uart.c
```

(Ajustar la ruta del HAL al nombre real del directorio en el árbol; es el
mismo lugar donde ya están `stm32f4xx_hal_i2c.c`, etc.)

En CubeIDE no hay que tocar nada: los archivos dentro de `Core/` y
`Drivers/` se compilan automáticamente al refrescar el proyecto (F5).

## 3. `stm32f4xx_it.c`

Añadir el vector de interrupción del USART2:

```c
#include "comm.h"

/* Interrupción global del USART2: recepción byte a byte del enlace con el PC */
void USART2_IRQHandler(void)
{
    Comm_IRQHandler();   /* delega en HAL_UART_IRQHandler(&huart2) */
}
```

## 4. `main.c` — firmware de prueba de la fase

Tras la inicialización ya existente (LCD incluida), añadir:

```c
#include "comm.h"
#include <string.h>   /* strcmp */
```

En la sección de arranque, después de `LCD_Init()` y su banner:

```c
    Comm_Init();                      /* USART2 a 115200, RX por interrupción */
    Comm_Printf("OK BOOT FASE2\r\n"); /* primer mensaje visible en picocom */
    LCD_Line(0, "UART: esperando");   /* estado del enlace en el LCD */
    LCD_Line(1, "115200 8N1");
```

Dentro del `while (1)`, al principio del cuerpo:

```c
        /* --- Servicio del enlace UART: se atiende en cada vuelta del lazo --- */
        const char *cmd = Comm_Poll();          /* NULL si no hay línea nueva */
        if (cmd != NULL) {
            if (strcmp(cmd, "PING") == 0) {
                Comm_Printf("OK PONG\r\n");
            } else if (strcmp(cmd, "STATUS") == 0) {
                /* Máquina de estados aún no implementada: valores fijos.
                   Se sustituirá por el estado real en la fase de integración. */
                Comm_Printf("OK STATE=IDLE GATE=CLOSED\r\n");
            } else {
                Comm_Printf("ERR UNKNOWN\r\n");
            }
            /* El LCD refleja el enlace y el último comando recibido */
            LCD_Line(0, "UART: enlazado");
            LCD_Line(1, cmd);                   /* LCD_Line ya recorta a 16 chars */
        }
```

No añadir retardos (`HAL_Delay`) al lazo: la recepción es por interrupción,
pero la respuesta y el LCD se atienden desde el lazo y deben ejecutarse sin
bloqueos, igual que el resto del proyecto.

## 5. Prueba en el PC (Arch Linux)

```sh
# Pertenencia al grupo uucp (una sola vez; en Debian el grupo es dialout):
#   sudo usermod -aG uucp $USER   # y reiniciar sesión

picocom -b 115200 --echo /dev/ttyACM0
```

`--echo` activa el eco local: el firmware no devuelve eco de lo tecleado.
Para salir de picocom: `C-a C-x`.

Resultado esperado:

1. Al resetear la placa (botón negro) aparece `OK BOOT FASE2` en picocom y
   el LCD muestra `UART: esperando` / `115200 8N1`.
2. Teclear `PING` + Enter → `OK PONG`; el LCD pasa a `UART: enlazado` y la
   línea 1 muestra `PING`.
3. `STATUS` + Enter → `OK STATE=IDLE GATE=CLOSED`.
4. Cualquier otra cosa → `ERR UNKNOWN`.
5. El latido en PH1 sigue a 1 Hz durante todo lo anterior (el enlace no
   bloquea nada).

Nota sobre picocom: Enter envía `\r`; el analizador de `comm.c` acepta `\r`
o `\n` como terminador, así que no hace falta configurar mapeos.

## 6. Detalles de diseño que conviene conocer

- Prioridad NVIC del USART2 = 6, por debajo del TIM5 del latido (5). La ISR
  solo copia un byte; su latencia no es crítica.
- `HAL_UART_ErrorCallback` está implementado: sin él, un *overrun* (llegar
  bytes mientras la ISR está inhibida, p. ej. durante `Storage_Save` en la
  fase 4) mataría la recepción por interrupción en silencio.
- Si una línea llega antes de que el lazo consuma la anterior, la nueva se
  descarta; la pendiente nunca se corrompe. Con un operador humano tecleando
  esto no ocurre.
- `HAL_UART_RxCpltCallback` es un símbolo débil de HAL redefinido en
  `comm.c`. Cuando en la fase de cámara el USART1 use recepción por
  interrupción, ese callback deberá discriminar por `huart->Instance`
  (ya lo hace).
