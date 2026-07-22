/*
 * fsm.c
 * Maquina de estados finitos y logica de presentacion del proyecto.
 *
 * Arquitectura: productor-consumidor.
 *   - Productores: las rutinas de interrupcion (UART, TIM4, EXTI, RTC).
 *     Solo depositan un evento en una cola circular y retornan.
 *   - Consumidor: fsm_run(), llamado desde el bucle infinito de main().
 *     Extrae los eventos y ejecuta las transiciones y el refresco del LCD.
 *
 * Distribucion de la pantalla de 16x4:
 *   Fila 0: HH:MM:SS DD-MM
 *   Fila 1: texto movil, o "MODO X" / "MODO Y", o mensaje temporal
 *   Fila 2: X:<coordenada> ADC:<muestra cruda>
 *   Fila 3: Y:<coordenada> ADC:<muestra cruda>
 */
#include "fsm.h"
#include "uart.h"
#include "i2c_lcd.h"
#include "rtc.h"
#include "adc_joystick.h"
#include "system_clock.h"

/* Bandera del segundo, definida en rtc.c */
extern volatile uint8_t g_rtc_second_flag;

/* ======================= Cola circular de eventos ======================= */
#define EVQ_SIZE  32U   /* holgado: los eventos se consumen en pocos ms */

static volatile fsm_event_t s_evq[EVQ_SIZE];
static volatile uint8_t     s_evq_head = 0;   /* escribe la ISR   */
static volatile uint8_t     s_evq_tail = 0;   /* lee el bucle main */

/* ======================= Variables de estado ============================ */
static fsm_state_t s_state = ST_INIT;
static fsm_state_t s_prev  = ST_IDLE;   /* estado al que volver tras un mensaje */

/* Coordenadas gobernadas conjuntamente por el joystick y por la UART */
static int16_t s_coord_x = 0;
static int16_t s_coord_y = 0;

#define COORD_MIN  (-999)
#define COORD_MAX  ( 999)

/* Texto de la fila 1 y desplazamiento actual del efecto de marquesina */
static const char *s_line2 = NULL;
static uint16_t    s_scroll = 0;

/* Copia de lo ultimo escrito en cada fila. Evita reenviar por I2C una linea
   que no ha cambiado: escribir una fila cuesta unos 7 ms, de modo que
   refrescar las cuatro sin necesidad consumiria una tercera parte del
   periodo de 100 ms sin aportar nada. */
static char s_shadow[LCD_ROWS][LCD_COLS + 1U];

/* Contadores de reparto de tareas, en unidades de 100 ms */
static uint16_t s_tick_scroll  = 0;   /* desplaza el texto cada 300 ms */
static uint16_t s_msg_timeout  = 0;   /* vida restante del mensaje temporal */

/* Textos fijos */
static const char TXT_BANNER[]  = "DANIEL FELIPE VARGAS ARIAS - PARCIAL TALLER V 2026-1 - UNAL";
static const char TXT_MODE_X[]  = "MODO X";
static const char TXT_MODE_Y[]  = "MODO Y";
static const char TXT_NO_MODE[] = "ACTIVAR UN MODO";

#define MSG_DURATION_TICKS  30U   /* 30 * 100 ms = 3 s */

/* ======================= Cola de eventos ================================ */

/*
 * fsm_post_event
 * Deposita un evento. Es la unica funcion del modulo que pueden invocar las
 * rutinas de interrupcion. Si la cola esta llena el evento se descarta, lo
 * que es preferible a bloquear una ISR.
 */
void fsm_post_event(fsm_event_t ev)
{
    uint8_t next = (uint8_t)((s_evq_head + 1U) % EVQ_SIZE);

    if (next == s_evq_tail) {
        return;   /* cola llena */
    }
    s_evq[s_evq_head] = ev;
    s_evq_head = next;
}

/*
 * fsm_get_event
 * Extrae el evento mas antiguo. Devuelve EV_NONE si no hay ninguno.
 */
static fsm_event_t fsm_get_event(void)
{
    fsm_event_t ev;

    if (s_evq_head == s_evq_tail) {
        return EV_NONE;
    }
    ev = s_evq[s_evq_tail];
    s_evq_tail = (uint8_t)((s_evq_tail + 1U) % EVQ_SIZE);
    return ev;
}

