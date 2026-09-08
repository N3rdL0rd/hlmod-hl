import os
import ast
from collections import deque

def get_mod_info(filepath):
    '''Safely parses a Python file to get its MOD_INFO dict without executing it.'''
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            tree = ast.parse(f.read(), filename=filepath)
        for node in tree.body:
            if isinstance(node, ast.Assign):
                if len(node.targets) == 1 and isinstance(node.targets[0], ast.Name) and node.targets[0].id == 'MOD_INFO':
                    return ast.literal_eval(node.value)
    except (FileNotFoundError, SyntaxError):
        return None
    return None

def find_mods(mods_dir):
    '''Finds all valid mods in the mods directory, including single files and directories.'''
    found_mods = []
    for name in os.listdir(mods_dir):
        path = os.path.join(mods_dir, name)
        if name == 'stubs': continue
        if os.path.isdir(path):
            init_path = os.path.join(path, '__init__.py')
            info = get_mod_info(init_path)
            if info: found_mods.append({'info': info, 'name': name, 'is_dir': True})
        elif name.endswith('.py') and not name.startswith('__'):
            info = get_mod_info(path)
            if info: found_mods.append({'info': info, 'name': name.rsplit('.', 1)[0], 'is_dir': False})
    return found_mods

def resolve_mod_order(mods_dir):
    '''Discovers mods, builds a dependency graph, and performs a topological sort.'''
    discovered_mods = find_mods(mods_dir)
    mods = {}
    for mod_data in discovered_mods:
        info = mod_data['info']
        if info.get('enabled', True) is False:
            print(f"    -> Skipping disabled mod: '{info['id']}'")
            continue
        mods[info['id']] = {'info': info, 'name': mod_data['name'], 'dependencies': set(info['dependencies'])}
    adj = {mod_id: [] for mod_id in mods}
    in_degree = {mod_id: 0 for mod_id in mods}
    for mod_id, data in mods.items():
        for dep_id in data['dependencies']:
            if dep_id not in mods:
                return {'status': 'error', 'message': f'Mod \'{mod_id}\' has an unmet dependency: \'{dep_id}\' '}
            adj[dep_id].append(mod_id)
            in_degree[mod_id] += 1
    queue = deque([mod_id for mod_id in mods if in_degree[mod_id] == 0])
    sorted_order = []
    while queue:
        mod_id = queue.popleft()
        sorted_order.append({'id': mod_id, 'name': mods[mod_id]['name']})
        for neighbor in adj[mod_id]:
            in_degree[neighbor] -= 1
            if in_degree[neighbor] == 0:
                queue.append(neighbor)
    if len(sorted_order) == len(mods):
        return {'status': 'ok', 'order': sorted_order}
    else:
        cycle_nodes = set(mods.keys()) - {item['id'] for item in sorted_order}
        return {'status': 'error', 'message': f'Circular dependency detected among mods: {list(cycle_nodes)}'}
