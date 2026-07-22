/*
 * STM32F411RETX_FLASH.ld
 * Script de enlazado (linker script) para el STM32F411RET6 de la Nucleo-F411RE.
 * Define el mapa de memoria del microcontrolador y en que seccion se coloca
 * cada parte del programa (codigo, constantes, variables, pila, heap).
 */

/* Punto de entrada del programa: primera funcion que ejecuta el nucleo tras el reset.
   Reset_Handler esta definido en startup_stm32f411xe.s */
ENTRY(Reset_Handler)

/* _estack: direccion inicial del puntero de pila (Stack Pointer).
   La pila crece hacia direcciones bajas, por eso se ubica al final de la RAM.
   0x20000000 (inicio RAM) + 128K = 0x20020000 */
_estack = 0x20020000;

_Min_Heap_Size = 0x200;   /* 512 bytes reservados para el heap (malloc)  */
_Min_Stack_Size = 0x400;  /* 1024 bytes reservados para la pila          */

/* Descripcion de las memorias fisicas del chip */
MEMORY
{
  /* RAM: lectura(r), escritura(w), ejecucion(x). 128 KB en el F411RE */
  RAM   (xrw) : ORIGIN = 0x20000000, LENGTH = 128K
  /* FLASH: solo lectura y ejecucion. 512 KB en el F411RE */
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 512K
}

SECTIONS
{
  /* 1. Tabla de vectores de interrupcion.
        DEBE ser lo primero en FLASH: el nucleo Cortex-M4 lee en 0x08000000
        el valor inicial de la pila y en 0x08000004 la direccion de Reset_Handler. */
  .isr_vector :
  {
    . = ALIGN(4);          /* alinea el contador de posicion a 4 bytes */
    KEEP(*(.isr_vector))   /* KEEP evita que el enlazador la elimine por "no usada" */
    . = ALIGN(4);
  } >FLASH

  /* 2. Codigo del programa (texto) */
  .text :
  {
    . = ALIGN(4);
    *(.text)               /* todas las funciones */
    *(.text*)              /* funciones en secciones propias (-ffunction-sections) */
    *(.glue_7)             /* pegamento ARM->Thumb  */
    *(.glue_7t)            /* pegamento Thumb->ARM  */
    *(.eh_frame)           /* informacion de desenrollado de pila */

    KEEP (*(.init))        /* constructores de C++ / inicializacion */
    KEEP (*(.fini))        /* destructores                          */

    . = ALIGN(4);
    _etext = .;            /* simbolo: fin del codigo */
  } >FLASH

  /* 3. Constantes de solo lectura (const, cadenas literales, tablas) */
  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  /* Secciones auxiliares del manejo de excepciones de ARM */
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  /* _sidata: direccion EN FLASH donde estan guardados los valores iniciales
     de las variables inicializadas. El Reset_Handler los copia a RAM. */
  _sidata = LOADADDR(.data);

  /* 4. Variables globales inicializadas (int x = 5;).
        Se ejecutan desde RAM (>RAM) pero se almacenan en FLASH (AT> FLASH). */
  .data :
  {
    . = ALIGN(4);
    _sdata = .;            /* inicio de .data en RAM */
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;            /* fin de .data en RAM    */
  } >RAM AT> FLASH

  /* 5. Variables globales NO inicializadas o inicializadas a cero.
        No ocupan espacio en FLASH; el Reset_Handler las pone a 0. */
  . = ALIGN(4);
  .bss :
  {
    _sbss = .;             /* inicio de .bss */
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;             /* fin de .bss    */
    __bss_end__ = _ebss;
  } >RAM

  /* 6. Comprobacion de espacio libre: si no caben heap + pila, el enlace falla. */
  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );   /* simbolo 'end' usado por newlib para el heap */
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /* Descarta informacion de librerias que no se necesita en bare-metal */
  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
