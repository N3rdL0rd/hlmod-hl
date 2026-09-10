"""Render importable HashLink proxies from native metadata, without runtime access."""
from __future__ import annotations

import ast
import builtins
import hashlib
import json
import keyword
import os
import typing
from pathlib import Path


OBJ, STRUCT, VIRTUAL, FUN, METHOD, BYTES = 11, 21, 15, 10, 20, 8


def escaped_identifier(name: str) -> str:
    return "_hx_" + name.encode("utf-8").hex()


def identifier(name: str) -> str:
    # Keep historical dollar/keyword spellings without conflating real Haxe
    # identifiers with those spellings or with the escape namespace.
    if (name.startswith("_hx_") or "S_" in name
            or (name.endswith("_") and keyword.iskeyword(name[:-1]))
            or (name.startswith("__") and not name.endswith("__"))):
        return escaped_identifier(name)
    safe = name.replace("$", "S_")
    if not safe.isidentifier():
        return escaped_identifier(name)
    return safe + "_" if keyword.iskeyword(safe) else safe


GLOBAL_NAMES = frozenset(dir(builtins)) | {
    "hlmod", "HlPtr", "HlObject", "HlVirtual", "HlArray", "HlBytes", "HlEnum", "HlDynObject", "HlRef", "hltype", "hlfunction",
    "Any", "Callable", "ClassVar", "Never", "TYPE_CHECKING",
}
MEMBER_NAMES = {
    "STATIC", "__new__", "__init__", "__getattribute__", "__getattr__",
    "__setattr__", "__delattr__", "__class__", "__dict__", "__slots__",
    "__weakref__", "__annotations__", "__module__", "__qualname__", "__doc__",
    "__repr__", "__str__", "__hash__", "__eq__", "__del__", "__init_subclass__",
}


def global_identifier(name: str) -> str:
    safe = identifier(name)
    if safe in GLOBAL_NAMES or safe.startswith(("_hl_", "_hlmod_", "_T", "_Base", "__")):
        return escaped_identifier(name)
    return safe


def member_identifier(name: str) -> str:
    safe = identifier(name)
    if safe in MEMBER_NAMES or safe.startswith(("_hl_", "_hlmod_")):
        return escaped_identifier(name)
    return safe


