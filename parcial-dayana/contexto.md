# Instrucciones del proyecto:

Se debe implementar un C project en CubeIDE sin CubeMX en una STM32 Nucleo F411RE en lenguaje C para el cual se pueden utilizar las librería HAL que contenga lo siguiente:
 
# Funciones a realizar:
 
- Un módulo Joystick con pines: GND, +5V, VRX, VRY. (15 puntos)
- RTC interno del STM32 que siga registrando el tiempo incluso al remover la energía principal del equipo que marque la fecha y hora actual. Debe trabajar con el cristal de 32 kHz de la board (LSE). (20 puntos)

- Una pantalla LCD de 20x4 conectado mediante I2C que emita:
    1. HH:MM:SS DD-MM-YY 
    2. Dayana Madrid
    3. X: posición X del Joystick
    4. Y: posición Y del Joystick 
(10 puntos).

- Procesador funcionando a 100 MHz. Verificación con MCO1. Debe ser posible cambiar desde el puerto serial (utilizando tres caracteres) para observar tres señales: HSI, LSE y el PLL, con el mejor prescaler posible. Los comandos deben ser literalmente "HSI", "LSE" y "PLL" (15 puntos).

- Comunicación UART que responda a los siguientes comandos:
    1. +: Sube la coordenada X.
    2. -: Baja la coordenada X.
    3. p: Sube la coordenada Y.
    4. m: Baja la coordenada Y.
    5. 0: Pone ambas coordenadas en cero.
(10 puntos).

- Un LED blinky a 250 ms en el pin PA5 implemntado con un timer.  (0 puntos, -10 puntos si no está presente)


# Requisitos OBLIGATORIOS:

- El proyecto se realizará por pasos en un único archivo main.c que debe ser entregado:

    1. Blinky funcionando
    2. Funcionamiento de la pantalla LCD mediante I2C
    3. UART
    4. Joystick mediante ADC.
    5. Relojes del sistema y pasos faltantes.

    Si consideras que existe un orden más conveniente puedes proponerlo.
 

- Todos los periféricos a excepción del I2C (UART, timers, ADC, etc) deben implementarse con interrupciones.

- El código debe implementarse mediante una máquina de estados finitos con un ENUM.

- Se debe incluir un archivo README.md que contenga toda la información del proyecto, esto incluye pero no se reduce a: funcionalidades del proyecto, mapa de conexiones, lista de comandos para ejecutar, etc.

- Un archivo CODIGO.md que sirva como un manual de programación de la siguiente manera:
El archivo debe contener todos los códigos importantes del proyecto con su respectiva explicación (incluyendo el archivo de interrupciones, etc). Este archivo debe servir para estudiar para la sustentación oral que vale 30 puntos.
Debe incluir: una descripción general de lo que hace el código y su estructura, sus funciones y qué hace cada una y en el bloque de código debes incluir comentarios que expliquen qué hace cada línea, cada variable y cada función, así como ilustrar parámetros y funciones alternativas a las usadas. También debes incluir qué cosas son reutilizables en otro proyecto del mismo tipo y cuáles deben configurarse distinto y puedes dar algún que otro ejemplo, es decir, qué bloques de códigos se podrían copiar identicamente para cualquier proyecto y qué partes son exclusivas al proyecto en particular.  

- Los archivos README.md y CODIGO.md se deben ir actualizando a medida que el proyecto avanza.