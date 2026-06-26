#!/usr/bin/env python3
"""Fix hunk header line counts in unified diff patches.

Reads a unified diff patch (single or multi-file), for each hunk counts the
actual pre-image (context + '-') and new-file (context + '+') lines in the
body, and rewrites the hunk header to match.

Handles multi-file patches (git format-patch output with multiple diff --git
sections) correctly by splitting on diff --git boundaries first.

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
        elif ln.startswith('+'):
            added += 1
    return pre, added


def parse_patch(path):
    """Parse a patch file into sections.

    Returns (email_header, sections) where email_header is the metadata lines
    before the first diff --git (From/Date/Subject/Signed-off-by), and sections
    is a list of (preamble, hunks) tuples.
    """
    with open(path, 'r', newline='') as f:
        lines = f.readlines()

    # Find first diff --git to split email header from body
    first_diff = None
    for i, ln in enumerate(lines):
        if ln.startswith('diff --git '):
            first_diff = i
            break

    if first_diff is None:
        # No diff --git at all — not a multi-file patch, treat the whole
        # file as a single section
        return [], [([], lines)]

    email_header = lines[:first_diff]
    body = lines[first_diff:]

    # Split body on diff --git boundaries
    sections = []
    i = 0
    while i < len(body):
        if body[i].startswith('diff --git '):
            preamble = []
            while i < len(body) and not body[i].startswith('@@'):
                preamble.append(body[i])
                i += 1
            # Collect hunks
            hunks = []
            while i < len(body):
                if body[i].startswith('@@'):
                    hdr = body[i]
                    i += 1
                    hunk_body = []
                    while i < len(body) and not body[i].startswith('@@') \
                          and not body[i].startswith('diff --git ') \
                          and not re.match(r'^-- $', body[i]):
                        hunk_body.append(body[i])
                        i += 1
                    hunks.append((hdr, hunk_body))
                elif body[i].startswith('diff --git '):
                    break
                else:
                    i += 1
            sections.append((preamble, hunks))
        else:
            i += 1

    return email_header, sections


def fix_section(preamble, hunks, path):
    """Fix hunk counts in one section, return (preamble, fixed_hunks, changed)."""
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

        new_hdr = f"@@ -{old_start},{pre_actual} +{new_start},{new_actual} @@{func_hdr}\n"
        fixed_hunks.append((new_hdr, body))
        changed = True
        file = "unknown"
        for ln in preamble:
            if ln.startswith('+++ b/'):
                file = ln[6:].strip()
                break
        print(f"  {path}: {file} hunk @{old_start}: ({old_cnt},{new_cnt}) -> ({pre_actual},{new_actual})")

    return preamble, fixed_hunks, changed


def fix_patch(path, in_place=False):
    email_header, sections = parse_patch(path)

    if not sections:
        print(f"  {path}: no diff sections, skipping")
        return False

    # The email header may contain trailing metadata lines after the last
    # diff section (e.g. "-- 2.54.0" from git format-patch).  Strip them.
    trailer_start = None
    for i, ln in enumerate(email_header):
        if re.match(r'^-- $', ln):
            trailer_start = i
            break
    if trailer_start is not None:
        email_header = email_header[:trailer_start]

    fixed_sections = []
    any_changed = False
    for preamble, hunks in sections:
        p, h, changed = fix_section(preamble, hunks, path)
        fixed_sections.append((p, h))
        any_changed = any_changed or changed

    if not any_changed:
        print(f"  {path}: all counts correct")
        return False

    if in_place:
        output = list(email_header)
        for preamble, hunks in fixed_sections:
            output.extend(preamble)
            for hdr, body in hunks:
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
