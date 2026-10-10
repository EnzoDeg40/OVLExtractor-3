#!/usr/bin/env python3
"""Build the data behind the static extraction map (site/index.html).

Every resource of every OVL pair in a game install is classified by how far
the extractor takes it, and the verdict is derived rather than typed in:

    the install          `ovlextract --list-loaders` on each pair gives its
                         resources (linked files `Name:tag`) and loader names
    docs/*.md            the loader registry (RCT3_OVL_FORMAT.md §2) states a
                         status per tag; section headings give the links
    extractors/src/*.cpp a tag quoted in an extractor is one it dispatches on
    annotations.json     what neither can express, or what the registry has
                         not caught up with

Statuses, from best to worst:

    extracted   an extractor writes it in a standard format, decode complete
    partial     an extractor handles it but the decode is incomplete, or it
                waits on another block
    decoded     the structure is documented, nothing is written
    unknown     no document and no extractor mentions the tag

Usage, from the repository root:

    python3 site/build.py "$ASSETS"          # scan the install (about 30 s)
    python3 site/build.py --cached           # reuse site/inventory.json

The output is site/data.js; index.html needs nothing else and no server.
"""

from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HERE = os.path.join(ROOT, 'site')
CACHE = os.path.join(HERE, 'inventory.json')
DOCS = ['docs/RCT3_OVL_FORMAT.md', 'docs/EXTRACTORS.md', 'README.md']
EXTRACTORS = os.path.join(ROOT, 'extractors', 'src')
BINARY = os.path.join(ROOT, 'build', 'cli', 'ovlextract')
DEFAULT_ASSETS = os.path.expanduser(
    '~/Library/Application Support/Steam/steamapps/common/'
    'RollerCoaster Tycoon 3 Complete Edition/RollerCoaster Tycoon 3 Platinum.app/Contents/Assets'
)
ORDER = ['extracted', 'partial', 'decoded', 'unknown']
BIG_PAIR = 80  # resources per pair above which only counts are shipped
TAG = re.compile(r'`([a-z]{3,4})`')
NUMBERED = re.compile(r'^\d+(\.\d+)*\.?\s*')


# --- the install --------------------------------------------------------------
def scan(assets: str) -> dict:
    """Run --list-loaders on every OVL pair and keep its resources and loaders."""
    pairs = []
    for folder, _dirs, files in os.walk(assets):
        for name in files:
            if name.endswith('.common.ovl'):
                pairs.append(os.path.join(folder, name))
    pairs.sort(key=str.lower)
    print(f"{len(pairs)} OVL pairs under {assets}", file=sys.stderr)

    def one(path: str) -> dict:
        stem = os.path.relpath(path, assets)[: -len('.common.ovl')].replace(os.sep, '/')
        size = os.path.getsize(path)
        other = path[: -len('.common.ovl')] + '.unique.ovl'
        if os.path.exists(other):
            size += os.path.getsize(other)
        run = subprocess.run([BINARY, '--list-loaders', path], capture_output=True, text=True)
        resources, loaders, section = [], {}, None
        for line in run.stdout.splitlines():
            if line.startswith('Loaders:'):
                section = 'loaders'
            elif line.startswith('Linked files:'):
                section = 'files'
            elif section == 'loaders' and 'tag=' in line:
                m = re.search(r'tag=(\S+)\s+name=(\S+)', line)
                if m:
                    loaders[m.group(1)] = m.group(2)
            elif section == 'files' and line.startswith('  '):
                resources.append(line.strip())
        return dict(
            p=stem,
            s=size,
            r=resources,
            loaders=loaders,
            error=(run.returncode != 0) or bool(run.stderr.strip()),
        )

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as pool:
        out = list(pool.map(one, pairs))
    errors = sum(1 for o in out if o['error'])
    if errors:
        print(f"{errors} pair(s) reported an error while listing", file=sys.stderr)
    return dict(assets=assets, pairs=out)


