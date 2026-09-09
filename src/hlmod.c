#include <hlmod_python.h>
#include "hlmod_internal.h"

#include <stdlib.h>

int hlmod_type_index(hl_type *type)
{
    if (!g_code || !type) return -1;
    uintptr_t address = (uintptr_t)type, base = (uintptr_t)g_code->types;
    if (address < base || (address - base) / sizeof(hl_type) >= (size_t)g_code->ntypes ||
        (address - base) % sizeof(hl_type)) return -1;
    return (int)((address - base) / sizeof(hl_type));
}

hl_type *hlmod_function_type(int findex)
{
    if (!g_module || !g_module->code) {
        PyErr_SetString(PyExc_RuntimeError, "hlmod is not initialized.");
        return NULL;
    }
    if (findex < 0 || findex >= g_module->code->nfunctions + g_module->code->nnatives) {
        PyErr_Format(PyExc_IndexError, "Function index %d is out of bounds.", findex);
        return NULL;
    }
    hl_type *type = g_module->ctx.functions_types[findex];
    if (!type || type->kind != HFUN || !type->fun || !g_module->functions_ptrs[findex]) {
        PyErr_SetString(PyExc_TypeError, "Function metadata is unavailable.");
        return NULL;
    }
    return type;
}

bool uchar_eq(const uchar *s1, const uchar *s2)
{
    while (*s1 != u'\0' && *s2 != u'\0')
    {
        if (*s1 != *s2)
        {
            return false;
        }
        s1++;
        s2++;
    }
    return *s1 == *s2;
}

THREAD_LOCAL int64_t g_return_value_int = 0;
THREAD_LOCAL double g_return_value_double = 0.0;
static THREAD_LOCAL int* g_passthrough_stack = NULL;
static THREAD_LOCAL int g_passthrough_stack_size = 0;
static THREAD_LOCAL int g_passthrough_stack_capacity = 0;

int push_passthrough(int findex) {
    if (g_passthrough_stack_size >= g_passthrough_stack_capacity) {
        int new_capacity = g_passthrough_stack_capacity == 0 ? 8 : g_passthrough_stack_capacity * 2;
        int *stack = realloc(g_passthrough_stack, new_capacity * sizeof(int));
        if (!stack) { PyErr_NoMemory(); return -1; }
        g_passthrough_stack = stack;
        g_passthrough_stack_capacity = new_capacity;
    }
    g_passthrough_stack[g_passthrough_stack_size++] = findex;
    return 0;
}

void pop_passthrough() {
    if (g_passthrough_stack_size > 0) {
        g_passthrough_stack_size--;
    }
}

bool is_passthrough(int findex) {
    for (int i = 0; i < g_passthrough_stack_size; i++) {
        if (g_passthrough_stack[i] == findex) {
            return true;
        }
    }
    return false;
}

EXPORT int64_t hlmod_get_return_int() {
    return g_return_value_int;
}

EXPORT double hlmod_get_return_double() {
    return g_return_value_double;
}

PyObject **g_hlobjs = NULL;
int g_hlobjs_l = 0;
PyObject *g_hlobj_module = NULL;
PyObject *g_hlcallable_class = NULL;
PyObject *g_hlvirtual_class = NULL;
HlMethodSignature *g_method_signatures = NULL;

void hlmod_shutdown(void)
{
    PyObject **classes = g_hlobjs;
    int count = g_hlobjs_l;
    g_hlobjs = NULL;
    g_hlobjs_l = 0;
    g_method_signatures = NULL; /* Storage belongs to the bytecode allocator. */
    for (int i = 0; i < count; i++) Py_XDECREF(classes[i]);
    free(classes);
    Py_CLEAR(g_hlcallable_class);
    Py_CLEAR(g_hlvirtual_class);
    Py_CLEAR(g_hlobj_module);
    hlmod_hook_registry_shutdown();
    free(g_passthrough_stack);
    g_passthrough_stack = NULL;
    g_passthrough_stack_size = 0;
    g_passthrough_stack_capacity = 0;
    g_return_value_int = 0;
    g_return_value_double = 0;
}

