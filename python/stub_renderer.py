"""Render importable HashLink proxies from native metadata, without runtime access."""
from __future__ import annotations

import builtins
import hashlib
import json
import keyword
from pathlib import Path


OBJ, STRUCT, VIRTUAL, FUN, METHOD = 11, 21, 15, 10, 20


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
    "hlmod", "HlPtr", "HlObject", "HlVirtual", "HlArray", "hltype",
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
    def __init__(self, metadata: dict):
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

    def annotation(self, index: int, used: set[int], active: frozenset[int] = frozenset(), *, argument: bool = False, bound_method: bool = False) -> str:
        t = self.types.get(index)
        if t is None or index in active:
            return "Any"
        kind = t["kind"]
        simple = {0: "None", 1: "int", 2: "int", 3: "int", 4: "int", 5: "float", 6: "float",
                  7: "bool", 8: "HlPtr", 9: "Any", 12: "HlArray[Any]", 13: "HlPtr",
                  14: "HlPtr", 16: "HlPtr", 17: "HlPtr", 18: "HlPtr", 20: "Never",
                  21: "HlPtr", 22: "Never", 23: "Never"}
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
        prefix.append(f"    def {name}({parameters}) -> {result}:")
        doc = self.member_doc(t, method["name"], "functions")
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
                      "from hlobj import HlArray, HlObject, HlVirtual, hltype", *sorted(imports), ""]
            if used:
                header.append("if TYPE_CHECKING:")
                for dependency in sorted(used):
                    dep_module, dep_name = self.locations[dependency]
                    header.append(f"    from {dep_module} import {dep_name} as _T{dependency}")
                header.append("")
            files[module.removeprefix("stubs.").replace(".", "/") + ".py"] = "\n".join(header + body) + "\n"
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
        return files


def generate(metadata: dict, base_dir: str, source_hash: str) -> None:
    if "docs" not in metadata:
        metadata["docs"] = {}
        for path in (Path("haxe_docs.json"), Path("mods/haxe_docs.json"), Path("std_doc/haxe_docs.json")):
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
    root = Path(base_dir)
    signature = hashlib.sha256((source_hash + metadata["code_hash"] + json.dumps(
        metadata.get("docs", {}), sort_keys=True, ensure_ascii=True)).encode()).hexdigest()
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
    files = Renderer(metadata).render()
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
