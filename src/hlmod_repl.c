/*
 * A loopback-only TCP REPL server, letting a developer attach a live text
 * session to a running game and evaluate Python or call hooked functions.
 * Modeled directly on HL's own debugger.c: a single background thread
 * accepts connections one at a time via HL's own hl_socket_* API (so the
 * listening/client sockets are GC-managed objects, rooted from C, exactly
 * like debug_socket/client_socket there), and every actual command is
 * dispatched into modcore._repl under the GIL - this file only owns
 * framing and shutdown coordination, never REPL semantics.
 */
#include "hlmod_repl.h"
#include "hlmod_python.h"
#include <hl.h>
#include <Python.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Not exposed via any shared header - std/socket.c's own natives, declared
 * the same way HL's own debugger.c declares them. */
typedef struct hl_socket hl_socket;
HL_API void hl_socket_init(void);
HL_API hl_socket *hl_socket_new(bool udp);
HL_API bool hl_socket_bind(hl_socket *s, int host, int port);
HL_API bool hl_socket_listen(hl_socket *s, int n);
HL_API void hl_socket_close(hl_socket *s);
HL_API hl_socket *hl_socket_accept(hl_socket *s);
HL_API int hl_socket_send(hl_socket *s, vbyte *buf, int pos, int len);
HL_API int hl_socket_recv(hl_socket *s, vbyte *buf, int pos, int len);
HL_API void hl_sys_sleep(double t);

#define REPL_LINE_MAX 65536
#define REPL_LOCALHOST 0x0100007F /* 127.0.0.1, network byte order - never bind wider */
#define REPL_STOP_WAIT_ITERATIONS 200 /* ~2s bounded wait; see hlmod_repl_stop */

static hl_socket *g_repl_listen = NULL;
static hl_socket *g_repl_client = NULL;
static volatile bool g_repl_stop_requested = false;
static volatile bool g_repl_stopped = true;
static int g_next_connection_id = 1;

/* Calls modcore._repl.<method>(connection_id[, line]) under the GIL.
 * Returns a newly malloc'd UTF-8 response the caller must free, or NULL if
 * there is nothing to send back (either the call returned None, or it
 * failed - failures are logged to stderr and never crash the connection
 * loop or the game). */
static char *hlmod_repl_call(const char *method, int connection_id, const char *line) {
    char *result = NULL;
    hl_blocking(true);
    PyGILState_STATE gstate = PyGILState_Ensure();
    hl_blocking(false);

    PyObject *module = PyImport_ImportModule("modcore._repl");
    if (module == NULL) {
        char *error = hlmod_python_take_error();
        fprintf(stderr, "[hlmod] REPL: could not import modcore._repl: %s\n", error ? error : "(unknown)");
        free(error);
        goto done;
    }
    PyObject *py_result = line != NULL
        ? PyObject_CallMethod(module, method, "is", connection_id, line)
        : PyObject_CallMethod(module, method, "i", connection_id);
    Py_DECREF(module);
    if (py_result == NULL) {
        char *error = hlmod_python_take_error();
        fprintf(stderr, "[hlmod] REPL: %s() failed: %s\n", method, error ? error : "(unknown)");
        free(error);
        goto done;
    }
    if (py_result != Py_None) {
        const char *utf8 = PyUnicode_AsUTF8(py_result);
        if (utf8 != NULL) result = strdup(utf8);
    }
    Py_DECREF(py_result);
done:
    hl_blocking(true);
    PyGILState_Release(gstate);
    hl_blocking(false);
    return result;
}

static bool hlmod_repl_send(hl_socket *client, const char *text) {
    int len = (int)strlen(text);
    int sent = 0;
    while (sent < len) {
        int n = hl_socket_send(client, (vbyte *)(text + sent), 0, len - sent);
        if (n <= 0) return false;
        sent += n;
    }
    return true;
}

/* Serves exactly one client to completion (until it disconnects, errors, or
 * shutdown is requested), then returns so the accept loop can move on. */
static void hlmod_repl_serve_connection(hl_socket *client) {
    int connection_id = g_next_connection_id++;
    g_repl_client = client;

    char *greeting = hlmod_repl_call("open_connection", connection_id, NULL);
    if (greeting != NULL) {
        hlmod_repl_send(client, greeting);
        free(greeting);
    }

    char line[REPL_LINE_MAX];
    int line_len = 0;
    vbyte chunk[4096];
    while (!g_repl_stop_requested) {
        int n = hl_socket_recv(client, chunk, 0, sizeof(chunk));
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            char c = (char)chunk[i];
            if (c == '\n') {
                line[line_len] = 0;
                char *response = hlmod_repl_call("handle_line", connection_id, line);
                line_len = 0;
                if (response == NULL) goto done;
                bool ok = hlmod_repl_send(client, response);
                free(response);
                if (!ok) goto done;
            } else if (c != '\r' && line_len + 1 < (int)sizeof(line)) {
                line[line_len++] = c;
            }
        }
    }
done:
    {
        char *ignored = hlmod_repl_call("close_connection", connection_id, NULL);
        free(ignored);
    }
    g_repl_client = NULL;
    hl_socket_close(client);
}

static void hlmod_repl_loop(void *unused) {
    (void)unused;
    hl_get_thread()->flags |= HL_THREAD_INVISIBLE;
    while (!g_repl_stop_requested) {
        hl_socket *client = hl_socket_accept(g_repl_listen);
        if (client == NULL) break; /* listen socket closed -> shutting down */
        hlmod_repl_serve_connection(client);
    }
    g_repl_stopped = true;
}

bool hlmod_repl_start(int port) {
    hl_socket_init();
    hl_socket *s = hl_socket_new(false);
    if (s == NULL) return false;
    if (!hl_socket_bind(s, REPL_LOCALHOST, port) || !hl_socket_listen(s, 4)) {
        hl_socket_close(s);
        return false;
    }
    g_repl_listen = s;
    hl_add_root(&g_repl_listen);
    hl_add_root(&g_repl_client);
    g_repl_stop_requested = false;
    g_repl_stopped = false;
    if (!hl_thread_start(hlmod_repl_loop, NULL, true)) {
        hl_remove_root(&g_repl_listen);
        hl_remove_root(&g_repl_client);
        hl_socket_close(s);
        g_repl_listen = NULL;
        return false;
    }
    printf("[hlmod] REPL listening on 127.0.0.1:%d\n", port);
    fflush(stdout); /* stdout is fully buffered (not line-buffered) when
                      * piped, e.g. by tooling waiting on this banner to
                      * know when it's safe to connect. */
    return true;
}

void hlmod_repl_stop(void) {
    if (g_repl_listen == NULL) return;
    g_repl_stop_requested = true;
    hl_socket_close(g_repl_listen);
    if (g_repl_client != NULL) hl_socket_close(g_repl_client);
    /* Bounded, not indefinite: an idle client can hold the loop inside a
     * blocking recv() that closing the listen socket alone won't wake.
     * Closing the client socket above usually breaks it out promptly, but
     * we must never hang process shutdown waiting on a REPL peer. */
    for (int waited = 0; !g_repl_stopped && waited < REPL_STOP_WAIT_ITERATIONS; waited++)
        hl_sys_sleep(0.01);
    hl_remove_root(&g_repl_listen);
    hl_remove_root(&g_repl_client);
    g_repl_listen = NULL;
}
