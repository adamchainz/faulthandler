#include "faulthandler.h"
#include <signal.h>

#ifdef HAVE_SIGACTION
typedef struct sigaction _Py_sighandler_t;
#else
typedef PyOS_sighandler_t _Py_sighandler_t;
#endif

#ifdef HAVE_SIGALTSTACK
static stack_t stack;
#endif

int faulthandler_enabled = 0;

/* fileno(stderr) should be 2: anyway, the value is replaced in
 * faulthandler_enable() */
static int fatal_error_fd = 2;

typedef struct {
    int signum;
    int enabled;
    const char* name;
    _Py_sighandler_t previous;
} fault_handler_t;

static struct {
    int fd;
    int delay;
    int repeat;
    int all_threads;
} fault_alarm;

static int fault_signals[] = {
#ifdef SIGBUS
    SIGBUS,
#endif
#ifdef SIGILL
    SIGILL,
#endif
    SIGFPE,
    /* define SIGSEGV at the end to make it the default choice if searching the
       handler fails in faulthandler_fatal_error() */
    SIGSEGV
};
#define NFAULT_SIGNALS (sizeof(fault_signals) / sizeof(fault_signals[0]))
static fault_handler_t fault_handlers[NFAULT_SIGNALS];

/* Fault handler: display the current Python backtrace and restore the previous
   handler. It should only use signal-safe functions. The previous handler will
   be called when the fault handler exits, because the fault will occur
   again. */

void
faulthandler_fatal_error(int signum)
{
    const int fd = fatal_error_fd;
    unsigned int i;
    fault_handler_t *handler;

    /* restore the previous handler */
    for (i=0; i < NFAULT_SIGNALS; i++) {
        handler = &fault_handlers[i];
        if (handler->signum == signum)
            break;
    }
#ifdef HAVE_SIGACTION
    (void)sigaction(handler->signum, &handler->previous, NULL);
#else
    (void)signal(handler->signum, handler->previous);
#endif
    handler->enabled = 0;

    PUTS(fd, "Fatal Python error: ");
    PUTS(fd, handler->name);
    PUTS(fd, "\n\n");

    faulthandler_dump_backtrace(fd);
}

/*
 * Handler of the SIGALRM signal: dump the backtrace of the current thread or
 * of all threads if fault_alarm.all_threads is true. On success, register
 * itself again if fault_alarm.repeat is true.
 */
void
faulthandler_alarm(int signum)
{
    int ok;
    PyThreadState *current_thread;

    if (fault_alarm.all_threads) {
        const char* errmsg;

        /* PyThreadState_Get() doesn't give the state of the current thread if
           the thread doesn't hold the GIL. Read the thread local storage (TLS)
           instead: call PyGILState_GetThisThreadState(). */
        current_thread = PyGILState_GetThisThreadState();
        if (current_thread == NULL) {
            /* unable to get the current thread, do nothing */
            return;
        }
        errmsg = faulthandler_dump_backtrace_threads(fault_alarm.fd,
                                                     current_thread);
        ok = (errmsg == NULL);
    }
    else {
        faulthandler_dump_backtrace(fault_alarm.fd);
        ok = 1;
    }

    if (ok && fault_alarm.repeat)
        alarm(fault_alarm.delay);
    else
        faulthandler_cancel_dumpbacktrace_later();
}

void
faulthandler_init()
{
    faulthandler_enabled = 0;
#ifdef HAVE_SIGALTSTACK
    stack.ss_sp = NULL;
#endif
}

static void
faulthandler_unload(void)
{
    faulthandler_cancel_dumpbacktrace_later();
#ifdef HAVE_SIGALTSTACK
    if (stack.ss_sp != NULL) {
        PyMem_Free(stack.ss_sp);
        stack.ss_sp = NULL;
    }
#endif
}

int
get_stderr()
{
    fflush(stderr);
    return fileno(stderr);
}

PyObject*
faulthandler_enable(PyObject *self)
{
    unsigned int i;
    fault_handler_t *handler;
#ifdef HAVE_SIGACTION
    struct sigaction action;
    int err;
#endif

    if (faulthandler_enabled)
        Py_RETURN_NONE;

    fatal_error_fd = get_stderr();
    if (fatal_error_fd == -1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "unable to get the file descriptor "
                        "of the standard error");
        return NULL;
    }

    faulthandler_enabled = 1;

