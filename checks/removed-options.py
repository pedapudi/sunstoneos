#!/usr/bin/env python3
"""Fail when a tracked file still tests a removed build option, which would compile its branch out with no error.

The one-OS-image series removed these options (docs/designs/one-os-image.md): every OS image links the code they selected,
and a session grant or the contract magic chooses it at run time. Nothing defines them any more, so a remaining test is
silently false: `#ifdef`, `#if defined(...)` or `defined(...)` in C, C++, assembly or a patch; any use in CMake (an
`if()`, a cache entry or a compile definition); any mention in a JSON manifest; and a configure argument (`-DNAME`) or
cache read (`NAME:BOOL`, `NAME=`) in a script. Documentation, history and evidence files that describe a removal are exempt
(docs/, *.md and *.jsonl), and ALLOWED lists the few uses that remain on purpose, each with its reason.

    removed-options.py [--extra-tree PATH ...]

--extra-tree runs the same rules on another Git work tree, such as a mimus work tree with its SunstoneOS port. Before
scanning, the check confirms that its own rules find a mutated C, CMake, manifest and script line.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
CHECK_INPUTS = ['apps', 'arch', 'boot', 'checks', 'cmake', 'drivers', 'include', 'kernel', 'lib', 'network', 'patches',
                'platform', 'runtime', 'shaders', 'tools', 'CMakeLists.txt']

# Each removed option and what replaced it.
REMOVED = {
    'SUNSTONE_A733_OS_CPU_RETURN': 'every OS image links the returning lifetime; the contract magic selects it '
                                   '(os_lifetime() == A733_LIFETIME_RETURN)',
    'SUNSTONE_A733_OS_RESIDENT': 'every OS image links the resident lifetime; the contract magic selects it '
                                 '(os_lifetime(), BOARD_RESIDENT())',
    'SUNSTONE_BOARD_RESIDENT': 'the resident lifetime comes from the contract magic (BOARD_RESIDENT(), os_lifetime())',
    'SUNSTONE_A733_OS_CPU_CLOCK': 'every OS image links the CPU clock owner; the session grants it '
                                  '(os_granted(OS_GRANT_CPU_CLOCK), manifest cpu.clock_grants)',
    'A733_A76_ADMITTED': 'every OS image links the A76 placement; the manifest cpu.cluster and the session grant select it',
    'SUNSTONE_A733_OS_PERF': 'every OS image links the performance-mode service; the manifest perf declaration grants it',
    'SUNSTONE_A733_OS_PERF_BOOT': 'the boot mode comes from the manifest perf declaration (tools/session.py --perf)',
    'SUNSTONE_A733_OS_AUDIO': 'every OS image links I2S capture; the session grants it (SS_DEVICE_AUDIO_I2S)',
    'SUNSTONE_A733_OS_RADIO': 'every OS image links the Wi-Fi station; the session grants it (SS_DEVICE_RADIO)',
    'SUNSTONE_A733_OS_HEADSET': 'an image links the headset library when its application declares audio-headset; '
                                'the session grants it',
    'SUNSTONE_A733_OS_MSBC': 'mSBC is always linked with the headset; the Bluetooth profile mode selects it',
    'SUNSTONE_A733_OS_NPU': 'every OS image links the NPU service; a manifest npu section grants it '
                            '(os_granted(OS_GRANT_NPU))',
    'SUNSTONE_A733_OS_NPU_QUEUE': 'every OS image links the NPU ring; npu.in_flight above one selects it',
    'SUNSTONE_A733_OS_NPU_OBSERVE': 'every OS image links the NPU observer; the manifest npu.observe flag selects it',
}
# Uses that stay on purpose: (tree-relative path, option) -> reason.
ALLOWED = {
    ('cmake/sunstone-os-core.cmake', 'SUNSTONE_A733_OS_CPU_RETURN'):
        'drops the entry from a build directory configured before the removal',
    ('tools/host_admission.py', 'SUNSTONE_A733_OS_CPU_RETURN'):
        'builds the admission harness of an image built before the removal from that image\'s own flags',
    ('apps/os/os-services.h', 'SUNSTONE_A733_OS_HEADSET'):
        'the image build defines the name for the headset library and the image tables that reference it',
    ('cmake/sunstone-image.cmake', 'SUNSTONE_A733_OS_HEADSET'): 'defines the name for an image that links the headset',
    ('cmake/sunstone-os-core.cmake', 'SUNSTONE_A733_OS_HEADSET'): 'defines the name for the headset library',
    ('checks/os-audio-replay.py', 'SUNSTONE_A733_OS_HEADSET'):
        'asserts that the image build does not define the name for the capture unit',
    ('checks/os-image-headset.py', 'SUNSTONE_A733_OS_HEADSET'):
        'asserts that the image build defines the name for the headset unit',
    ('checks/removed-options.py', None): 'this check',
}
NAMES = '|'.join(sorted(REMOVED, key=len, reverse=True))
C_SUFFIXES = {'.c', '.h', '.cc', '.cpp', '.cxx', '.hh', '.hpp', '.inc', '.S', '.s', '.ld', '.lds', '.patch', '.diff'}
SCRIPT_SUFFIXES = {'.py', '.sh', '.txt', '.toml', '.yaml', '.yml'}
DIRECTIVE = re.compile(r'^\s*#\s*(?:if|ifdef|ifndef|elif|elifdef|elifndef)\b.*?\b(%s)\b' % NAMES)
DEFINED = re.compile(r'\bdefined\s*\(?\s*(%s)\b' % NAMES)
# A name that starts a word or follows -D, as in add_definitions(-DNAME).
WORD = re.compile(r'(?:(?<=-D)|\b)(%s)\b' % NAMES)
SCRIPT = re.compile(r'-D(%s)\b|\b(%s)(?=\s*[:=])' % (NAMES, NAMES))


def exempt(relative):
    parts = Path(relative).parts
    return (parts[0] == 'docs' or relative.endswith('.md') or relative.endswith('.jsonl')
            or any('history' in part for part in parts))


def logical_lines(text):
    """Lines with backslash continuations joined, each with the number of its first physical line."""
    pending, start = '', None
    for number, line in enumerate(text.splitlines(), 1):
        if start is None:
            start = number
        if line.endswith('\\'):
            pending += line[:-1] + ' '
            continue
        yield start, pending + line
        pending, start = '', None
    if start is not None:
        yield start, pending


def kind(relative):
    name = Path(relative).name
    suffix = Path(relative).suffix
    if suffix in C_SUFFIXES:
        return 'c'
    if name == 'CMakeLists.txt' or suffix == '.cmake':
        return 'cmake'
    if suffix == '.json':
        return 'manifest'
    if suffix in SCRIPT_SUFFIXES:
        return 'script'
    return None


def hits(relative, text):
    """(line, option, what) for every use of a removed option in one file."""
    found = []
    form = kind(relative)
    if form is None:
        return found
    for number, line in logical_lines(text):
        if form == 'c':
            if Path(relative).suffix in ('.patch', '.diff') and line.startswith(('-', '---')):
                continue
            body = line[1:] if Path(relative).suffix in ('.patch', '.diff') and line[:1] in '+ ' else line
            names = [m.group(1) for m in DIRECTIVE.finditer(body)] + [m.group(1) for m in DEFINED.finditer(body)]
            found += [(number, n, 'preprocessor test') for n in dict.fromkeys(names)]
        elif form == 'cmake':
            code = line.split('#', 1)[0] if not line.lstrip().startswith('#[') else ''
            found += [(number, n, 'CMake use') for n in dict.fromkeys(m.group(1) for m in WORD.finditer(code))]
        elif form == 'manifest':
            found += [(number, n, 'manifest entry') for n in dict.fromkeys(m.group(1) for m in WORD.finditer(line))]
        else:
            names = [m.group(1) or m.group(2) for m in SCRIPT.finditer(line)]
            found += [(number, n, 'configure argument or cache read') for n in dict.fromkeys(names)]
    return found


def scan(tree):
    listed = subprocess.run(['git', '-C', str(tree), 'ls-files', '-z'], capture_output=True, check=True).stdout
    failures = []
    for relative in sorted(p.decode() for p in listed.split(b'\0') if p):
        if exempt(relative) or kind(relative) is None or (relative, None) in ALLOWED and tree == ROOT:
            continue
        path = tree / relative
        if not path.is_file():
            continue
        text = path.read_text(errors='replace')
        if not WORD.search(text):
            continue
        for number, option, what in hits(relative, text):
            if tree == ROOT and (relative, option) in ALLOWED:
                continue
            failures.append('%s:%d: %s of %s; it became: %s' % (tree / relative, number, what, option, REMOVED[option]))
    return failures


def self_test():
    """Each rule finds one mutated line, and the allowed and exempt forms stay quiet."""
    cases = [
        ('apps/os/example.c', 'int a;\n#ifdef SUNSTONE_A733_OS_NPU\nint b;\n#endif\n', 1),
        ('apps/os/example.h', '#if X || \\\n    defined(A733_A76_ADMITTED)\n#endif\n', 1),
        ('apps/os/example.patch', '-#ifdef SUNSTONE_A733_OS_RADIO\n+#ifdef SUNSTONE_A733_OS_AUDIO\n', 1),
        ('cmake/example.cmake', 'if(SUNSTONE_A733_OS_CPU_CLOCK)\nendif()\n# SUNSTONE_A733_OS_PERF in a comment\n', 1),
        ('CMakeLists.txt', 'add_definitions(-DSUNSTONE_A733_OS_RESIDENT)\n', 1),
        ('apps/x/manifest.json', '{"options": ["SUNSTONE_A733_OS_NPU_QUEUE"]}\n', 1),
        ('tools/example.py', "flags = ['-DSUNSTONE_A733_OS_PERF_BOOT=high']\n# SUNSTONE_A733_OS_MSBC was removed\n", 1),
        ('apps/os/example.c', '/* SUNSTONE_A733_OS_NPU was removed */\nint SUNSTONE_A733_OS_NPU_COUNT;\n', 0),
    ]
    for relative, text, expected in cases:
        got = len(hits(relative, text))
        assert got == expected, ('self-test', relative, text, got, expected)
    assert exempt('docs/designs/one-os-image.md') and exempt('README.md') and not exempt('apps/os/start.c')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--extra-tree', type=Path, action='append', default=[],
                        help='another Git work tree to scan with the same rules (repeatable)')
    args = parser.parse_args()
    self_test()
    failures = []
    for tree in [ROOT, *[t.resolve() for t in args.extra_tree]]:
        failures += scan(tree)
    for line in failures:
        print('FAIL ' + line)
    if failures:
        sys.exit('%d uses of removed build options' % len(failures))
    print('PASS: no tracked file tests a removed build option (%d options, %d trees)'
          % (len(REMOVED), 1 + len(args.extra_tree)))


if __name__ == '__main__':
    main()