# --- what the docs and the code say -------------------------------------------
class Docs:
    """Headings and the loader registry of the Markdown documentation."""

    def __init__(self, paths: list[str]):
        self.sections: list[dict] = []  # file, title, line
        self.registry: dict[str, dict] = {}  # tag -> meaning, status text, line
        for rel in paths:
            path = os.path.join(ROOT, rel)
            if os.path.exists(path):
                self._parse(path, rel)

    def _parse(self, path: str, rel: str) -> None:
        fenced = False
        with open(path, encoding='utf-8') as fh:
            for no, line in enumerate(fh, 1):
                if line.startswith('```'):
                    fenced = not fenced
                    continue
                if fenced:
                    continue
                head = re.match(r'(#+)\s+(.*)', line)
                if head:
                    title = head.group(2).strip().replace('`', '')
                    self.sections.append(dict(f=rel, t=title, l=no))
                    continue
                # A registry row: | `tag`, `tag2` | meaning | status |
                row = re.match(r'\|\s*((?:`[a-z]{3,4}`[,\s]*)+)\|([^|]*)\|([^|]*)\|', line)
                if row and rel.endswith('RCT3_OVL_FORMAT.md'):
                    for tag in TAG.findall(row.group(1)):
                        self.registry.setdefault(
                            tag,
                            dict(meaning=row.group(2).strip(), status=row.group(3).strip(), l=no, f=rel),
                        )

    def heading_for(self, tag: str) -> dict | None:
        """The first heading that names the tag, as `tag` or in capitals."""
        for sec in self.sections:
            words = re.findall(r'[A-Za-z]{3,4}', sec['t'])
            if tag in words or tag.upper() in words:
                return dict(k='doc', f=sec['f'], t=sec['t'], l=sec['l'])
        return None

    def section_line(self, prefix: str) -> dict | None:
        for sec in self.sections:
            if NUMBERED.sub('', sec['t']).startswith(NUMBERED.sub('', prefix)) or sec['t'].startswith(prefix):
                return dict(k='doc', f=sec['f'], t=sec['t'], l=sec['l'])
        return None


def code_hits(folder: str) -> dict[str, list[dict]]:
    """Which extractor source quotes which tag, with the first line that does."""
    hits: dict[str, list[dict]] = defaultdict(list)
    for name in sorted(os.listdir(folder)):
        if not name.endswith('.cpp'):
            continue
        rel = os.path.relpath(os.path.join(folder, name), ROOT).replace(os.sep, '/')
        seen: set[str] = set()
        with open(os.path.join(folder, name), encoding='utf-8', errors='replace') as fh:
            for no, line in enumerate(fh, 1):
                for tag in re.findall(r'"([a-z]{3,4})"', line):
                    if tag not in seen:
                        seen.add(tag)
                        hits[tag].append(dict(k='code', f=rel, l=no))
    return hits


def judge(tag: str, docs: Docs, code: dict, notes: dict) -> dict:
    """Status, evidence and note for one loader tag."""
    manual = notes.get('tags', {}).get(tag)
    row = docs.registry.get(tag)
    sources = [h for h in code.get(tag, []) if 'Extractor' in h['f']]
    evidence: list[dict] = []
    heading = docs.heading_for(tag)
    if heading:
        evidence.append(heading)
    if row:
        evidence.append(dict(k='doc', f=row['f'], t='Loader registry', l=row['l']))
    evidence += sources[:2]

    note = None
    if manual:
        status, note = manual['status'], manual.get('note')
        ref = docs.section_line(manual['section']) if manual.get('section') else None
        if ref:
            evidence = [ref] + [e for e in evidence if e.get('l') != ref['l']]
    elif row:
        text = row['status'].lower()
        words = notes.get('status_words', {})
        if any(w in text for w in words.get('partial', [])):
            status = 'partial' if sources else 'decoded'
        elif any(w in text for w in words.get('decoded', [])):
            status = 'decoded'
        elif any(w in text for w in words.get('extracted', [])):
            status = 'extracted' if sources else 'decoded'
        else:
            status = 'partial' if sources else 'decoded'
    elif sources:
        status = 'partial'  # the code touches it, no document vouches for it
    elif heading:
        status = 'decoded'
    else:
        status = 'unknown'
    return dict(
        st=status,
        meaning=row['meaning'] if row else None,
        registry=row['status'] if row else None,
        ev=evidence[:4],
        note=note,
    )


# --- output -------------------------------------------------------------------
def git(*args: str) -> str:
    try:
        run = subprocess.run(['git', '-C', ROOT, *args], capture_output=True, text=True, check=True)
        return run.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ''