/* ======================= Utilidades de presentacion ===================== */

/*
 * clamp_coord
 * Satura un valor dentro del rango representable en la pantalla.
 */
static int16_t clamp_coord(int32_t v)
{
    if (v > COORD_MAX) return (int16_t)COORD_MAX;
    if (v < COORD_MIN) return (int16_t)COORD_MIN;
    return (int16_t)v;
}

/*
 * lcd_update
 * Escribe una fila solo si su contenido difiere del ultimo enviado.
 */
static void lcd_update(uint8_t row, const char *txt)
{
    char norm[LCD_COLS + 1U];
    uint8_t i, end = 0U;

    /* Normaliza a 16 caracteres exactos para que la comparacion sea fiable */
    for (i = 0U; i < LCD_COLS; i++) {
        if (!end && (txt[i] == '\0')) end = 1U;
        norm[i] = end ? ' ' : txt[i];
    }
    norm[LCD_COLS] = '\0';

    if (strncmp(norm, s_shadow[row], LCD_COLS) == 0) {
        return;                       /* sin cambios: no se toca el bus */
    }
    memcpy(s_shadow[row], norm, LCD_COLS + 1U);
    lcd_print_line(row, norm);
}

/*
 * set_line2
 * Cambia el texto de la fila 1 y reinicia el desplazamiento.
 */
static void set_line2(const char *txt)
{
    s_line2  = txt;
    s_scroll = 0;
}

/*
 * render_line2
 * Compone la ventana de 16 caracteres visible de la fila 1.
 * Si el texto cabe entero se muestra fijo; si no, se desplaza ciclicamente
 * anadiendo un separador de tres espacios entre el final y el principio.
 */
static void render_line2(void)
{
    char     win[LCD_COLS + 1U];
    uint16_t len, total, i, idx;

    if (s_line2 == NULL) {
        return;
    }

    len = (uint16_t)strlen(s_line2);

    if (len <= LCD_COLS) {
        lcd_update(1U, s_line2);   /* cabe entero: sin desplazamiento */
        return;
    }

    total = (uint16_t)(len + 3U);      /* longitud del ciclo con separador */

    for (i = 0U; i < LCD_COLS; i++) {
        idx = (uint16_t)((s_scroll + i) % total);
        win[i] = (idx < len) ? s_line2[idx] : ' ';
    }
    win[LCD_COLS] = '\0';

    lcd_update(1U, win);
}

/*
 * render_time
 * Refresca la fila 0 con la hora y la fecha del RTC.
 */
static void render_time(void)
{
    char buf[LCD_COLS + 1U];

    rtc_format_line(buf, sizeof(buf));
    lcd_update(0U, buf);
}

/*
 * render_coords
 * Refresca las filas 2 y 3 con la coordenada y la muestra cruda de cada eje.
 * Formato: "X:+025 ADC:2048" (15 caracteres de los 16 disponibles).
 */
static void render_coords(void)
{
    /* El buffer se dimensiona con holgura para que el compilador pueda
       descartar cualquier posibilidad de truncamiento en snprintf. */
    char buf[24];

    snprintf(buf, sizeof(buf), "X:%+04d ADC:%04u",
             (int)s_coord_x, (unsigned)adc_joystick_raw_x());
    lcd_update(2U, buf);

    snprintf(buf, sizeof(buf), "Y:%+04d ADC:%04u",
             (int)s_coord_y, (unsigned)adc_joystick_raw_y());
    lcd_update(3U, buf);
}

/*
 * show_message
 * Muestra un texto temporal en la fila 1 y programa el regreso al estado
 * anterior transcurridos 3 segundos.
 */
static void show_message(const char *txt)
{
    if (s_state != ST_MESSAGE) {
        s_prev = s_state;      /* memoriza a donde volver */
    }
    s_state       = ST_MESSAGE;
    s_msg_timeout = MSG_DURATION_TICKS;
    set_line2(txt);
    render_line2();
}

/*
 * restore_line2
 * Devuelve a la fila 1 el texto que corresponde al estado activo.
 */
static void restore_line2(void)
{
    switch (s_state) {
    case ST_MODE_X: set_line2(TXT_MODE_X); break;
    case ST_MODE_Y: set_line2(TXT_MODE_Y); break;
    default:        set_line2(TXT_BANNER); break;
    }
    render_line2();
}

