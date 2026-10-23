#!/usr/bin/env python3
"""Validate the repository's README visual assets and example companions.

This intentionally uses only the Python standard library so it can run before
the project has been built and on the supported CI platforms.
"""

from __future__ import annotations

import argparse
import os
import re
import struct
import sys
from pathlib import Path
from urllib.parse import urlparse


PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
GIF_MAGICS = (b"GIF87a", b"GIF89a")
MAX_PNG_BYTES = 300 * 1024
MAX_GIF_BYTES = 3 * 1024 * 1024


def local_target(raw: str) -> str | None:
    """Return a local path from a Markdown/HTML URL, or None for remote URLs."""
    target = raw.strip().strip("<>")
    if not target or target.startswith(("#", "/", "//")):
        return None
    parsed = urlparse(target)
    if parsed.scheme or parsed.netloc:
        return None
    return parsed.path


def image_info(path: Path) -> tuple[str, int, int, int]:
    data = path.read_bytes()
    size = len(data)
    if data.startswith(PNG_MAGIC):
        if size < 24 or data[12:16] != b"IHDR":
            raise ValueError("truncated PNG")
        width, height = struct.unpack(">II", data[16:24])
        if width == 0 or height == 0:
            raise ValueError("zero-sized PNG")
        if size > MAX_PNG_BYTES:
            raise ValueError(f"PNG is {size} bytes; limit is {MAX_PNG_BYTES}")
        return "PNG", width, height, size
    if data.startswith(GIF_MAGICS):
        if size < 10:
            raise ValueError("truncated GIF")
        width, height = struct.unpack("<HH", data[6:10])
        if width == 0 or height == 0:
            raise ValueError("zero-sized GIF")
        if size > MAX_GIF_BYTES:
            raise ValueError(f"GIF is {size} bytes; limit is {MAX_GIF_BYTES}")
        return "GIF", width, height, size
    raise ValueError("unrecognised PNG/GIF magic bytes")


def markdown_targets(text: str) -> set[str]:
    # Markdown images, ordinary links, and HTML image sources are all local
    # references a rendered README can make to an asset or source example.
    raw_targets = re.findall(r"!?(?:\[[^\]]*\])\(([^)\s]+)(?:\s+[^)]*)?\)", text)
    raw_targets += re.findall(r"<img\b[^>]*?\bsrc=[\"']([^\"']+)", text, re.I)
    return {target for raw in raw_targets if (target := local_target(raw))}


def image_targets(text: str) -> set[str]:
    raw_targets = re.findall(r"!\[[^\]]*\]\(([^)\s]+)(?:\s+[^)]*)?\)", text)
    raw_targets += re.findall(r"<img\b[^>]*?\bsrc=[\"']([^\"']+)", text, re.I)
    return {target for raw in raw_targets if (target := local_target(raw))}


def check_document_refs(root: Path, document: Path, errors: list[str]) -> None:
    for target in sorted(markdown_targets(document.read_text(encoding="utf-8"))):
        path = (document.parent / target).resolve()
        try:
            path.relative_to(root.resolve())
        except ValueError:
            errors.append(f"{document.relative_to(root)} reference escapes repository: {target}")
            continue
        if not path.is_file() and not path.is_dir():
            errors.append(f"{document.relative_to(root)} reference does not exist: {target}")
            continue
        if path.suffix.lower() in (".png", ".gif"):
            try:
                kind, width, height, size = image_info(path)
                print(f"visual: {document.relative_to(root)}:{target} ({kind} {width}x{height}, {size} bytes)")
            except (OSError, ValueError) as exc:
                errors.append(f"invalid {document.relative_to(root)} image {target}: {exc}")


def check_readme_gallery(root: Path, errors: list[str]) -> None:
    readme = root / "README.md"
    text = readme.read_text(encoding="utf-8")
    preamble = text.split("\n## ", 1)[0]
    images = image_targets(preamble)
    if not 3 <= len(images) <= 6:
        errors.append(f"README gallery must contain 3-6 images before the first ## heading (found {len(images)})")
    sources = re.findall(r"\((examples/[^)\s]+\.lms)\)", preamble)
    autoruns = re.findall(r"lmctl autorun\s+\S+", preamble)
    if len(sources) < len(images):
        errors.append(f"README gallery has {len(images)} images but only {len(sources)} linked example sources")
    if len(autoruns) < len(images):
        errors.append(f"README gallery has {len(images)} images but only {len(autoruns)} lmctl autorun commands")


def companion_candidates(root: Path, source: Path) -> list[Path]:
    rel = source.relative_to(root / "examples").with_suffix("")
    # Keep the convention predictable while allowing a nested example to use
    # either its basename or its path with separators converted to '-'.
    names = [rel.name, str(rel).replace(os.sep, "-")]
    return [root / "docs" / "images" / f"{name}{ext}" for name in dict.fromkeys(names) for ext in (".png", ".gif")]


def check(root: Path) -> list[str]:
    errors: list[str] = []
    readme = root / "README.md"
    if not readme.is_file():
        return ["README.md is missing"]

    # Validate generated assets even when a README edit has not linked one yet;
    # this catches truncated or accidentally oversized files in CI early.
    image_root = root / "docs" / "images"
    if image_root.is_dir():
        for path in sorted(image_root.rglob("*")):
            if path.is_file() and path.suffix.lower() in (".png", ".gif"):
                try:
                    kind, width, height, size = image_info(path)
                    print(f"asset: {path.relative_to(root)} ({kind} {width}x{height}, {size} bytes)")
                except (OSError, ValueError) as exc:
                    errors.append(f"invalid generated image {path.relative_to(root)}: {exc}")

    check_document_refs(root, readme, errors)
    gallery = root / "docs" / "gallery.md"
    if gallery.is_file():
        check_document_refs(root, gallery, errors)
    else:
        errors.append("docs/gallery.md is missing")
    check_readme_gallery(root, errors)

    for source in sorted((root / "examples").rglob("*.lms")):
        candidates = companion_candidates(root, source)
        companion = next((path for path in candidates if path.is_file()), None)
        if companion is None:
            rel_candidates = ", ".join(str(path.relative_to(root)) for path in candidates)
            errors.append(f"example has no companion visual: {source.relative_to(root)} (tried {rel_candidates})")
            continue
        try:
            kind, width, height, size = image_info(companion)
            print(f"companion: {source.relative_to(root)} -> {companion.relative_to(root)} ({kind} {width}x{height}, {size} bytes)")
        except (OSError, ValueError) as exc:
            errors.append(f"invalid companion {companion.relative_to(root)}: {exc}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", nargs="?", default=Path(__file__).resolve().parents[1], type=Path)
    args = parser.parse_args()
    errors = check(args.root.resolve())
    if errors:
        for error in errors:
            print(f"visual check: ERROR: {error}", file=sys.stderr)
        return 1
    print("visual check: all README references and example companions are valid")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
