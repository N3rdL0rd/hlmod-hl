#ifndef HLMOD_INTERNAL_H
#define HLMOD_INTERNAL_H

/* Shared internal state and helpers for the hlmod.c family of translation
 * units (hlmod.c, hlmod_ptr.c, hlmod_cast.c, hlmod_fields.c,
 * hlmod_jit_hook.c, hlmod_api.c). This is a private split of what used to be
 * a single 2000+ line hlmod.c; nothing outside this family should include
 * it - use hlmod.h/hlmod_python.h instead. */

#include <hl.h>
#include <hlmod.h>
#include <Python.h>

/* Bytecode type <-> index and findex <-> HFUN-type helpers, used throughout
 * casting, field access, and reflection. */
int hlmod_type_index(hl_type *type);
hl_type *hlmod_function_type(int findex);
bool uchar_eq(const uchar *s1, const uchar *s2);

/* Findexes currently being called directly on this thread (bypassing their
 * own hook chain), e.g. from `HookContext.call_next()`; consulted by the JIT
 * hook dispatcher so a passthrough call never re-triggers its own hook. */
int push_passthrough(int findex);
void pop_passthrough(void);
bool is_passthrough(int findex);

/* type_index -> registered Python class, populated by register_hlobj and
 * consulted by casting/field-access/reflection to find the right wrapper
 * class for a native object. */
extern PyObject **g_hlobjs;
extern int g_hlobjs_l;
extern PyObject *g_hlobj_module;
extern PyObject *g_hlcallable_class;
extern PyObject *g_hlvirtual_class;

/* Synthesized HFUN-kind views of HMETHOD types, cached per bytecode type and
 * freed with the bytecode allocator; used wherever a method type needs to be
 * treated as an ordinary function type (e.g. building a vclosure). */
typedef struct HlMethodSignature {
    hl_type *method;
    hl_type callable;
    struct HlMethodSignature *next;
} HlMethodSignature;
extern HlMethodSignature *g_method_signatures;

/* Cross-file helpers defined in hlmod_ptr.c. */
PyObject *hlmod_py_make_hlcallable(vclosure *cl);
PyObject *hlmod_py_make_hlvirtual(void *ptr, hl_type *type);
PyObject *hlmod_invoke(vclosure *closure, PyObject *arguments, int original, int direct);

/* Cross-file helpers defined in hlmod_cast.c. */
HlPtr *hlmod_extract_pointer(PyObject *obj);
void *hlmod_require_pointer(HlPtr *pointer, hl_type_kind kind);

#endif // HLMOD_INTERNAL_H
