#!/usr/bin/env python3
"""Render the model prompt with exact copies of the fixed AST and baseline."""

import argparse
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCES = {
    "AST_HEADER": "include/ast.h",
    "BASELINE_CODEGEN": "src/codegen.c",
}


def render(version):
    directory = ROOT / version
    template = (directory / "prompts/compiler.template.md").read_text(encoding="utf-8")
    markers = set(re.findall(r"\{\{([A-Z_]+)\}\}", template))
    if markers != set(SOURCES):
        raise ValueError(f"Unexpected prompt markers: {markers ^ set(SOURCES)}")
    for marker, source in SOURCES.items():
        template = template.replace("{{" + marker + "}}",
                                    (directory / source).read_text(encoding="utf-8").rstrip("\n"))
    return template


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="fail if the rendered prompt is stale")
    parser.add_argument("--version", choices=("v1", "v2"), help="default: generate/check both versions")
    args = parser.parse_args()
    for version in (args.version,) if args.version else ("v1", "v2"):
        prompt = ROOT / version / "prompts/compiler.md"
        content = render(version)
        if args.check:
            if not prompt.exists() or prompt.read_text(encoding="utf-8") != content:
                print(f"{version} prompt is stale; run make prompt-{version}", file=sys.stderr)
                return 1
            print(f"{version} prompt matches AST and baseline codegen")
        else:
            prompt.write_text(content, encoding="utf-8")
            print(f"Wrote {prompt.relative_to(ROOT)} ({len(content.encode('utf-8')):,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
