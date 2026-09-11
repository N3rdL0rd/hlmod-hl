#ifndef HLMOD_REPL_H
#define HLMOD_REPL_H

#include <stdbool.h>

/**
 * @brief Starts the REPL TCP server on 127.0.0.1:`port` in a background
 * thread. Never binds to anything but loopback - the REPL runs arbitrary
 * Python, so it must never be reachable from the network.
 * @return false if the socket could not be bound/listened on or the
 * background thread could not start.
 */
bool hlmod_repl_start(int port);

/**
 * @brief Stops accepting new connections, closes any connection in
 * progress, and waits briefly (bounded, never indefinitely) for the
 * background thread to notice and exit. Safe to call even if
 * hlmod_repl_start was never called or failed. Must be called before
 * Py_FinalizeEx(), since the REPL thread calls into Python.
 */
void hlmod_repl_stop(void);

#endif // HLMOD_REPL_H