/* ======================= Acciones sobre las coordenadas ================= */

/*
 * coord_step
 * Aplica un incremento a la coordenada del modo activo. Si no hay modo
 * seleccionado emite el aviso exigido por el enunciado.
 */
static void coord_step(int16_t delta)
{
    switch (s_state) {

    case ST_MODE_X:
        s_coord_x = clamp_coord((int32_t)s_coord_x + delta);
        render_coords();
        break;

    case ST_MODE_Y:
        s_coord_y = clamp_coord((int32_t)s_coord_y + delta);
        render_coords();
        break;

    case ST_MESSAGE:
        /* Si ya se esta mostrando el aviso se prolonga su duracion, salvo que
           el modo memorizado sea valido, en cuyo caso se aplica el paso. */
        if (s_prev == ST_MODE_X) {
            s_coord_x = clamp_coord((int32_t)s_coord_x + delta);
            render_coords();
        } else if (s_prev == ST_MODE_Y) {
            s_coord_y = clamp_coord((int32_t)s_coord_y + delta);
            render_coords();
        } else {
            s_msg_timeout = MSG_DURATION_TICKS;
        }
        break;

    default:
        show_message(TXT_NO_MODE);
        uart_send_line("ERROR: seleccione modo con 'x' o 'y'");
        break;
    }
}

/*
 * apply_joystick
 * Traduce la posicion del joystick en incrementos de coordenada. Se ejecuta
 * cada 100 ms, de modo que mantener la palanca desviada produce un barrido
 * continuo de 10 unidades por segundo.
 */
static void apply_joystick(void)
{
    int8_t dx = adc_joystick_dir_x();
    int8_t dy = adc_joystick_dir_y();

    if (dx != 0) {
        s_coord_x = clamp_coord((int32_t)s_coord_x + dx);
    }
    if (dy != 0) {
        s_coord_y = clamp_coord((int32_t)s_coord_y + dy);
    }
}

/* ======================= Ayuda por consola ============================== */
static void print_help(void)
{
    uart_send_line("");
    uart_send_line("=== PARCIAL TALLER V 2026-1 - DANIEL VARGAS - UNAL ===");
    uart_send_line(" c : coordenadas X e Y a cero");
    uart_send_line(" x : activa modo X");
    uart_send_line(" y : activa modo Y");
    uart_send_line(" + : incrementa la coordenada del modo activo");
    uart_send_line(" - : decrementa la coordenada del modo activo");
    uart_send_line(" h : MCO1 (PA8) = HSI 16 MHz");
    uart_send_line(" l : MCO1 (PA8) = LSE 32768 Hz");
    uart_send_line(" p : MCO1 (PA8) = PLL / 5 = 20 MHz");
    uart_send_line(" d : informe de diagnostico del sistema");
    uart_send_line(" s : sondeo completo del bus I2C");
    uart_send_line(" ? : esta ayuda");
    uart_send_line("======================================================");
}

/*
 * print_diagnostics
 * Informe de estado de los perifericos criticos. Se emite al arrancar y bajo
 * demanda con el comando 'd'. Permite localizar un fallo de montaje sin
 * depurador y sin osciloscopio.
 */
static void print_diagnostics(void)
{
    char buf[64];

    uart_send_line("");
    uart_send_line("--- Diagnostico del sistema ---");

    snprintf(buf, sizeof(buf), " SYSCLK        : %lu Hz",
             (unsigned long)HAL_RCC_GetSysClockFreq());
    uart_send_line(buf);

    snprintf(buf, sizeof(buf), " MCO1 (PA8)    : %s", system_clock_mco_label());
    uart_send_line(buf);

    uart_send_line(system_clock_lse_ok()
                   ? " Cristal LSE   : OK (32768 Hz)"
                   : " Cristal LSE   : NO ARRANCA -> RTC sobre LSI");

    if (lcd_is_present()) {
        snprintf(buf, sizeof(buf), " LCD I2C       : OK en 0x%02X, %lu fallos",
                 (unsigned)lcd_address(), (unsigned long)lcd_error_count());
    } else {
        snprintf(buf, sizeof(buf), " LCD I2C       : NO DETECTADO");
    }
    uart_send_line(buf);

    snprintf(buf, sizeof(buf), " Joystick ADC  : X=%u  Y=%u",
             (unsigned)adc_joystick_raw_x(), (unsigned)adc_joystick_raw_y());
    uart_send_line(buf);
    uart_send_line("-------------------------------");
}