#ifdef HAVE_SIGALTSTACK
    /* Try to allocate an alternate stack for faulthandler() signal handler to
     * be able to allocate memory on the stack, even on a stack overflow. If it
     * fails, ignore the error. */
    stack.ss_flags = SS_ONSTACK;
    stack.ss_size = SIGSTKSZ;
    stack.ss_sp = PyMem_Malloc(stack.ss_size);
    if (stack.ss_sp != NULL) {
        (void)sigaltstack(&stack, NULL);
    }
#endif
    (void)Py_AtExit(faulthandler_unload);

    for (i=0; i < NFAULT_SIGNALS; i++) {
        handler = &fault_handlers[i];
        handler->signum = fault_signals[i];;
        handler->enabled = 0;
        if (handler->signum == SIGFPE)
            handler->name = "Floating point exception";
#ifdef SIGBUS
        else if (handler->signum == SIGBUS)
            handler->name = "Bus error";
#endif
#ifdef SIGILL
        else if (handler->signum == SIGILL)
            handler->name = "Illegal instruction";
#endif
        else
            handler->name = "Segmentation fault";
    }

    for (i=0; i < NFAULT_SIGNALS; i++) {
        handler = &fault_handlers[i];
#ifdef HAVE_SIGACTION
        action.sa_handler = faulthandler_fatal_error;
        sigemptyset(&action.sa_mask);
        action.sa_flags = SA_ONSTACK;
        err = sigaction(handler->signum, &action, &handler->previous);
        if (!err)
            handler->enabled = 1;
#else
        handler->previous = signal(handler->signum, faulthandler_fatal_error);
        if (handler->previous != SIG_ERR)
            handler->enabled = 1;
#endif
    }
    Py_RETURN_NONE;
}

PyObject*
faulthandler_disable(PyObject *self)
{
    unsigned int i;
    fault_handler_t *handler;

    if (!faulthandler_enabled)
        goto exit;
    faulthandler_enabled = 0;

    for (i=0; i < NFAULT_SIGNALS; i++) {
        handler = &fault_handlers[i];
        if (!handler->enabled)
            continue;
#ifdef HAVE_SIGACTION
        (void)sigaction(handler->signum, &handler->previous, NULL);
#else
        (void)signal(handler->signum, handler->previous);
#endif
        handler->enabled = 0;
    }

exit:
    Py_RETURN_NONE;
}

PyObject*
faulthandler_isenabled(PyObject *self)
{
    return PyBool_FromLong(faulthandler_enabled);
}

PyObject*
faulthandler_dumpbacktrace_later(PyObject *self, PyObject *args, PyObject *kwargs)
{
    static char *kwlist[] = {"delay", "repeat", "all_threads", NULL};
    int delay;
    PyOS_sighandler_t previous;
    int repeat = 0;
    int all_threads = 0;
    int fd;

    if (!PyArg_ParseTupleAndKeywords(args, kwargs,
        "i|ii:dump_backtrace_later", kwlist,
        &delay, &repeat, &all_threads))
        return NULL;
    if (delay <= 0) {
        PyErr_SetString(PyExc_ValueError, "delay must be greater than 0");
        return NULL;
    }

    fd = get_stderr();
    if (fd == -1) {
        PyErr_SetString(PyExc_RuntimeError,
                        "unable to get stderr file descriptor");
        return NULL;
    }

    previous = signal(SIGALRM, faulthandler_alarm);
    if (previous == SIG_ERR) {
        PyErr_SetString(PyExc_RuntimeError, "unable to set SIGALRM handler");
        return NULL;
    }

    fault_alarm.fd = fd;
    fault_alarm.delay = delay;
    fault_alarm.repeat = repeat;
    fault_alarm.all_threads = all_threads;

    alarm(delay);

    Py_RETURN_NONE;
}

void
faulthandler_cancel_dumpbacktrace_later()
{
    alarm(0);
}

PyObject*
faulthandler_cancel_dumpbacktrace_later_py(PyObject *self)
{
    faulthandler_cancel_dumpbacktrace_later();
    Py_RETURN_NONE;
}

