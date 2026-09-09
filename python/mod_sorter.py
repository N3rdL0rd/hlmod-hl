"""Discover literal mod manifests and resolve a deterministic dependency order."""

import ast
from collections import deque
from pathlib import Path


def get_mod_info(filepath):
    path = Path(filepath)
    try:
        source = path.read_text(encoding="utf-8")
    except FileNotFoundError:
        return None
    if "MOD_INFO" not in source:
        return None
    tree = ast.parse(source, filename=str(path))
    for node in tree.body:
        value = None
        if isinstance(node, ast.Assign) and len(node.targets) == 1:
            target = node.targets[0]
            if isinstance(target, ast.Name) and target.id == "MOD_INFO":
                value = node.value
        elif isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
            if node.target.id == "MOD_INFO":
                value = node.value
        if value is None:
            continue
        try:
            info = ast.literal_eval(value)
        except (ValueError, TypeError) as error:
            raise ValueError(f"{path}: MOD_INFO must be a literal dictionary") from error
        if not isinstance(info, dict):
            raise ValueError(f"{path}: MOD_INFO must be a dictionary")
        if not isinstance(info.get("enabled", True), bool):
            raise ValueError(f"{path}: MOD_INFO.enabled must be a boolean")
        mod_id = info.get("id")
        if not isinstance(mod_id, str) or not mod_id:
            raise ValueError(f"{path}: MOD_INFO.id must be a nonempty string")
        dependencies = info.get("dependencies", [])
        if not isinstance(dependencies, (list, tuple)) or any(
            not isinstance(dep, str) or not dep for dep in dependencies
        ):
            raise ValueError(f"{path}: MOD_INFO.dependencies must contain mod IDs")
        return {**info, "dependencies": list(dict.fromkeys(dependencies))}
    return None


def find_mods(mods_dir):
    found = []
    for path in sorted(Path(mods_dir).iterdir()):
        if path.name.startswith("_") or path.name == "stubs":
            continue
        if path.is_dir():
            info = get_mod_info(path / "__init__.py")
            name = path.name
        elif path.suffix == ".py":
            info = get_mod_info(path)
            name = path.stem
        else:
            continue
        if info is not None:
            found.append({"info": info, "name": name})
    return found


def resolve_mod_order(mods_dir):
    mods = {}
    for item in find_mods(mods_dir):
        info = item["info"]
        if not info.get("enabled", True):
            print(f"    -> Skipping disabled mod: {info['id']!r}")
            continue
        mod_id = info["id"]
        if mod_id in mods:
            raise ValueError(f"Duplicate mod ID {mod_id!r}: {mods[mod_id]['name']} and {item['name']}")
        mods[mod_id] = item
    dependents = {mod_id: [] for mod_id in mods}
    indegree = {mod_id: 0 for mod_id in mods}
    for mod_id, item in mods.items():
        for dependency in item["info"]["dependencies"]:
            if dependency not in mods:
                return {"status": "error", "message": f"Mod {mod_id!r} has an unmet dependency: {dependency!r}"}
            dependents[dependency].append(mod_id)
            indegree[mod_id] += 1
    ready = deque(mod_id for mod_id in mods if not indegree[mod_id])
    order = []
    while ready:
        mod_id = ready.popleft()
        item = mods[mod_id]
        order.append({"id": mod_id, "name": item["name"], "dependencies": item["info"]["dependencies"]})
        for dependent in dependents[mod_id]:
            indegree[dependent] -= 1
            if not indegree[dependent]:
                ready.append(dependent)
    if len(order) != len(mods):
        blocked = sorted(mod_id for mod_id, degree in indegree.items() if degree)
        return {"status": "error", "message": f"Dependency cycle blocks mods: {blocked}"}
    return {"status": "ok", "order": order}
