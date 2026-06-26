#!/usr/bin/env python3
"""
fix-patch-format.py — Fix empty context lines in unified diff patches.

In unified diff format, every context line MUST start with a single space.
An empty context line must be `` \n`` (space + newline), NOT just ``\n``.

This script reads each patch, identifies lines inside hunk bodies that are
bare newlines (empty lines that should be context), and prefixes them with
`` `` (a single space).

Handles multi-file patches correctly by tracking diff --git boundaries
and NOT modifying lines outside hunk bodies (e.g. git version trailers
like "-- 2.54.0").

Usage:
  python3 fix-patch-format.py patches/stable/*/*.patch
  python3 fix-patch-format.py --rewrite patches/stable/*/*.patch
"""

import os
import re
import sys

HUNK_HEADER_RE = re.compile(r'^@@ ')
GIT_TRAILER_RE = re.compile(r'^-- $')


def fix_patch(path, in_place=False):
    with open(path, 'r', newline='') as f:
        lines = f.readlines()

    fixed = []
    in_hunk_body = False
    in_diff_section = False

    for i, line in enumerate(lines):
        # Track whether we're inside any diff section at all
        if line.startswith('diff --git '):
            in_diff_section = True
            in_hunk_body = False
            fixed.append(line)
        elif GIT_TRAILER_RE.match(line):
            # Git format-patch trailer: "-- 2.54.0\n" — NOT a hunk line
            in_hunk_body = False
            fixed.append(line)
        elif HUNK_HEADER_RE.match(line):
            in_hunk_body = True
            fixed.append(line)
        elif line.startswith('---') or (line.startswith('diff ') and not line.startswith('diff --git ')):
            # "diff --git" is handled above; plain "diff " is a fallback
            in_hunk_body = False
            fixed.append(line)
        elif not in_diff_section:
            # Lines before the first diff --git (email headers) — pass through
            fixed.append(line)
        elif not in_hunk_body:
            # Between sections: diff metadata lines (index, ---, +++) — pass through
            fixed.append(line)
        elif in_hunk_body:
            # Inside a hunk body: lines must start with ' ', '+', '-', or '\'
            if line == '\n':
                fixed.append(' \n')
            elif line == '\r\n':
                fixed.append(' \r\n')
            elif line.startswith(' ') or line.startswith('+') \
                    or line.startswith('-') or line.startswith('\\'):
                fixed.append(line)
            else:
                fixed.append(' ' + line)

    content = ''.join(fixed)

    if content != ''.join(lines):
        if in_place:
            with open(path, 'w', newline='') as f:
                f.write(content)
            print(f"  FIXED {path}")
        else:
            print(f"  WOULD FIX {path}")
        return True
    else:
        print(f"  OK    {path}")
        return False


def main():
    paths = [p for p in sys.argv[1:] if not p.startswith('--')]
    in_place = '--rewrite' in sys.argv or '--in-place' in sys.argv

    if not paths:
        print("Usage: python3 fix-patch-format.py [--rewrite] <patch-file> [patch-file...]")
        print("  --rewrite   Actually modify files (default: dry-run)")
        sys.exit(1)

    fixed_count = 0
    for p in paths:
        if os.path.isfile(p):
            if fix_patch(p, in_place):
                fixed_count += 1

    print(f"\n{fixed_count} file(s) would be / were fixed.")
    return 0 if fixed_count == 0 else 1


if __name__ == '__main__':
    sys.exit(main())
