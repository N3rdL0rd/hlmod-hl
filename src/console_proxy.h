#ifndef HLMOD_CONSOLE_PROXY_H
#define HLMOD_CONSOLE_PROXY_H

/* On Windows, Python's own `sys.stdout`/`sys.stderr` don't share hlmod's C
 * stdout stream cleanly (buffering/encoding differ), so mod print output can
 * interleave incorrectly with hlmod's own `[hlmod] ...` logging. Installs a
 * small Python file-like object that forwards writes to C stdout instead.
 * No-op on other platforms. */
void hlmod_setup_pyio(void);

#endif // HLMOD_CONSOLE_PROXY_H
