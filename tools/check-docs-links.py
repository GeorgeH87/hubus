#!/usr/bin/env python3
"""Checks every relative link in the Markdown files of the repository (tracked or new, not ignored): the target file must exist and a #anchor must match a
heading of the target (GitHub anchor rules). Prints one line per broken link and exits with 1 if there is any."""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LINK = re.compile(r"!?\[[^\]]*\]\(([^)\s]+)(?:\s+\"[^\"]*\")?\)")
HEADING = re.compile(r"^(#{1,6})\s+(.*?)\s*#*\s*$")
FENCE = re.compile(r"^\s*(```|~~~)")


def slug(text):
    """GitHub heading anchor: lower case, markup and punctuation removed, spaces to hyphens."""
    text = re.sub(r"`([^`]*)`", r"\1", text)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = text.strip().lower()
    text = re.sub(r"[^\w\- ]", "", text)
    return text.replace(" ", "-")


def lines_outside_code(path):
    in_fence = False
    for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if FENCE.match(line):
            in_fence = not in_fence
            continue
        if not in_fence:
            yield number, line


def anchors(path, cache={}):
    if path not in cache:
        seen = {}
        result = set()
        for _, line in lines_outside_code(path):
            m = HEADING.match(line)
            if not m:
                continue
            base = slug(m.group(2))
            n = seen.get(base, 0)
            seen[base] = n + 1
            result.add(base if n == 0 else f"{base}-{n}")
        cache[path] = result
    return cache[path]


def main():
    files = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "*.md"], cwd=ROOT,
                           capture_output=True, text=True, check=True)
    broken = 0
    checked = 0
    for name in files.stdout.split():
        source = ROOT / name
        if not source.exists():
            continue
        for number, line in lines_outside_code(source):
            for target in LINK.findall(re.sub(r"`[^`]*`", "", line)):
                if re.match(r"^[a-z]+:", target):
                    continue
                checked += 1
                file_part, _, anchor = target.partition("#")
                dest = (source.parent / file_part).resolve() if file_part else source
                problem = None
                if not dest.exists():
                    problem = "missing file"
                elif anchor and dest.suffix == ".md" and anchor not in anchors(dest):
                    problem = f"no heading for #{anchor}"
                if problem:
                    print(f"{name}:{number}: {target}: {problem}")
                    broken += 1
    print(f"{checked} links checked, {broken} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())
