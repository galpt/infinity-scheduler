#!/usr/bin/env python3
"""
adapt-patches.py — Adapt Infinity patches for different kernel versions.

Strategy: for each hunk, extract the first significant context line and
search for it DIRECTLY in the target file.  This anchors the hunk at the
correct position within a function body, not just at the function start.
If the context line is not found, fall back to shifting by function position.

Usage:
  python3 adapt-patches.py \
    --source patches/stable/linux-7.0.12-infinity/ \
    --target /path/to/kernel-source \
    --output patches/stable/linux-7.X-infinity/
"""

import argparse
import os
import re
import subprocess
import sys

HUNK_RE = re.compile(r'^@@\s+-(\d+)(?:,(\d+))?\s+\+(\d+)(?:,(\d+))?\s+@@\s*(.*)$')


def get_clue_offset(hunk_lines, old_line):
    """Count how many pre-image (context or removed) lines appear in the
    hunk body BEFORE the first non-empty context line.  Returns offset."""
    count = 0
    for ln in hunk_lines:
        if ln.startswith(' ') and ln.strip():
            return count  # first non-empty context line found
        if ln.startswith(' ') or ln.startswith('-'):
            count += 1
        # + lines don't count toward pre-image offset
    return count


def parse_patch(path):
    """Read patch file, return (header_lines, list_of_sections).

    Each section is (preamble_lines, hunk_list), where preamble_lines
    includes the 'diff --git', 'index', '---', '+++' lines that precede
    the hunks for one file, and hunk_list is [(hdr, body), ...].
    """
    with open(path) as f:
        lines = f.readlines()

    # Split header from first @@ or diff line
    i = 0
    while i < len(lines):
        if lines[i].startswith('@@') or lines[i].startswith('diff --'):
            break
        i += 1
    header = lines[:i]

    sections = []
    while i < len(lines):
        # Collect preamble (diff --git, index, ---, +++, etc.)
        preamble = []
        while i < len(lines) and not lines[i].startswith('@@'):
            preamble.append(lines[i])
            i += 1
        # Collect hunks for this file
        hunks = []
        while i < len(lines) and not lines[i].startswith('diff --'):
            if not lines[i].startswith('@@'):
                i += 1
                continue
            hdr = lines[i]
            i += 1
            body = []
            while i < len(lines) and not lines[i].startswith('@@') \
                    and not lines[i].startswith('diff --'):
                if lines[i].startswith('---') or lines[i].startswith('+++'):
                    # These are file markers inside a hunk body — valid
                    pass
                body.append(lines[i])
                i += 1
            hunks.append((hdr, body))
        sections.append((preamble, hunks))
    return header, sections


def extract_func(hdr):
    """Extract the function name from the @@ header comment."""
    m = HUNK_RE.match(hdr)
    if not m or not m.group(5):
        return None
    rest = m.group(5).strip()
    paren = rest.find('(')
    name = rest[:paren].strip() if paren >= 0 else rest.strip()
    # Strip type qualifiers (in order of increasing length for correct stripping)
    for p in ['const', 'static', 'inline', 'unsigned', 'bool', 'int',
              'void', 'u64', 's32', 's64', 'u32', 'long', 'char',
              'struct', 'enum']:
        if name.startswith(p + ' '):
            name = name[len(p) + 1:].strip()
    return name.strip('* \t,')


def find_in_file(target_lines, text, start_from=0):
    """Find `text` in target_lines starting from start_from.
    Returns the 0-based line index of the first match or -1."""
    for i in range(start_from, len(target_lines)):
        if text in target_lines[i]:
            return i
    return -1


def compute_shift(hdr, body, target_lines, patch_lines, patch_hdr_idx):
    """Compute the per-hunk line-number shift based on function anchoring.

    Strategy:
      1. Find the function's DEFINITION in the target file by matching
         `func_name(` at a line that is NOT a comment or documentation.
      2. Compute shift = target_function_definition_line - old_line.
         This adjusts for the function being at a different position
         in the target vs the source kernel version.

    Returns the shift (int) to add to both old_line and new_line."""
    func_name = extract_func(hdr)
    if not func_name:
        return 0

    m = HUNK_RE.match(hdr)
    if not m:
        return 0
    old_line = int(m.group(1))

    # Find the FUNCTION DEFINITION: match `func_name(` but exclude:
    # - Comment/doc lines (starting with `*`, `/*`, `//`)
    # - Prototype declarations (semicolon at end: `func_name(...);`)
    # This gives us the actual function body, not a forward declaration.
    tgt_func_idx = -1
    fname_paren = func_name + '('
    for j, ln in enumerate(target_lines):
        if fname_paren not in ln:
            continue
        stripped = ln.lstrip()
        # Skip comments
        if stripped.startswith('*') or stripped.startswith('/*') or stripped.startswith('//'):
            continue
        # Skip forward declarations (prototypes end with `);`)
        if stripped.rstrip().endswith(');'):
            continue
        # Skip struct members (.func_name)
        if '.' in stripped.split(func_name)[0]:
            continue
        tgt_func_idx = j
        break
    if tgt_func_idx < 0:
        # Fallback: try finding func_name anywhere (may hit comments)
        for j, ln in enumerate(target_lines):
            if func_name in ln:
                tgt_func_idx = j
                break
    if tgt_func_idx < 0:
        return 0

    # Shift = target_function_line - old_line.
    # The hunk was at old_line in the source.  In the target, the
    # function definition is at tgt_func_idx+1.  The hunk's position
    # within the function body may differ by a few lines between
    # versions, but -F 10 handles that.
    tgt_def_line = tgt_func_idx + 1
    return tgt_def_line - old_line


