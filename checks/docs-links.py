#!/usr/bin/env python3
"""Check local Markdown links in docs, including heading fragments. No network or device access."""
import html
from pathlib import Path
import re
import sys
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
DESTINATION = r'(<[^>\n]+>|(?:\\.|[^\s()\\]|\([^()\n]*\))+)'
INLINE = re.compile(r'!?\[(?:\\.|[^\]\\])*\]\(\s*' + DESTINATION + r'(?:\s+[^\n]*?)?\s*\)')
DEFINITION = re.compile(r'^ {0,3}\[([^\]\n]+)\]:\s*' + DESTINATION, re.M)
REFERENCE = re.compile(r'!?\[([^\]\n]+)\](?:\[([^\]\n]*)\])?')


def prose(text):
    """Blank comments and fenced code while preserving offsets for line numbers."""
    text = re.sub(r'<!--.*?-->', lambda m: re.sub(r'[^\n]', ' ', m[0]), text, flags=re.S)
    lines, fence = [], None
    for line in text.splitlines(keepends=True):
        marker = re.match(r' {0,3}(`{3,}|~{3,})', line)
        inside = fence is not None
        if marker:
            run = marker[1]
            if fence is None:
                fence = run
            elif run[0] == fence[0] and len(run) >= len(fence):
                fence = None
        lines.append(re.sub(r'[^\n]', ' ', line) if inside or marker else line)
    return ''.join(lines)


def anchors(text):
    text = prose(text)
    result = set(re.findall(r'\b(?:id|name)=["\']([^"\']+)["\']', text))
    headings = re.findall(r'^ {0,3}#{1,6}\s+(.+?)\s*#*\s*$', text, re.M)
    headings += re.findall(r'^([^\n]+)\n {0,3}(?:=+|-+)\s*$', text, re.M)
    used = set()
    for heading in headings:
        heading = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', heading)
        heading = html.unescape(re.sub(r'<[^>]*>', '', heading)).lower()
        slug = re.sub(r'[^\w\- ]', '', heading).replace(' ', '-')
        candidate, index = slug, 0
        while candidate in used:
            index += 1
            candidate = f'{slug}-{index}'
        used.add(candidate)
    return result | used


def links(text):
    text = prose(text)
    # Inline code is not a link; code inside a real link label is harmless.
    text = re.sub(r'(`+)([^`]*?)\1', lambda m: ' ' * len(m[0]), text)
    definitions = {m[1].casefold(): m[2] for m in DEFINITION.finditer(text)}
    occupied = []
    for pattern in (INLINE, DEFINITION):
        for match in pattern.finditer(text):
            occupied.append(match.span())
            yield text.count('\n', 0, match.start()) + 1, match[1 if pattern is INLINE else 2].strip('<>')
    for match in REFERENCE.finditer(text):
        if any(start <= match.start() < end for start, end in occupied):
            continue
        key = (match[2] or match[1]).casefold()
        if key in definitions:
            yield text.count('\n', 0, match.start()) + 1, definitions[key].strip('<>')
    for match in re.finditer(r'\b(?:href|src)=["\']([^"\']+)["\']', text):
        yield text.count('\n', 0, match.start()) + 1, html.unescape(match[1])


def main():
    documents = sorted((ROOT / 'docs').rglob('*.md'))
    heading_cache, broken, checked = {}, [], 0
    for document in documents:
        for line, destination in links(document.read_text()):
            destination = re.sub(r'\\(.)', r'\1', destination)
            url = urlsplit(destination)
            if url.scheme or url.netloc or url.path.startswith('/'):
                continue
            checked += 1
            target = document.parent / unquote(url.path) if url.path else document
            reason = None
            if not target.exists():
                reason = 'missing path'
            elif url.fragment and target.suffix.lower() == '.md':
                target = target.resolve()
                if target not in heading_cache:
                    heading_cache[target] = anchors(target.read_text())
                if unquote(url.fragment) not in heading_cache[target]:
                    reason = 'missing heading or HTML anchor'
            if reason:
                broken.append(f'{document.relative_to(ROOT)}:{line}: {destination}: {reason}')
    for failure in broken:
        print(failure)
    print(f'{len(documents)} documents, {checked} relative links, {len(broken)} broken links')
    return bool(broken)


if __name__ == '__main__':
    sys.exit(main())
