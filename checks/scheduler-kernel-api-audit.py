#!/usr/bin/env python3
"""Check private API routes at function definitions, including disabled APIs."""
import re

SIGNATURE = re.compile(
    r'^\s*(?:(?:STATIC|static)\s+)?'
    r'(?:void|char\s*\*|BaseType_t|UBaseType_t|TickType_t|eTaskState|'
    r'uint32_t|TaskHandle_t|TaskHookFunction_t|configSTACK_DEPTH_TYPE|configRUN_TIME_COUNTER_TYPE|'
    r'StackType_t\s*\*|void\s*\*)\s*'
    r'([A-Za-z_]\w*)\s*\([^;{}]*\)\s*\{', re.M)


def definitions(source):
    masked = re.sub(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"',
                    lambda match: ' ' * len(match[0]), source, flags=re.S)
    result = {}
    for match in SIGNATURE.finditer(masked):
        opening = match.end() - 1
        depth, end = 1, opening + 1
        while depth:
            depth += (masked[end] == '{') - (masked[end] == '}')
            end += 1
        result.setdefault(match[1], []).append(source[opening + 1:end - 1])
    return result


def audit(source, inventory):
    bodies = definitions(source)
    for entry in inventory['apis']:
        if entry['state'] != 'routed-private':
            continue
        body = bodies[entry['name']][-1]  # SMP definition if UP also exists.
        branch = re.match(r'\s*#if \( configSS_PARTITIONED_SCHEDULER == 1 \)\n'
                          r'(.*?)\n#else\n', body, re.S)
        assert branch, f"{entry['name']}: missing definition route"
        assert branch[1].splitlines()[0] == entry['definition_route'], (
            f"{entry['name']}: definition route changed")
        helpers = set(re.findall(r'\b(ssKernel\w+)\s*\(', branch[1]))
        assert helpers == set(entry.get('definition_helpers', helpers)), (
            f"{entry['name']}: intended adapter helpers changed")
        assert body.count('#if ( configSS_PARTITIONED_SCHEDULER == 1 )') == 1, (
            f"{entry['name']}: nested call-site route")
    # A wrapper at a call site must never satisfy the definition check.
    negative = '''void vTaskSuspendAll(void) { legacy(); }
void caller(void) { if (vTaskSuspendAll()) {
#if ( configSS_PARTITIONED_SCHEDULER == 1 )
ssKernelSuspendAll();
#else
legacy();
#endif
} }
'''
    assert not re.match(r'\s*#if', definitions(negative)['vTaskSuspendAll'][0])


def audit_reachability(ast, selected):
    functions = {node['name']: node for node in ast['inner']
                 if node['kind'] == 'FunctionDecl'
                 and any(child['kind'] == 'CompoundStmt' for child in node.get('inner', []))}
    forbidden = {'pxReadyTasksLists', 'pxDelayedTaskList', 'pxOverflowDelayedTaskList',
                 'xSuspendedTaskList', 'xTasksWaitingTermination', 'xPendingReadyList'}

    def references(node):
        result = set()
        if node.get('kind') == 'DeclRefExpr':
            declaration = node['referencedDecl']
            result.add((declaration['kind'], declaration['name']))
        for child in node.get('inner', []):
            result.update(references(child))
        return result

    seen, todo = set(), list(selected)
    while todo:
        name = todo.pop()
        if name in seen or name not in functions:
            continue
        seen.add(name)
        for kind, reference in references(functions[name]):
            if kind == 'FunctionDecl':
                todo.append(reference)
            assert not (kind == 'VarDecl' and reference in forbidden), (
                f'{name}: selected API reaches legacy scheduler list {reference}')
    return len(seen)
