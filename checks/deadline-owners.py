#!/usr/bin/env python3
"""Prove from the linked code of the OS images that the deadline-owner bound is exact (apps/os/os-services.h).

Every task that calls sunstone_deadline_register takes one slot of a table that the image's own tables size by
OS_DEADLINE_OWNERS (runtime/deadline.c, tools/sunstone-manifest.py). The bound is a sum of terms: the owners column of each
linked start entry (OS_START_ROWS), less the DMA0 pool's two tasks when both the audio and the dma entries count them,
plus the board's registering tasks, the application's, the SD0 owner's and the background load's. This check derives each
term from the linked image and refuses a column or term that differs from the code.

From the disassembly and symbol table of each image the check builds:
- the direct call graph: bl and b to the first instruction of a function;
- every task creation: a call of xTaskCreateStatic, xTaskCreateStaticAffinitySet or one of the OS and kit wrappers
  (CREATE), with its entry function taken from the argument register (an adrp and add pair, adr or mov). A creation
  whose entry is loaded from an unnamed constant array of function pointers (the board's task list in apps/board/main.c)
  creates one task for each function in the array that no other creation in the same function names;
- the tasks that register: a creation whose entry function reaches sunstone_deadline_register through direct calls,
  without passing another task creation.

The tasks of a term are the creations reachable from its root functions, and the creations reachable from the entry
functions of those tasks: each start entry's start function (os_applications, the image's start list), the
application's preparation (os_application_prepare, which creates the main task and the GPU peer) and the board's start
(a733_smp_diagnostic_start). Each creation is taken to create at most one task; every creation that can register in
these images uses static task storage that only one call can hold.

A registration can also be reached through a function pointer, such as a retirement callback that a service worker
calls. The check finds each function that calls sunstone_deadline_register and that no registering task reaches by
direct calls, and requires it in CALLBACKS with the task it runs in. A registering creation that no term reaches, or a
creation whose entry the check cannot resolve, fails the check.

The check requires, in each image: each term at or below its value, the registering tasks at or below the image's slot
count (sunstone_deadline_slot_count), and that slot count equal to the sum of the terms the image links. Over all the
images, each column and term must be reached by some image. The images (GATE_RUNS) are the gate's OS variants and
recipes that together link every start entry: no image links all of them, since the headset and the video decoder are
linked per application. The bound of an image that links every service is the sum of the same proven terms.

Two mutants are run before the result: the RTC start entry rebuilt with a second registering task and relinked (from
the compile and link commands of the sup build), which the column comparison must refuse; and the RTC column raised by
one in a copy of apps/os/os-services.h, which the comparison must also refuse.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import re
import shlex
import struct
import subprocess

import harness
from elftools.elf.elffile import ELFFile

ROOT = Path(__file__).resolve().parents[1]
GATE_RUNS = [['--elf', '{elf:sup}', '--elf', '{elf:headset}', '--elf', '{elf:ret-gpu}', '--elf', '{elf:ret-vdec}',
              '--elf', '{elf:ret-g2d}', '--elf', '{elf:os-deadline-owners}', '--mutant-build', '{build:sup}',
              '--output', '{output}']]
# The files that create a task or register a deadline owner in the OS images, and the declaration and sizing of the
# bound: a change to one selects this check. A new file that creates a registering task must be added here.
CHECK_INPUTS = ['apps/os/os-services.h', 'apps/os/grants.c', 'runtime/deadline.c', 'include/sunstone_deadline.h',
                'include/sunstone_deadline_slot.h', 'tools/sunstone-manifest.py', 'tools/gate_images.py',
                'apps/board/host_irq.c', 'apps/board/main.c', 'apps/board/radio.c', 'apps/os/main.patch',
                'apps/os/application.c', 'apps/os/audio.c', 'apps/os/capture.c', 'apps/os/client.c', 'apps/os/cpu-return.c',
                'apps/os/dma.c', 'apps/os/g2d.c', 'apps/os/gpu.c', 'apps/os/headset.c', 'apps/os/i2c.c', 'apps/os/large.c',
                'apps/os/large_load.c', 'apps/os/large_sd.c', 'apps/os/npu-client.c', 'apps/os/npu.c', 'apps/os/radio.c',
                'apps/os/rtc.c', 'apps/os/sd_owner.c', 'apps/os/services.c', 'apps/os/supervisor-wait.c', 'apps/os/thermal.c',
                'apps/os/tlbi-issuer.c', 'apps/os/video_decode.c', 'drivers/dma/dma_service.c', 'drivers/gpu/gpu_queue.c',
                'drivers/gpu/pvr_gpu_service.c', 'drivers/net/a733_radio_events.c', 'drivers/net/sunstone_net.c',
                'drivers/net/a733_radio_session.c', 'drivers/npu/npu_runtime_queue.c', 'drivers/npu/npu_service.c',
                'drivers/npu/a733_npu_runtime.c', 'platform/a733/dma_pool_session.c', 'runtime/bg_load.c']
OBJDUMP = '/usr/lib/llvm-21/bin/llvm-objdump'
# The task creations: the function and the argument register that holds the new task's entry function.
CREATE = {'xTaskCreateStatic': 0, 'xTaskCreateStaticAffinitySet': 0, 'os_task_create_static': 1,
          'os_task_create_static_affinity_set': 1, 'ss_task_create_static_affinity_set': 0}
REGISTER = 'sunstone_deadline_register'
# The roots of the terms outside the start list.
APPLICATION_ROOT = 'os_application_prepare'
BOARD_ROOT = 'a733_smp_diagnostic_start'
# Creations that belong to one term wherever they are reached: the SD0 owner, which the large-region loader or the
# application's own storage call starts (apps/os/sd_owner.c start).
SHARED_ENTRIES = {'sd_owner.c:owner_main': 'sd'}
# Creations whose task can register but that the OS never creates, with the reason.
EXCLUDED = {
    'sunstone_net.c:worker': 'the network service creates its own worker only in sunstone_net_init; the OS binds the '
                             'service to board-net through sunstone_net_init_owned (drivers/net/a733_radio_session.c), '
                             'which creates no task',
}
# Functions that register and that no registering task reaches by direct calls, with the task each runs in.
CALLBACKS = {
    'audio.c:mask': 'the I2S stream\'s retirement callback (dma_service_bind_retirement), called by the dma-service worker, '
                    'which registers at its entry (drivers/dma/dma_service.c)',
    'dma.c:mask': 'the RAM-copy service\'s retirement callback (dma_service_bind_retirement), called by the dma-service '
                  'worker, which registers at its entry',
    'npu_runtime_queue.c:power_owner': 'the entry of the NPU queue\'s power worker, which no OS code creates '
                                       '(npu_runtime_queue_power_start has no caller)',
}


class Image:
    """The symbols, direct call graph and task creations of one linked image."""

    def __init__(self, path):
        self.path = Path(path)
        self.elf = ELFFile(open(path, 'rb'))
        self.funcs, objects, self.byname = {}, {}, collections.defaultdict(list)
        current = None
        for s in self.elf.get_section_by_name('.symtab').iter_symbols():
            kind = s['st_info']['type']
            if kind == 'STT_FILE':
                current = s.name
                continue
            if s['st_shndx'] in ('SHN_UNDEF', 'SHN_ABS'):
                continue
            if kind == 'STT_FUNC' and s['st_value'] not in self.funcs:
                local = s['st_info']['bind'] == 'STB_LOCAL'
                self.funcs[s['st_value']] = (s.name, (current + ':' + s.name) if local and current else s.name)
                self.byname[s.name].append(s['st_value'])
            elif kind == 'STT_OBJECT' and s['st_size']:
                objects[s['st_value']] = s['st_size']
        self.symbols = {s.name: s for s in self.elf.get_section_by_name('.symtab').iter_symbols()
                        if s['st_info']['type'] in ('STT_OBJECT', 'STT_FUNC')}
        okeys = sorted(objects)

        def named(address):
            i = bisect.bisect_right(okeys, address) - 1
            return i >= 0 and address < okeys[i] + objects[okeys[i]]
        self.calls = collections.defaultdict(set)
        self.sites = []                  # (address, creating function, entry or None, [array candidates])
        create = {a: CREATE[n] for n in CREATE for a in self.byname.get(n, ())}
        anonymous = collections.defaultdict(set)
        text = subprocess.run([OBJDUMP, '-d', '--no-show-raw-insn', str(path)], capture_output=True, text=True,
                              check=True).stdout
        function_line = re.compile(r'^([0-9a-f]+) <.+>:$')
        instruction = re.compile(r'^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$')
        current, regs = None, {}
        for line in text.splitlines():
            m = function_line.match(line)
            if m:
                start = int(m.group(1), 16)
                current, regs = (start if start in self.funcs else None), {}
                continue
            m = instruction.match(line)
            if not m or current is None:
                continue
            here, op, operands = int(m.group(1), 16), m.group(2), m.group(3)
            args = [a.strip() for a in re.split(r',\s*', operands.split('<')[0].split('//')[0])] if operands else []
            if op in ('bl', 'b') and args and re.fullmatch(r'0x[0-9a-f]+|[0-9a-f]+', args[0]):
                target = int(args[0], 16)
                if target in self.funcs and target != current:
                    self.calls[current].add(target)
                    if op == 'bl' and target in create and current not in create:
                        value = regs.get('x%d' % create[target])
                        entry = value if value in self.funcs else None
                        self.sites.append((here, current, entry))
                if op == 'bl':      # a call keeps the callee-saved registers x19 to x28 only
                    regs = {r: v for r, v in regs.items() if r[1:].isdigit() and 19 <= int(r[1:]) <= 28}
                continue
            if not args or not re.fullmatch(r'[xw]\d+|sp', args[0]):
                continue
            destination = 'x' + args[0][1:]
            value = None
            if op == 'adrp' and len(args) >= 2:
                regs[destination] = ('page', int(args[1], 16))
                continue
            if op == 'adr' and len(args) >= 2:
                value = int(args[1], 16)
            elif (op == 'add' and len(args) >= 3 and args[2].startswith('#') and isinstance(regs.get('x' + args[1][1:]), tuple)
                  and regs['x' + args[1][1:]][0] == 'page'):
                value = regs['x' + args[1][1:]][1] + int(args[2][1:], 0)
            elif op == 'mov' and len(args) == 2 and args[1].startswith('x') and args[1] in regs:
                regs[destination] = regs[args[1]]
                continue
            else:
                regs.pop(destination, None)
                continue
            regs[destination] = value
            if value not in self.funcs and not named(value):
                anonymous[current].add(value)
        # A creation whose entry is loaded: the functions of the unnamed constant arrays its function addresses.
        named_entries = collections.defaultdict(set)
        for _, function, entry in self.sites:
            if entry:
                named_entries[function].add(entry)
        resolved = []
        for here, function, entry in self.sites:
            candidates = []
            if entry is None:
                for address in sorted(anonymous[function]):
                    run = self.pointer_run(address)
                    if len(run) >= 2:
                        candidates += [f for f in run if f not in named_entries[function] and f not in candidates]
            resolved.append((here, function, entry, candidates))
        self.sites = resolved
        self.stop = set(create)
        self.register = set(self.byname.get(REGISTER, ()))
        self.reached = {}

    def read(self, address, size):
        for section in self.elf.iter_sections():
            if section['sh_type'] == 'SHT_PROGBITS' and section['sh_addr'] <= address < section['sh_addr'] + section['sh_size']:
                return section.data()[address - section['sh_addr']:address - section['sh_addr'] + size]
        return b''

    def pointer_run(self, address):
        """The function addresses stored one after another from address, in a read-only section."""
        for section in self.elf.iter_sections():
            if (section['sh_type'] == 'SHT_PROGBITS' and section['sh_flags'] & 2 and not section['sh_flags'] & 5 and
                    section['sh_addr'] <= address < section['sh_addr'] + section['sh_size']):
                data, offset, run = section.data(), address - section['sh_addr'], []
                while offset + 8 <= len(data):
                    value = struct.unpack_from('<Q', data, offset)[0]
                    if value not in self.funcs:
                        break
                    run.append(value)
                    offset += 8
                return run
        return []

    def name(self, address):
        return self.funcs[address][1]

    def reach(self, start, boundary=frozenset()):
        """The functions that start reaches by direct calls without passing a task creation or a boundary function."""
        key = (start, boundary)
        if key not in self.reached:
            seen, todo = set(), [start]
            while todo:
                n = todo.pop()
                if n in seen or n in self.stop or (n in boundary and n != start):
                    continue
                seen.add(n)
                todo += self.calls.get(n, ())
            self.reached[key] = seen
        return self.reached[key]

    def registers(self, entry):
        return bool(self.reach(entry) & self.register)

    def word(self, symbol, size=4):
        s = self.symbols[symbol]
        return int.from_bytes(self.read(s['st_value'], size), 'little')

    def start_list(self):
        """(name, start function) of each entry of the image's start list (os_applications)."""
        table, rows = self.symbols['os_applications'], []
        raw = self.read(table['st_value'], table['st_size'])
        for i in range(table['st_size'] // 32):
            name, _privileged, _pins, start = struct.unpack_from('<4Q', raw, 32 * i)
            text = self.read(name, 64)
            rows.append((text[:text.index(0)].decode(), start))
        return rows

    def tasks(self, roots, boundary=frozenset()):
        """The registering tasks created from roots and from the tasks those create: {(creation, entry)}. The search
        does not enter a boundary function: the root of another term."""
        by_function = collections.defaultdict(list)
        for site in self.sites:
            by_function[site[1]].append(site)
        found, todo, seen = set(), list(roots), set()
        unresolved = []
        while todo:
            root = todo.pop()
            if root in seen:
                continue
            seen.add(root)
            for function in self.reach(root, boundary):
                for here, _, entry, candidates in by_function.get(function, ()):
                    entries = [entry] if entry else candidates
                    if not entries:
                        unresolved.append(here)
                    for e in entries:
                        todo.append(e)
                        if self.registers(e):
                            found.add((here, e))
        return found, unresolved


def columns(text):
    """{row name: owners} of OS_START_ROWS, and the literal terms, from the text of apps/os/os-services.h."""
    body = re.search(r'#define OS_START_ROWS\(X\)((?:[^\n]*\\\n)*[^\n]*)', text).group(1)
    rows = {}
    for row in re.findall(r'\bX\(([^()]*)\)', body):
        name, _prefix, _start, _with, owners = [a.strip() for a in row.split(',')]
        rows[name.strip('"')] = int(owners.rstrip('u'))
    literal = lambda name: int(re.search(r'^#define %s (\d+)u\s*$' % name, text, re.M).group(1))
    return rows, {'board': literal('OS_BOARD_DEADLINE_OWNERS'), 'pool': literal('OS_DMA_POOL_OWNERS')}


def analyse(image):
    """What one image links and registers, per term."""
    rows = image.start_list()
    starts = {name: start for name, start in rows if image.name(start) != 'os_application_start'}
    application_roots = [image.byname[APPLICATION_ROOT][0]] + [start for _, start in rows if image.name(start) == 'os_application_start']
    terms, unresolved = {}, []
    boundary = frozenset(list(starts.values()) + application_roots)
    for name, start in list(starts.items()) + [('application', None), ('board', None)]:
        roots = [start] if start else application_roots if name == 'application' else image.byname[BOARD_ROOT][:1]
        found, missing = image.tasks(roots, boundary)
        terms[name] = found
        unresolved += missing
    everything = set().union(*terms.values())
    shared, excluded = collections.defaultdict(set), set()
    for task in everything:
        key = image.name(task[1])
        if key in SHARED_ENTRIES:
            shared[SHARED_ENTRIES[key]].add(task)
        if key in EXCLUDED:
            excluded.add(task)
    for name in terms:
        terms[name] -= excluded | set().union(*shared.values())
    pool = terms.get('audio', set()) & terms.get('dma', set())
    # Every registering creation of the image must lie in a term, unless excluded.
    unattributed = []
    for here, function, entry, candidates in image.sites:
        for e in ([entry] if entry else candidates):
            if image.registers(e) and (here, e) not in everything:
                unattributed.append('%s creates %s at %#x' % (image.name(function), image.name(e), here))
    callers = {f for f, targets in image.calls.items() if targets & image.register}
    covered = set()
    for _, e in everything:
        covered |= image.reach(e)
    return {'starts': starts, 'terms': terms, 'shared': shared, 'pool': pool, 'excluded': excluded,
            'unresolved': unresolved, 'unattributed': unattributed,
            'uncovered': sorted(image.name(f) for f in callers - covered),
            'tasks': len(everything - excluded), 'slots': image.word('sunstone_deadline_slot_count')}


def compare(results, text):
    """The refusals of the columns and terms in text against the analysed images; an empty list proves them."""
    rows, literal = columns(text)
    refusals, best = [], collections.defaultdict(int)
    for path, r in results.items():
        image = Path(path).parent.name + '/' + Path(path).name
        for name in r['starts']:
            if name not in rows:
                refusals.append('%s: start entry "%s" has no row in OS_START_ROWS' % (image, name))
                continue
        gpu = 'gpu-queue' in r['starts']
        expected = {name: rows.get(name, 0) for name in r['starts']}
        expected.update(board=literal['board'], application=1 + gpu)
        for name, found in r['terms'].items():
            best[name] = max(best[name], len(found))
            if len(found) > expected[name]:
                refusals.append('%s: %s creates %d registering tasks (%s), its column is %d' % (
                    image, name, len(found), ', '.join(sorted(r['names'][t] for t in found)), expected[name]))
        if gpu and len(r['terms']['application']) != 1 + gpu:
            refusals.append('%s: the application creates %d registering tasks, %d expected' % (
                image, len(r['terms']['application']), 1 + gpu))
        sd = len(r['shared'].get('sd', ()))
        best['sd'] = max(best['sd'], sd)
        if sd > 1:
            refusals.append('%s: %d SD0 owner tasks' % (image, sd))
        both = 'audio' in r['starts'] and 'dma' in r['starts']
        if both:
            best['pool'] = max(best['pool'], len(r['pool']))
            if len(r['pool']) != literal['pool']:
                refusals.append('%s: the audio and dma entries share %d registering tasks, OS_DMA_POOL_OWNERS is %d' % (
                    image, len(r['pool']), literal['pool']))
        elif r['pool']:
            refusals.append('%s: tasks shared between start entries outside the DMA0 pool' % image)
        bound = (sum(expected[name] for name in r['starts']) - (literal['pool'] if both else 0) + literal['board'] +
                 1 + gpu + (1 if sd else 0))
        if bound != r['slots']:
            refusals.append('%s: the image has %d deadline slots; its terms sum to %d' % (image, r['slots'], bound))
        if r['tasks'] > r['slots']:
            refusals.append('%s: %d registering tasks for %d slots' % (image, r['tasks'], r['slots']))
        for note in r['unresolved']:
            refusals.append('%s: a task creation at %#x whose entry function the check cannot resolve' % (image, note))
        for note in r['unattributed']:
            refusals.append('%s: a registering task outside every term: %s' % (image, note))
        for name in r['uncovered']:
            if name not in CALLBACKS:
                refusals.append('%s: %s registers but no registering task reaches it by direct calls' % (image, name))
    for name, owners in rows.items():
        if not any(name in r['starts'] for r in results.values()):
            refusals.append('start entry "%s" is linked in no analysed image' % name)
        elif best[name] != owners:
            refusals.append('%s: its column is %d; at most %d registering tasks in the analysed images' % (name, owners, best[name]))
    for name, value in (('board', literal['board']), ('application', 2), ('sd', 1), ('pool', literal['pool'])):
        if best[name] != value:
            refusals.append('%s: %d at most in the analysed images, %d declared' % (name, best[name], value))
    seen_uncovered = set().union(*(set(r['uncovered']) for r in results.values()))
    for name in CALLBACKS:
        if name not in seen_uncovered:
            refusals.append('CALLBACKS names %s, which no analysed image holds as an uncovered registration' % name)
    seen_excluded = {key for r in results.values() for key in r['excluded_names']}
    for key in EXCLUDED:
        if key not in seen_excluded:
            refusals.append('EXCLUDED names %s, which no analysed image creates' % key)
    return refusals


def summarize(image, r):
    names = {}
    for found in list(r['terms'].values()) + list(r['shared'].values()) + [r['excluded']]:
        for here, e in found:
            names[(here, e)] = image.name(e)
    r['names'] = names
    r['excluded_names'] = sorted({image.name(e) for _, e in r['excluded']})
    return r


def mutant_image(build, elf, output):
    """The image of build relinked with apps/os/rtc.c creating a second registering task in its start entry."""
    source = ROOT / 'apps/os/rtc.c'
    text = source.read_text()
    creation = 'owner=xTaskCreateStaticAffinitySet(os_rtc_task,"os-rtc",'
    assert text.count(creation) == 1, 'apps/os/rtc.c no longer creates its owner task as the mutant expects'
    mutated = text.replace('int os_rtc_start(void)\n{', 'static StackType_t second_stack[1024];static StaticTask_t second_tcb;\n'
                           'int os_rtc_start(void)\n{', 1)
    mutated = mutated.replace(creation, '(void)xTaskCreateStaticAffinitySet(os_rtc_task,"os-rtc-2",1024,NULL,4,second_stack,'
                              '&second_tcb,1);\n    ' + creation, 1)
    copy = output / 'mutant-rtc.c'
    copy.write_text(mutated)
    entry = [e for e in json.loads((build / 'compile_commands.json').read_text()) if Path(e['file']) == source]
    assert len(entry) == 1, 'the build compiles apps/os/rtc.c %d times' % len(entry)
    command = shlex.split(entry[0]['command'])
    obj = command[command.index('-o') + 1]
    command[command.index('-o') + 1] = str(output / 'mutant-rtc.o')
    command[command.index('-c') + 1] = str(copy)
    command[1:1] = ['-iquote', str(source.parent)]
    done = subprocess.run(command, cwd=entry[0]['directory'], capture_output=True, text=True)
    assert done.returncode == 0, 'the mutant apps/os/rtc.c does not compile:\n' + done.stderr
    links = [p for p in (build / 'CMakeFiles').glob('*.dir/link.txt') if (' -o ' + elf.name + ' ') in p.read_text() or
             p.read_text().rstrip().endswith(' -o ' + elf.name)]
    assert len(links) == 1, 'no single link command writes %s' % elf.name
    link = shlex.split(next(l for l in links[0].read_text().splitlines() if ' -o ' in l))
    relative = [i for i, a in enumerate(link) if a.endswith(obj) or obj.endswith(a)]
    assert len(relative) == 1, 'the link command does not name the RTC object once'
    link[relative[0]] = str(output / 'mutant-rtc.o')
    link[link.index('-o') + 1] = str(output / 'mutant.elf')
    link = [a for a in link if not a.startswith('-Wl,-Map,')]
    done = subprocess.run(link, cwd=build, capture_output=True, text=True)
    assert done.returncode == 0, 'the mutant image does not link:\n' + done.stderr[-2000:]
    return output / 'mutant.elf'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--elf', type=Path, action='append', required=True)
    parser.add_argument('--mutant-build', type=Path, required=True, help='the build whose ELF is the first --elf')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    text = (ROOT / 'apps/os/os-services.h').read_text()
    results = {}
    for path in args.elf:
        image = Image(path)
        results[str(path)] = summarize(image, analyse(image))
    refusals = compare(results, text)
    # Mutant 1: a second registering task in the RTC entry, column unchanged.
    mutant = mutant_image(args.mutant_build, args.elf[0], args.output)
    image = Image(mutant)
    mutated_results = dict(results)
    mutated_results[str(args.elf[0])] = summarize(image, analyse(image))
    caught = [r for r in compare(mutated_results, text) if 'rtc creates 2 registering tasks' in r]
    assert caught, 'the mutant with a second registering RTC task passed the comparison'
    # Mutant 2: the RTC column raised by one, no new task.
    raised = re.sub(r'(X\("rtc", os_rtc, os_rtc_start, OS_ALWAYS, )1\)', r'\g<1>2)', text)
    assert raised != text, 'apps/os/os-services.h no longer has the RTC row the mutant raises'
    caught_raised = [r for r in compare(results, raised) if r.startswith('rtc: its column is 2')]
    assert caught_raised, 'the raised RTC column passed the comparison'
    report = {'images': {p: {'slots': r['slots'], 'tasks': r['tasks'], 'starts': sorted(r['starts']),
                             'terms': {n: sorted(r['names'][t] for t in found) for n, found in r['terms'].items()},
                             'sd': sorted(r['names'][t] for t in r['shared'].get('sd', ())),
                             'shared_pool': sorted(r['names'][t] for t in r['pool']),
                             'uncovered_registrations': r['uncovered']} for p, r in results.items()},
              'mutants': {'second-rtc-task': caught, 'raised-rtc-column': caught_raised},
              'refusals': refusals, 'passed': not refusals, 'physical_access': False}
    (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    assert not refusals, 'the deadline-owner bound differs from the linked code:\n  ' + '\n  '.join(refusals)
    for path, r in results.items():
        print('  %-60s %2d registering tasks, %2d slots' % (Path(path).parent.name + '/' + Path(path).name, r['tasks'], r['slots']))
    print('PASS deadline-owners: %d images; both mutants refused' % len(results))


if __name__ == '__main__':
    main()
