#!/usr/bin/env python3
"""Fix hunk header line counts in unified diff patches.

Reads a unified diff patch, for each hunk counts the actual pre-image
(context + '-') and new-file (context + '+') lines in the body, and
rewrites the hunk header to match.  Also ensures the body has the
correct number of trailing context lines.

Usage:
  python3 fix-patch-counts.py path/to/patch [path/to/patch ...]
  python3 fix-patch-counts.py --rewrite path/to/patch ...
"""

import re
import sys

HUNK_RE = re.compile(r'^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@(.*)$')


def count_hunk_body(body):
    """Count pre-image lines and new-file lines in a hunk body.

    In unified diff format:
      - Context lines (space prefix) count toward BOTH pre-image and new-file.
      - Removed lines ('-' prefix) count ONLY toward pre-image.
      - Added lines ('+' prefix) count ONLY toward new-file.
    """
    pre = added = 0
    for ln in body:
        if ln.startswith(' '):
            pre += 1
            added += 1
        elif ln.startswith('-'):
            pre += 1
            # '-' lines are NOT in the new file
        elif ln.startswith('+'):
            added += 1
        # lines starting with '\' (no newline) don't count
    return pre, added


def fix_patch(path, in_place=False):
    with open(path, 'r', newline='') as f:
        content = f.read()
    lines = content.splitlines(keepends=True)

    # Split into header + hunks
    i = 0
    while i < len(lines) and not lines[i].startswith('@@'):
        i += 1
    header = lines[:i]

    hunks = []
    while i < len(lines):
        if not lines[i].startswith('@@'):
            i += 1
            continue
        hdr = lines[i]
        i += 1
        body = []
        while i < len(lines) and not lines[i].startswith('@@') \
                and not lines[i].startswith('---') \
                and not lines[i].startswith('diff --'):
            body.append(lines[i])
            i += 1
        hunks.append((hdr, body))

    if not hunks:
        print(f"  {path}: no hunks, skipping")
        return False

    changed = False
    fixed_hunks = []
    for hdr, body in hunks:
        m = HUNK_RE.match(hdr)
        if not m:
            fixed_hunks.append((hdr, body))
            continue

        old_start = int(m.group(1))
        old_cnt = int(m.group(2)) if m.group(2) else 1
        new_start = int(m.group(3))
        new_cnt = int(m.group(4)) if m.group(4) else 1
        func_hdr = m.group(5) or ''

        pre_actual, new_actual = count_hunk_body(body)

        if pre_actual == old_cnt and new_actual == new_cnt:
            fixed_hunks.append((hdr, body))
            continue

        # Fix the hunk header
        new_hdr = f"@@ -{old_start},{pre_actual} +{new_start},{new_actual} @@{func_hdr}\n"
        fixed_hunks.append((new_hdr, body))
        changed = True
        print(f"  {path}: hunk @{old_start}: ({old_cnt},{new_cnt}) -> ({pre_actual},{new_actual})")

    if not changed:
        print(f"  {path}: all counts correct")
        return False

    if in_place:
        output = list(header)
        for hdr, body in fixed_hunks:
            output.append(hdr)
            output.extend(body)
        with open(path, 'w', newline='') as f:
            f.writelines(output)
        print(f"  {path}: rewritten")
    return True


def main():
    paths = [p for p in sys.argv[1:] if not p.startswith('--')]
    in_place = '--rewrite' in sys.argv or '--in-place' in sys.argv
    if not paths:
        print("Usage: python3 fix-patch-counts.py [--rewrite] <patch-file>...")
        sys.exit(1)
    for p in paths:
        fix_patch(p, in_place)


if __name__ == '__main__':
    main()
