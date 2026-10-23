#!/usr/bin/env python3
"""Check package metadata and the candidate binary, including release-tag builds."""
import json
import os
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parent.parent
name = "lmmolecule"
version = (root / "VERSION").read_text(encoding="utf-8").strip()
if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
    sys.exit("VERSION must contain one stable semantic version")
cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
match = re.search(r"project\(" + name + r" VERSION ([0-9.]+)\b", cmake)
if not match or match[1] != version:
    sys.exit("CMake project version differs from VERSION")
schema = json.loads((root / "schema" / (name + ".dsl.json")).read_text(encoding="utf-8"))
if schema.get("name") != name or schema.get("dsl.schema") != 2:
    sys.exit("schema name or DSL format version differs from the host contract")
notices = (root / "THIRD_PARTY_NOTICES").read_text(encoding="utf-8")
if not notices.startswith(name + " " + version + " includes"):
    sys.exit("notice package version differs from VERSION")
ref = os.environ.get("GITHUB_REF", "")
if ref.startswith("refs/tags/v") and ref != "refs/tags/v" + version:
    sys.exit("release tag differs from VERSION")
host = str(Path(sys.argv[1]).resolve())
for args, expected in [(["--version"], name + " " + version + "\n"),
                       (["--notices"], notices),
                       (["--dsl-schema", "--json"], (root / "schema" / (name + ".dsl.json")).read_text(encoding="utf-8"))]:
    actual = subprocess.run([host] + args, check=True, capture_output=True, text=True, timeout=30).stdout
    if actual != expected:
        sys.exit("candidate " + " ".join(args) + " differs from package metadata")
print(name + " " + version + ": source, binary, schema, notices and release tag agree")