def build(inventory: dict) -> dict:
    with open(os.path.join(HERE, 'annotations.json'), encoding='utf-8') as fh:
        notes = json.load(fh)
    docs = Docs(DOCS)
    code = code_hits(EXTRACTORS)

    counts: Counter = Counter()
    files: Counter = Counter()
    names: dict[str, Counter] = defaultdict(Counter)
    for pair in inventory['pairs']:
        tags = Counter(sym.rsplit(':', 1)[-1] for sym in pair['r'] if ':' in sym)
        counts.update(tags)
        files.update(tags.keys())
        for tag, name in pair['loaders'].items():
            names[tag][name] += 1

    tags = {}
    for tag in sorted(set(counts) | set(files) | set(names)):
        info = judge(tag, docs, code, notes)
        info['n'] = counts[tag]
        info['files'] = files[tag]
        info['name'] = names[tag].most_common(1)[0][0] if names[tag] else None
        tags[tag] = info

    pairs = []
    for pair in inventory['pairs']:
        by_tag: dict[str, list[str]] = defaultdict(list)
        for sym in pair['r']:
            if ':' in sym:
                name, tag = sym.rsplit(':', 1)
                by_tag[tag].append(name)
        mix = [0, 0, 0, 0]
        for tag, syms in by_tag.items():
            mix[ORDER.index(tags[tag]['st'])] += len(syms)
        # Names are worth shipping for a stall or a track piece, not for the
        # 38 000 strings of a language file: past the cap only counts travel.
        big = len(pair['r']) > BIG_PAIR
        res = {tag: (len(syms) if big else syms) for tag, syms in by_tag.items()}
        pairs.append(dict(p=pair['p'], s=pair['s'], r=res, m=mix, e=1 if pair['error'] else 0))

    totals = [sum(p['m'][i] for p in pairs) for i in range(4)]
    remote = git('remote', 'get-url', 'origin')
    remote = re.sub(r'\.git$', '', remote.replace('git@github.com:', 'https://github.com/'))
    return dict(
        generated=datetime.date.today().isoformat(),
        commit=git('rev-parse', '--short', 'HEAD'),
        branch=git('rev-parse', '--abbrev-ref', 'HEAD') or 'main',
        dirty=bool(git('status', '--porcelain', '--', 'docs', 'extractors', 'README.md')),
        repo=remote if remote.startswith('https://') else '',
        assets=inventory['assets'],
        totals=totals,
        tags=tags,
        pairs=pairs,
    )


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('assets', nargs='?', help='Assets folder of the game install')
    ap.add_argument('--cached', action='store_true', help=f'reuse {os.path.relpath(CACHE, ROOT)}')
    args = ap.parse_args()

    if args.cached and os.path.exists(CACHE):
        with open(CACHE, encoding='utf-8') as fh:
            inventory = json.load(fh)
    else:
        assets = args.assets or DEFAULT_ASSETS
        if not os.path.isdir(assets):
            ap.error(f'no Assets folder at {assets}')
        if not os.path.exists(BINARY):
            ap.error(f'{BINARY} missing: build the CLI first (cmake --build build)')
        inventory = scan(assets)
        with open(CACHE, 'w', encoding='utf-8') as fh:
            json.dump(inventory, fh, ensure_ascii=False)

    site = build(inventory)
    total = sum(site['totals']) or 1
    print(
        f"{len(site['pairs'])} pairs, {total} resources, {len(site['tags'])} tags -- "
        + ', '.join(f"{s} {100 * n / total:.1f}%" for s, n in zip(ORDER, site['totals']))
    )
    for tag, info in sorted(site['tags'].items(), key=lambda kv: -kv[1]['n']):
        print(f"  {tag:<5} {info['st']:<10} {info['n']:>6} in {info['files']:>5} files  {info['name'] or ''}")

    out = os.path.join(HERE, 'data.js')
    with open(out, 'w', encoding='utf-8') as fh:
        fh.write('// Generated by site/build.py -- do not edit, rebuild instead.\n')
        fh.write('window.OVL_SITE = ')
        json.dump(site, fh, ensure_ascii=False, separators=(',', ':'))
        fh.write(';\n')
    print(f"-> {out} ({os.path.getsize(out) // 1024} KB)")
    return 0


if __name__ == '__main__':
    sys.exit(main())