class Renderer:
    def __init__(self, metadata: dict, overlay: dict | None = None):
        self.types = {t["index"]: t for t in metadata["types"]}
        self.functions = {f["findex"]: f for f in metadata["functions"]}
        self.docs = metadata.get("docs", {})
        self.constructors = {entry["static_type"]: entry["findex"] for entry in metadata.get("constructors", [])}
        self.locations = {}
        self.pairs = {}
        self.instance_pairs = {}
        self.named = {
            t["name"]: t for t in self.types.values()
            if t["kind"] == OBJ and t["name"] != "String"
        }
        for name, t in self.named.items():
            package, _, short = name.rpartition(".")
            pair = self.named.get((package + "." if package else "") + "$" + short)
            if pair is not None and not short.startswith("$"):
                self.pairs[t["index"]] = pair["index"]
                self.instance_pairs[pair["index"]] = t["index"]
        occupied = set()
        for index, t in self.types.items():
            if t["kind"] not in (OBJ, VIRTUAL) or t.get("name") == "String":
                continue
            if index in self.instance_pairs:
                continue
            parts = ("hl", "virtuals", f"Virtual_{index}") if t["kind"] == VIRTUAL else tuple(
                identifier(part) for part in t["name"].split("."))
            parts = (*parts[:-1], global_identifier(t["name"].rsplit(".", 1)[-1])) if t["kind"] != VIRTUAL else parts
            while parts in occupied:
                parts = (*parts[:-1], f"{parts[-1]}_{index}")
            occupied.add(parts)
            self.locations[index] = ("stubs." + ".".join(parts), parts[-1])
        for static, instance in self.instance_pairs.items():
            module, name = self.locations[instance]
            self.locations[static] = (module, f"_{name}Static")
        self.overlay = {} if overlay is None else overlay
        self.validate_overlay()

    def validate_overlay(self) -> None:
        """Accept data-only annotation patches; never import or execute user code."""
        overlay = self.overlay
        if not isinstance(overlay, dict) or set(overlay) - {"version", "imports", "types"}:
            raise ValueError("Typing overlay must contain only version, imports, and types")
        if overlay and (type(overlay.get("version")) is not int or overlay["version"] != 1):
            raise ValueError("Typing overlay version must be 1")
        imports = overlay.get("imports", {})
        targets = overlay.get("types", {})
        if not isinstance(imports, dict) or not isinstance(targets, dict):
            raise ValueError("Typing overlay imports and types must be objects")
        exports = {f"{module}.{name}" for module, name in self.locations.values()}
        exports.update("hlobj." + name for name in (
            "HlArray", "HlBytes", "HlObject", "HlVirtual", "HlEnum", "HlDynObject", "HlRef", "HlCallable"))
        exports.add("hlmod.HlPtr")
        exports.update("typing." + name for name in typing.__all__)
        reserved = GLOBAL_NAMES | {name for _, name in self.locations.values()}
        for alias, source in imports.items():
            if (not isinstance(alias, str) or not alias.isidentifier() or keyword.iskeyword(alias)
                    or alias.startswith("_") or alias in reserved):
                raise ValueError(f"Invalid or conflicting overlay import alias: {alias!r}")
            if not isinstance(source, str) or source not in exports:
                raise ValueError(f"Unknown overlay import: {source!r}")
        allowed_names = {"Any", "Callable", "Never", "int", "str", "float", "bool", "bytes",
                         "list", "dict", "tuple", "set", "frozenset", "object", "type",
                         "HlArray", "HlBytes", "HlEnum", "HlDynObject", "HlRef", "HlPtr", "HlObject", "HlVirtual"} | imports.keys()
        self.overlay_names = allowed_names
        for native_name, patch in targets.items():
            t = self.named.get(native_name)
            if t is None or not isinstance(patch, dict) or set(patch) - {"fields", "methods"}:
                raise ValueError(f"Unknown overlay type or invalid patch: {native_name!r}")
            methods = self.methods(t)
            fields = {field["name"] for field in t.get("fields", [])} - methods.keys()
            for category in ("fields", "methods"):
                entries = patch.get(category, {})
                if not isinstance(entries, dict):
                    raise ValueError(f"{native_name}.{category} must be an object")
                for name, value in entries.items():
                    target = f"{native_name}.{name}"
                    if category == "fields":
                        if name not in fields:
                            raise ValueError(f"Unknown overlay field: {target}")
                        self.overlay_annotation(value)
                        continue
                    constructor = self.constructors.get(self.pairs.get(t["index"])) if name == "new" else None
                    method = methods.get(name)
                    if method is None and constructor is None:
                        raise ValueError(f"Unknown overlay method: {target}")
                    if not isinstance(value, dict) or set(value) != {"args", "returns"} or not isinstance(value["args"], list):
                        raise ValueError(f"{target} requires args (annotation list) and returns")
                    function = self.functions[constructor if constructor is not None else method["findex"]]
                    signature = self.types[function["type"]]
                    skip = 1
                    if constructor is not None:
                        skip = int(bool(signature["args"]) and signature["args"][0] == t["index"]
                                   and self.types[signature["return"]]["kind"] == 0)
                    elif "field_type" in method:
                        skip = len(signature["args"]) - len(self.types[method["field_type"]]["args"])
                    if len(value["args"]) != len(signature["args"]) - skip:
                        raise ValueError(f"Overlay argument count differs from native signature: {target}")
                    if name == "new" and value["returns"] != "None":
                        raise ValueError(f"Constructor overlay must return None: {target}")
                    for annotation in [*value["args"], value["returns"]]:
                        self.overlay_annotation(annotation)

    def overlay_annotation(self, text: str) -> ast.expr:
        if not isinstance(text, str) or not text.strip():
            raise ValueError("Overlay annotations must be nonempty strings")
        try:
            expression = ast.parse(text, mode="eval").body
        except SyntaxError as error:
            raise ValueError(f"Invalid overlay annotation: {text!r}") from error
        for node in ast.walk(expression):
            if not isinstance(node, (ast.Name, ast.Subscript, ast.Tuple, ast.List, ast.BinOp,
                                     ast.BitOr, ast.Constant, ast.Load)):
                raise ValueError(f"Forbidden overlay annotation syntax: {text!r}")
            if isinstance(node, ast.Name) and node.id not in self.overlay_names:
                raise ValueError(f"Unknown overlay annotation name: {node.id}")
            if isinstance(node, ast.Constant) and node.value is not None and node.value is not Ellipsis:
                raise ValueError("Overlay annotations do not permit literals or string forward references")
        return expression

    def editor_source(self, source: str, index: int) -> str:
        """Strip runtime machinery while retaining real method declarations."""
        tree = ast.parse(source)
        classes = {self.locations[index][1]: self.types[index]}
        if index in self.pairs:
            static = self.pairs[index]
            classes[self.locations[static][1]] = self.types[static]
        for node in tree.body:
            if not isinstance(node, ast.ClassDef):
                continue
            t = classes[node.name]
            patch = self.overlay.get("types", {}).get(t.get("name"), {})
            field_patches = {member_identifier(name): value for name, value in patch.get("fields", {}).items()}
            method_patches = {"__init__" if name == "new" else member_identifier(name): value
                              for name, value in patch.get("methods", {}).items()}
            static_index = self.pairs.get(t["index"])
            if static_index is not None:
                static_patch = self.overlay.get("types", {}).get(self.types[static_index]["name"], {})
                for name, value in static_patch.get("fields", {}).items():
                    field_patches.setdefault(member_identifier(name), value)
                for name, value in static_patch.get("methods", {}).items():
                    method_patches.setdefault(member_identifier(name), value)
            dynamic = {member_identifier(name) for name, method in self.methods(t).items() if "field" in method}
            if t["index"] in self.instance_pairs or t.get("name", "").rsplit(".", 1)[-1].startswith("$"):
                dynamic = set()
            node.decorator_list = []
            body = []
            for member in node.body:
                if isinstance(member, ast.Assign):
                    continue
                if isinstance(member, ast.AnnAssign):
                    patch_text = field_patches.get(member.target.id)
                    if patch_text is not None:
                        annotation = self.overlay_annotation(patch_text)
                        if isinstance(member.annotation, ast.Subscript) and isinstance(member.annotation.value, ast.Name) and member.annotation.value.id == "ClassVar":
                            member.annotation.slice = annotation
                        else:
                            member.annotation = annotation
                if isinstance(member, ast.FunctionDef):
                    first = member.body[0] if member.body else None
                    docstring = (first if isinstance(first, ast.Expr) and isinstance(first.value, ast.Constant)
                                and isinstance(first.value.value, str) else None)
                    member.body = ([docstring] if docstring else []) + [ast.Expr(value=ast.Constant(value=Ellipsis))]
                    member.decorator_list = [decorator for decorator in member.decorator_list
                                             if isinstance(decorator, ast.Attribute) and decorator.attr == "staticmethod"]
                    static = bool(member.decorator_list)
                    patch_method = method_patches.get(member.name)
                    if patch_method is not None:
                        for parameter, annotation in zip(member.args.args[0 if static else 1:], patch_method["args"]):
                            parameter.annotation = self.overlay_annotation(annotation)
                        member.returns = self.overlay_annotation(patch_method["returns"])
                    if member.name == "__init__" and member.args.vararg is not None:
                        # There is no native constructor: accepting arbitrary args would lie.
                        member.args.vararg = None
                        member.args.kwarg = None
                        member.args.args.append(ast.arg(arg="_unavailable", annotation=ast.Name(id="Never", ctx=ast.Load())))
                    if member.name in dynamic:
                        args = ", ".join(ast.unparse(arg.annotation) for arg in member.args.args[1:])
                        annotation = ast.parse(f"Callable[[{args}], {ast.unparse(member.returns)}]", mode="eval").body
                        member = ast.AnnAssign(target=ast.Name(id=member.name, ctx=ast.Store()), annotation=annotation, value=None, simple=1)
                if isinstance(member, ast.FunctionDef):
                    # HL calls are positional: parameter names are not a contract,
                    # so hook callbacks must not be forced to reuse these names.
                    member.args.posonlyargs = member.args.posonlyargs + member.args.args
                    member.args.args = []
                body.append(member)
            node.body = body or [ast.Expr(value=ast.Constant(value=Ellipsis))]
        for alias, source in sorted(self.overlay.get("imports", {}).items()):
            module, _, name = source.rpartition(".")
            tree.body.insert(1, ast.ImportFrom(module=module, names=[ast.alias(name=name, asname=alias)], level=0))
        return "# Generated editor interface; typing overlays apply only here.\n" + ast.unparse(ast.fix_missing_locations(tree)) + "\n"

    def annotation(self, index: int, used: set[int], active: frozenset[int] = frozenset(), *, argument: bool = False, bound_method: bool = False) -> str:
        t = self.types.get(index)
        if t is None or index in active:
            return "Any"
        kind = t["kind"]
        simple = {0: "None", 1: "int", 2: "int", 3: "int", 4: "int", 5: "float", 6: "float",
                  7: "bool", 8: "HlBytes", 9: "Any", 12: "HlArray[Any]", 13: "HlPtr",
                  14: "HlRef[Any]", 16: "HlDynObject", 17: "HlPtr", 18: "HlEnum", 20: "Never",
                  21: "HlPtr", 22: "Never", 23: "Never"}
        if kind == BYTES and argument:
            # HL copies a Python buffer; HlBytes shares the native allocation.
            return "HlBytes | bytes | bytearray | memoryview | None"
        if kind == STRUCT and argument:
            return "Never"
        if kind == 19:
            inner = self.annotation(t["param"], used, active | {index}, argument=argument)
            return inner if inner in ("Any", "None") or inner.endswith(" | None") else f"{inner} | None"
        if t.get("name") == "String" and kind == OBJ:
            result = "str"
        elif kind in simple and not (kind == METHOD and bound_method):
            result = simple[kind]
        elif kind in (FUN, METHOD):
            parameters = t["args"][1:] if kind == METHOD and bound_method else t["args"]
            args = ", ".join(self.annotation(arg, used, active | {index}, argument=True) for arg in parameters)
            returns = self.annotation(t["return"], used, active | {index})
            result = f"Callable[[{args}], {returns}]"
        elif index in self.locations:
            used.add(index)
            result = f"_T{index}"
        else:
            result = "HlPtr"
        # HL bytecode does not carry non-null guarantees for pointer values.
        if kind in (8, FUN, OBJ, 12, 13, 14, VIRTUAL, 16, 17, 18, STRUCT) and result != "Never":
            result += " | None"
        return result

    def fields(self, t: dict) -> list[dict]:
        parent = self.types.get(t.get("super", -1))
        return (self.fields(parent) if parent else []) + t.get("fields", [])

    def methods(self, t: dict) -> dict[str, dict]:
        result = {}
        fields = self.fields(t)
        for binding in t.get("bindings", []):
            field = fields[binding["field"]]
            if self.types[field["type"]]["kind"] in (FUN, METHOD):
                result[field["name"]] = {**binding, "name": field["name"], "field_type": field["type"]}
        result.update((m["name"], m) for m in t.get("methods", []))
        return result

    def member_doc(self, t: dict, name: str, category: str) -> str | None:
        return self.docs.get(t.get("name"), {}).get(category, {}).get(name)

    def signature(self, func: dict, used: set[int], skip: int, owner: str | None = None) -> tuple[str, list[str], str]:
        signature = self.types[func["type"]]
        names = func.get("arg_names", [])
        taken = {"self", "cls", "hlmod", owner}
        arguments = []
        parameters = []
        for i, arg in enumerate(signature["args"][skip:]):
            name = identifier(names[i] if i < len(names) else f"arg{i}")
            while name in taken:
                name += "_"
            taken.add(name)
            arguments.append(name)
            parameters.append(f"{name}: {self.annotation(arg, used, argument=True)}")
        return ", ".join(parameters), arguments, self.annotation(signature["return"], used)

    def method_source(self, t: dict, method: dict, used: set[int], *, constructor: bool = False,
                      static_owner: str | None = None) -> list[str]:
        func = self.functions.get(method["findex"])
        if func is None:
            raise ValueError(f"No signature for function {method['findex']}")
        skip = 1
        if "field_type" in method:
            field_type = self.types[method["field_type"]]
            skip = len(self.types[func["type"]]["args"]) - len(field_type["args"])
            if skip not in (0, 1):
                raise ValueError(f"Incompatible bound method {t.get('name')}.{method['name']}")
        params, args, result = self.signature(func, used, skip, static_owner)
        name = "__init__" if constructor else member_identifier(method["name"])
        if constructor:
            result = "None"
        prefix = []
        if static_owner:
            prefix.append("    @_hlmod_builtins.staticmethod")
            parameters = params
        else:
            parameters = "self" + (", " + params if params else "")
        if not constructor:
            prefix.append(f"    @hlfunction({method['findex']})")
        prefix.append(f"    def {name}({parameters}) -> {result}:")
        doc = self.member_doc(t, method["name"], "functions")
        location = f"{func['file']}:{func['line']}" if "file" in func else None
        if location:
            doc = f"{doc}\n\n{location}" if doc else location
        if doc:
            prefix.append(f"        {doc!r}")
        if static_owner:
            call = f"{static_owner}._hlmod_call_static({name!r}" + (", " + ", ".join(args) if args else "") + ")"
        else:
            call_args = (["self"] if skip else []) + args
            tuple_args = "(" + ", ".join(call_args) + ("," if len(call_args) == 1 else "") + ")"
            call = f"hlmod.call({method['findex']}, {tuple_args})"
        prefix.append(f"        {'return ' if not constructor else ''}{call}")
        return prefix + [""]

    def class_source(self, t: dict, used: set[int], imports: set[str]) -> list[str]:
        index = t["index"]
        module, name = self.locations[index]
        parent = t.get("super", -1)
        if index in self.instance_pairs:
            # Runtime static objects inherit hl.Class, which can point back to
            # this very module. Model static API inheritance through the paired
            # instance hierarchy instead, while retaining all native field slots.
            instance = self.types[self.instance_pairs[index]]
            parent = self.pairs.get(instance.get("super", -1), -1)
        elif t.get("name", "").rsplit(".", 1)[-1].startswith("$"):
            parent = -1
        if parent in self.locations:
            parent_module, parent_name = self.locations[parent]
            if parent_module != module:
                imports.add(f"from {parent_module} import {parent_name} as _Base{parent}")
                base = f"_Base{parent}"
            else:
                base = parent_name
        else:
            base = "HlVirtual" if t["kind"] == VIRTUAL else "HlObject"
        lines = [f"@hltype({index})", f"class {name}({base}):"]
        doc = self.docs.get(t.get("name"), {}).get("doc")
        if doc:
            lines.append(f"    {doc!r}")
        lines.extend(["    _hl_generated = True", f"    _hl_type_index = {index}"])
        all_fields = self.fields(t)
        fields = {member_identifier(f["name"]): i for i, f in enumerate(all_fields)}
        lines.append(f"    _hl_fields = {fields!r}")
        methods = self.methods(t)
        method_names = {member_identifier(raw): raw for raw in methods if raw != "new"}
        if parent in self.locations:
            lines.append(f"    _hl_methods = {{**{base}._hl_methods, **{method_names!r}}}")
        else:
            lines.append(f"    _hl_methods = {method_names!r}")
        bound_fields = {member_identifier(raw): method["field"] for raw, method in methods.items() if "field" in method}
        inherited_bound_fields = f"**{base}._hl_bound_fields, " if parent in self.locations else ""
        lines.append(f"    _hl_bound_fields = {{{inherited_bound_fields}**{bound_fields!r}}}")
        static_index = self.pairs.get(index)
        standalone = t.get("name", "").rsplit(".", 1)[-1].startswith("$") and index not in self.instance_pairs
        if standalone:
            static_index = index
        lines.append(f"    _hl_static_tindex = {static_index!r}")
        lines.append("    _hl_static_obj = None")
        if static_index is not None:
            static = self.types[static_index]
            static_fields = {member_identifier(f["name"]): i for i, f in enumerate(self.fields(static))
                             if self.types[f["type"]]["kind"] not in (FUN, METHOD)}
            static_methods = {member_identifier(f["name"]): static["name"] + "." + f["name"]
                              for f in self.fields(static) if self.types[f["type"]]["kind"] in (FUN, METHOD)}
            static_methods.update({member_identifier(m["name"]): static["name"] + "." + m["name"]
                                   for m in static.get("methods", [])})
            lines.extend([f"    _hl_static_fields = {static_fields!r}", f"    _hl_static_methods = {static_methods!r}"])
        lines.append("")
        for field in t.get("fields", []):
            if field["name"] in methods:
                continue
            annotation = self.annotation(field["type"], used, bound_method=t["kind"] == VIRTUAL)
            lines.append(f"    {member_identifier(field['name'])}: {annotation}")
            doc = self.member_doc(t, field["name"], "fields")
            if doc:
                lines.append(f"    {doc!r}")
        lines.append("")
        constructor = self.constructors.get(self.pairs.get(index))
        if constructor is not None:
            function = self.functions[constructor]
            signature = self.types[function["type"]]
            receiver = int(bool(signature["args"]) and signature["args"][0] == index
                           and self.types[signature["return"]]["kind"] == 0)
            params, args, _ = self.signature(function, used, receiver)
            parameters = "self" + (", " + params if params else "")
            arguments = "(" + ", ".join(args) + ("," if len(args) == 1 else "") + ")"
            lines.extend([f"    def __init__({parameters}) -> None:",
                          f"        hlmod.init_obj(self._hlmod_ptr, {constructor}, {arguments})", ""])
        elif t["kind"] == OBJ and index not in self.instance_pairs and not standalone and "new" not in methods:
            lines.extend(["    def __init__(self, *args: Any, **kwargs: Any) -> None:",
                          f"        raise TypeError({'No native constructor metadata for ' + t['name']!r})", ""])
        for method in methods.values():
            if method["name"] == "new":
                lines.extend(self.method_source(t, method, used, constructor=True))
            else:
                lines.extend(self.method_source(t, method, used, static_owner=name if standalone else None))
        if static_index is not None and not standalone:
            static = self.types[static_index]
            used.add(static_index)
            lines.append(f"    STATIC: ClassVar[_T{static_index}]")
            existing = set(fields) | set(method_names)
            static_methods = self.methods(static)
            for field in self.fields(static):
                safe = member_identifier(field["name"])
                if safe not in existing and field["name"] not in static_methods:
                    lines.append(f"    {safe}: ClassVar[{self.annotation(field['type'], used)}]")
            for method in static_methods.values():
                if member_identifier(method["name"]) not in existing:
                    lines.extend(self.method_source(static, method, used, static_owner=name))
        return lines

    def render(self) -> dict[str, str]:
        files = {}
        packages = {"stubs": {}}
        for index, (module, name) in self.locations.items():
            if index in self.instance_pairs:
                continue
            used, imports = set(), set()
            body = []
            if index in self.pairs:
                body.extend(self.class_source(self.types[self.pairs[index]], used, imports))
            body.extend(self.class_source(self.types[index], used, imports))
            header = ["# Generated by hlmod; edit the renderer, not this file.",
                      "from __future__ import annotations",
                      "from typing import Any, Callable, ClassVar, Never, TYPE_CHECKING",
                      "import hlmod", "from hlmod import HlPtr",
                      "import builtins as _hlmod_builtins",
                      "from hlobj import HlArray, HlBytes, HlDynObject, HlEnum, HlRef, HlObject, HlVirtual, hltype, hlfunction", *sorted(imports), ""]
            if used:
                header.append("if TYPE_CHECKING:")
                for dependency in sorted(used):
                    dep_module, dep_name = self.locations[dependency]
                    header.append(f"    from {dep_module} import {dep_name} as _T{dependency}")
                header.append("")
            files[module.removeprefix("stubs.").replace(".", "/") + ".py"] = "\n".join(header + body) + "\n"
            files[module.removeprefix("stubs.").replace(".", "/") + ".pyi"] = self.editor_source("\n".join(header + body), index)
            package = module.rsplit(".", 1)[0]
            packages.setdefault(package, {})[name] = module
            while "." in package:
                package = package.rsplit(".", 1)[0]
                packages.setdefault(package, {})
        for package, exports in sorted(packages.items()):
            path = package.removeprefix("stubs").lstrip(".").replace(".", "/")
            typing_imports = "\n".join(f"    from {module} import {name} as {name}"
                                       for name, module in sorted(exports.items())) or "    pass"
            files[(path + "/" if path else "") + "__init__.py"] = (
                "# Generated by hlmod. Lazy exports avoid package import cycles.\n"
                "from importlib import import_module as _import_module\n"
                "from types import ModuleType as _ModuleType\n"
                "from typing import TYPE_CHECKING\n"
                "import sys as _sys\n"
                f"_exports = {dict(sorted(exports.items()))!r}\n"
                "__all__ = list(_exports)\n"
                f"if TYPE_CHECKING:\n{typing_imports}\n"
                "class _Package(_ModuleType):\n"
                "    def __getattribute__(self, name):\n"
                "        exports = super().__getattribute__('__dict__').get('_exports', {})\n"
                "        if name in exports:\n"
                "            return getattr(_import_module(exports[name]), name)\n"
                "        return super().__getattribute__(name)\n"
                "_sys.modules[__name__].__class__ = _Package\n")
            files[(path + "/" if path else "") + "__init__.pyi"] = "\n".join(
                f"from {module} import {name} as {name}" for name, module in sorted(exports.items())
            ) + "\n"
        return files


