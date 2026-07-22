/*
 * syscalls.c - Stubs minimos de las llamadas al sistema que exige newlib.
 * En un sistema bare-metal no hay sistema operativo, por lo que se
 * proporcionan implementaciones vacias para satisfacer al enlazador.
 */
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>

/* Puntero al final de la region estatica; lo define el linker script */
extern int _end;

/*
 * _sbrk: reserva memoria para el heap (usado por malloc).
 * Avanza un puntero desde el final de .bss.
 */
void *_sbrk(int incr)
{
    static unsigned char *heap = NULL;  /* posicion actual del heap */
    unsigned char *prev;

    if (heap == NULL) {
        heap = (unsigned char *)&_end;  /* primera llamada: arranca en _end */
    }
    prev = heap;
    heap += incr;
    return (void *)prev;
}

int _close(int file)                        { (void)file; return -1; }
int _fstat(int file, struct stat *st)       { (void)file; st->st_mode = S_IFCHR; return 0; }
int _isatty(int file)                       { (void)file; return 1; }
int _lseek(int file, int ptr, int dir)      { (void)file; (void)ptr; (void)dir; return 0; }
int _read(int file, char *ptr, int len)     { (void)file; (void)ptr; (void)len; return 0; }
int _write(int file, char *ptr, int len)    { (void)file; (void)ptr; return len; }
void _exit(int status)                      { (void)status; while (1) { } }
int _kill(int pid, int sig)                 { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void)                           { return 1; }