def _normalize_body(body):
    """Ensure every line in a hunk body starts with a valid unified diff prefix
    (space, '+', '-', or backslash).  Lines with no prefix (e.g. starting with
    a tab) get a space prepended so they become context lines."""
    result = []
    for ln in body:
        if ln == '\n':
            result.append(' \n')
        elif ln == '\r\n':
            result.append(' \r\n')
        elif ln.startswith(' ') or ln.startswith('+') \
                or ln.startswith('-') or ln.startswith('\\'):
            result.append(ln)
        else:
            # Tab-prefixed or other unrecognized context line
            result.append(' ' + ln)
    return result


def _count_hunk_body(body):
    """Count pre-image and new-file lines in a hunk body.

    Context (space) → both pre-image and new-file.
    Removed ('-')   → pre-image only.
    Added ('+')     → new-file only.
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


def process(source_dir, target_dir, output_dir):
    target_file = os.path.join(target_dir, 'kernel', 'sched', 'fair.c')
    if not os.path.exists(target_file):
        print(f"ERROR: {target_file} not found", file=sys.stderr)
        sys.exit(1)
    with open(target_file) as f:
        target_lines = f.readlines()

    print(f"Target: {target_file} ({len(target_lines)} lines)")
    os.makedirs(output_dir, exist_ok=True)

    for fname in sorted(os.listdir(source_dir)):
        if not fname.endswith('.patch'):
            continue
        src = os.path.join(source_dir, fname)
        dst = os.path.join(output_dir, fname)

        # Parse the patch into header + sections
        patch_header, sections = parse_patch(src)
        if not sections or all(len(h) == 0 for _, h in sections):
            print(f"  {fname}: no hunks, copying as-is")
            with open(dst, 'w') as f:
                f.writelines(patch_header)
            continue

        # Build output: header + adapted sections
        adapted = list(patch_header)
        total_hunks = 0

        for preamble, hunks in sections:
            adapted.extend(preamble)

            # Track cumulative shift for hunks without function anchors
            cumulative_shift = 0

            # Adapt each hunk: compute new position, normalize body,
            # recalculate counts
            adapted_hunks = []
            for hdr, body in hunks:
                total_hunks += 1
                m = HUNK_RE.match(hdr)
                if not m:
                    cumulative_shift = 0
                    adapted_hunks.append((0, hdr, body))
                    continue

                old_line = int(m.group(1))
                old_cnt = int(m.group(2)) if m.group(2) else 1
                new_line = int(m.group(3))
                new_cnt = int(m.group(4)) if m.group(4) else 1
                func_hdr = m.group(5) or ''

                shift = compute_shift(hdr, body, target_lines, None, None)

                # If no function anchor found, apply cumulative shift
                # from the previous function-anchored hunk
                if shift == 0 and extract_func(hdr) is None:
                    shift = cumulative_shift

                new_old = old_line + shift
                new_new = new_line + shift

                norm_body = _normalize_body(body)
                pre_actual, new_actual = _count_hunk_body(norm_body)

                new_hdr = (f"@@ -{new_old},{pre_actual}"
                           f" +{new_new},{new_actual} @@ {func_hdr}\n")

                adapted_hunks.append((new_old, new_hdr, norm_body))
                cumulative_shift = shift

            # Sort hunks by adapted position to avoid "misordered hunks"
            adapted_hunks.sort(key=lambda x: x[0])
            for _, new_hdr, norm_body in adapted_hunks:
                adapted.append(new_hdr)
                adapted.extend(norm_body)

        with open(dst, 'w') as f:
            f.writelines(adapted)

        print(f"  {fname}: {total_hunks} hunk(s)")

    print(f"\nDone. Output: {output_dir}/")


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--source', required=True)
    p.add_argument('--target', required=True)
    p.add_argument('--output', required=True)
    args = p.parse_args()
    if not os.path.isdir(args.source):
        print(f"ERROR: {args.source} not found", file=sys.stderr)
        sys.exit(1)
    if not os.path.isdir(args.target):
        print(f"ERROR: {args.target} not found", file=sys.stderr)
        sys.exit(1)
    process(args.source, args.target, args.output)


if __name__ == '__main__':
    main()