/* ======================= Interfaz publica =============================== */

void fsm_init(void)
{
    s_state   = ST_IDLE;
    s_prev    = ST_IDLE;
    s_coord_x = 0;
    s_coord_y = 0;

    set_line2(TXT_BANNER);

    lcd_clear();
    render_time();
    render_line2();
    render_coords();

    print_help();
    print_diagnostics();
}

fsm_state_t fsm_get_state(void)
{
    return s_state;
}

/*
 * fsm_run
 * Procesa todos los eventos pendientes. Se invoca sin cesar desde main().
 */
void fsm_run(void)
{
    fsm_event_t ev;

    /* El despertador del RTC deja una bandera en lugar de un evento para no
       depender del tamano de la cola si el bucle principal se retrasa. */
    if (g_rtc_second_flag) {
        g_rtc_second_flag = 0U;
        fsm_post_event(EV_SECOND);
    }

    while ((ev = fsm_get_event()) != EV_NONE) {

        switch (ev) {

        /* ---------- Comandos de coordenada ---------- */
        case EV_CMD_CLEAR:
            s_coord_x = 0;
            s_coord_y = 0;
            render_coords();
            uart_send_line("OK: coordenadas a cero");
            break;

        case EV_CMD_MODE_X:
            s_state = ST_MODE_X;
            restore_line2();
            uart_send_line("OK: modo X");
            break;

        case EV_CMD_MODE_Y:
            s_state = ST_MODE_Y;
            restore_line2();
            uart_send_line("OK: modo Y");
            break;

        case EV_CMD_INC:
            coord_step(+1);
            break;

        case EV_CMD_DEC:
            coord_step(-1);
            break;

        /* ---------- Conmutacion de la salida MCO1 ---------- */
        case EV_CMD_MCO_HSI:
            system_clock_mco_set(MCO_SRC_HSI);
            uart_send_line("OK: MCO1 = HSI 16 MHz (prescaler /1)");
            break;

        case EV_CMD_MCO_LSE:
            system_clock_mco_set(MCO_SRC_LSE);
            uart_send_line("OK: MCO1 = LSE 32768 Hz (prescaler /1)");
            break;

        case EV_CMD_MCO_PLL:
            system_clock_mco_set(MCO_SRC_PLL);
            uart_send_line("OK: MCO1 = PLL 100 MHz / 5 = 20 MHz");
            break;

        case EV_CMD_HELP:
            print_help();
            break;

        /* ---------- Diagnostico ---------- */
        case EV_CMD_DIAG:
            print_diagnostics();
            break;

        case EV_CMD_SCAN:
            lcd_bus_scan_report();
            break;

        /* ---------- Pulsador del joystick ---------- */
        case EV_BUTTON:
            /* Recorre ciclicamente los modos: ninguno -> X -> Y -> ninguno */
            if (s_state == ST_MODE_X) {
                s_state = ST_MODE_Y;
            } else if (s_state == ST_MODE_Y) {
                s_state = ST_IDLE;
            } else {
                s_state = ST_MODE_X;
            }
            restore_line2();
            uart_send_line("SW: cambio de modo");
            break;

        /* ---------- Base de tiempo de 100 ms ---------- */
        case EV_TICK_100MS:
            apply_joystick();     /* la palanca desplaza las coordenadas */
            render_coords();      /* refresca las filas 2 y 3            */

            /* Desplaza el texto movil cada tercer tick, esto es, 300 ms */
            if (++s_tick_scroll >= 3U) {
                s_tick_scroll = 0U;
                s_scroll++;
                render_line2();
            }

            /* Vencimiento del mensaje temporal */
            if ((s_state == ST_MESSAGE) && (s_msg_timeout > 0U)) {
                if (--s_msg_timeout == 0U) {
                    s_state = s_prev;   /* vuelve al estado anterior */
                    restore_line2();
                }
            }
            break;

        /* ---------- Segundo del RTC ---------- */
        case EV_SECOND:
            render_time();
            break;

        default:
            break;
        }
    }
}