def generate(metadata: dict, base_dir: str, source_hash: str) -> None:
    root = Path(base_dir)
    mods_dir = root.parent
    if "docs" not in metadata:
        metadata["docs"] = {}
        for path in (Path("haxe_docs.json"), mods_dir / "haxe_docs.json", Path("std_doc/haxe_docs.json")):
            try:
                with path.open(encoding="utf-8") as source:
                    docs = json.load(source)
                if not isinstance(docs, dict):
                    raise ValueError("documentation root must be an object")
            except FileNotFoundError:
                continue
            except (OSError, ValueError) as error:
                print(f"[hlmod] Could not load documentation {path}: {error}")
                continue
            metadata["docs"] = docs
            break
    overlay_path = Path(os.environ.get("HLMOD_TYPING_OVERLAY", str(mods_dir / "typing_overlays.json")))
    try:
        overlay = json.loads(overlay_path.read_text(encoding="utf-8"))
    except FileNotFoundError:
        if "HLMOD_TYPING_OVERLAY" in os.environ:
            raise
        overlay = {}
    renderer = Renderer(metadata, overlay)
    signature = hashlib.sha256((source_hash + metadata["code_hash"] + json.dumps(
        {"docs": metadata.get("docs", {}), "overlay": overlay}, sort_keys=True, ensure_ascii=True)).encode()).hexdigest()
    manifest_path = root / ".hlmod_generated.json"
    try:
        previous = json.loads(manifest_path.read_text())
    except (FileNotFoundError, ValueError):
        previous = {}
    if source_hash and previous.get("signature") == signature and all(
        (root / name).is_file() for name in previous.get("files", [])
    ):
        print("[hlmod] Python proxy signature matches; skipping generation.")
        return
    files = renderer.render()
    for name, source in sorted(files.items()):
        target = root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(source, encoding="utf-8")
    # Only remove files owned by the previous generator run, never user modules.
    for name in previous.get("files", []):
        candidate = root / name
        if name not in files and candidate.resolve().is_relative_to(root.resolve()):
            candidate.unlink(missing_ok=True)
    manifest_path.write_text(json.dumps({"signature": signature, "files": sorted(files)}, sort_keys=True), encoding="utf-8")
    (root / ".source_hash").write_text(source_hash, encoding="utf-8")
    print(f"[hlmod] Generated {len(files)} Python proxy modules/packages.")
