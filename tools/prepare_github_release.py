#!/usr/bin/env python3
"""Check/export the public cxf working tree; never change Git state or print secrets."""
import argparse
import hashlib
import ipaddress
import json
from pathlib import Path
import re
import struct
import subprocess
from urllib.parse import unquote, urlsplit
import zipfile
import zlib

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "docs/github-release-manifest.json"
ROOT_FILES = {".gitignore", "AGENTS.md", "Makefile", "README.md", "README.zh-CN.md",
              "agentctl.example.json", "cxf.sh", "cxf.command"}
OPTIONAL_FILES = {"LICENSE", "NOTICE"}
TYPES = {"include": {".h"}, "src": {".c", ".h"},
         "tests": {".c", ".h", ".cpp", ".mm", ".py"},
         "tools": {".py", ".c", ".sh"},
         "docs": {".md", ".json", ".jsonl", ".ndjson", ".csv", ".svg", ".png"}}
PATTERNS = {
    "home_path": r"/(?:Users|home)/[A-Za-z0-9_.-]+/|[A-Za-z]:\\Users\\[A-Za-z0-9_.-]+\\",
    "personal_model_default": r"Documents/" + r"test/agent-context-lab",
    "secret_token": r"\b(?:sk-(?:proj-|ant-)?[A-Za-z0-9_-]{20,}|gh[pousr]_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[A-Z0-9]{16})",
    "bearer_value": r"Bearer\s+[A-Za-z0-9_.-]{16,}",
    "query_token": r"(?i)[?&](?:api[_-]?key|access_token|token)=[A-Za-z0-9_-]{12,}",
    "private_key": r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----",
    "url_credentials": r"https?://[^\s/@]+:[^\s/@]+@",
    "nonempty_key_assignment": r'''(?i)(?:["']?(?:api[_-]?key|password|access[_-]?token|CXF_LLM_KEY)["']?|["']key["'])\s*[=:]\s*["'][^"'\n]{4,}["']''',
}


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def public_files(root=ROOT):
    files = [root / n for n in sorted(ROOT_FILES)]
    files += [root / n for n in sorted(OPTIONAL_FILES) if (root / n).exists()]
    for directory, suffixes in TYPES.items():
        for p in sorted((root / directory).rglob("*")):
            if "__pycache__" in p.parts or any(part.startswith(".") for part in p.relative_to(root).parts):
                continue
            if p.is_symlink() or not p.resolve().is_relative_to(root.resolve()):
                raise ValueError(f"symlink rejected: {p.relative_to(root)}")
            if p.is_file() and p.suffix in suffixes and p != root / "docs/github-release-manifest.json":
                files.append(p)
    return sorted(files)


def scan_text(name, text):
    """Return only locations/rules. A caller must never log matching values."""
    findings = []
    for rule, pattern in PATTERNS.items():
        for match in re.finditer(pattern, text):
            findings.append({"file": name, "line": text.count("\n", 0, match.start()) + 1, "rule": rule})
    for match in re.finditer(r'''https?://[^\s"'`<>]+''', text):
        try:
            host = urlsplit(match.group().rstrip("),.;")).hostname or ""
        except ValueError:
            host = ""
        try:
            address = ipaddress.ip_address(host)
        except ValueError:
            address = None
        # Loopback URLs are generic examples and deterministic test endpoints.
        private = address is not None and not address.is_global and not address.is_loopback
        private = private or host.endswith((".local", ".internal", ".lan"))
        # Real model-service URLs should be supplied at deployment, never embedded.
        endpoint = "/chat/completions" in match.group() and host not in {"host", "localhost", "example.com", "%s"}
        endpoint = endpoint and not (address is not None and address.is_loopback)
        if private or endpoint:
            findings.append({"file": name, "line": text.count("\n", 0, match.start()) + 1,
                             "rule": "private_or_embedded_service_url"})
    return findings


def git_candidates():
    check = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=ROOT, text=True, capture_output=True)
    if check.returncode or Path(check.stdout.strip()).resolve() != ROOT:
        return None
    result = subprocess.run(["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
                            cwd=ROOT, capture_output=True, check=True)
    return set(result.stdout.decode().split("\0")) - {""}


def png_metadata(data):
    if not data.startswith(b"\x89PNG\r\n\x1a\n"):
        raise ValueError("invalid PNG signature")
    texts, position = [], 8
    while position < len(data):
        if position + 12 > len(data):
            raise ValueError("truncated PNG chunk")
        size = struct.unpack(">I", data[position:position + 4])[0]
        kind = data[position + 4:position + 8]
        end = position + 12 + size
        if end > len(data):
            raise ValueError("truncated PNG payload")
        payload = data[position + 8:end - 4]
        if kind in (b"tEXt", b"eXIf"):
            texts.append(payload.decode("latin-1"))
        elif kind in (b"zTXt", b"iCCP"):
            _, rest = payload.split(b"\0", 1)
            texts.append(zlib.decompress(rest[1:]).decode("latin-1"))
        elif kind == b"iTXt":
            _, rest = payload.split(b"\0", 1)
            flag = rest[0]
            _, _, text = rest[2:].split(b"\0", 2)
            texts.append((zlib.decompress(text) if flag else text).decode("utf-8"))
        position = end
    return texts


def validate(files):
    failures = []
    allowed = {p.relative_to(ROOT).as_posix() for p in files} | {MANIFEST.relative_to(ROOT).as_posix()}
    candidates = git_candidates()
    if candidates is not None:
        for name in sorted(candidates - allowed):
            failures.append({"file": name, "rule": "git_visible_but_not_allowlisted"})
        for name in sorted(allowed - candidates):
            failures.append({"file": name, "rule": "public_file_missing_or_ignored"})
    for p in files:
        name = p.relative_to(ROOT).as_posix()
        if not p.is_file() or p.is_symlink():
            failures.append({"file": name, "rule": "missing_or_symlink"})
            continue
        if p.stat().st_size >= 100_000_000:
            failures.append({"file": name, "rule": "oversize_for_source_release"})
        if p.suffix == ".png":
            try:
                for text in png_metadata(p.read_bytes()):
                    failures.extend(scan_text(name, text))
            except (ValueError, IndexError, zlib.error):
                failures.append({"file": name, "rule": "invalid_png_metadata"})
            continue
        try:
            text = p.read_text(encoding="utf-8")
        except UnicodeError:
            failures.append({"file": name, "rule": "unexpected_binary"})
            continue
        failures.extend(scan_text(name, text))
        if p.suffix == ".md":
            # Ignore fenced code: historical examples may contain Markdown-shaped payloads.
            prose = re.sub(r"^```[^\n]*\n.*?^```\s*$", "", text, flags=re.M | re.S)
            for target in re.findall(r"!?\[[^\]]*\]\(([^)]+)\)", prose):
                if re.match(r"[a-zA-Z][a-zA-Z0-9+.-]*:", target):
                    continue
                target = unquote(target.split("#", 1)[0])
                if not target:
                    continue
                dest = (p.parent / target).resolve()
                if not dest.is_relative_to(ROOT) or (dest.is_file() and dest.relative_to(ROOT).as_posix() not in allowed) or not dest.exists():
                    failures.append({"file": name, "rule": "missing_or_excluded_local_link"})
    example = json.loads((ROOT / "agentctl.example.json").read_text())
    if example["api"]["key"] != "" or example["api"]["model"] != "local-model":
        failures.append({"file": "agentctl.example.json", "rule": "non_generic_example"})
    if failures:
        raise ValueError(json.dumps(failures, ensure_ascii=False, indent=2))
    return candidates is not None


def manifest(files):
    return {"schema_version": 1, "prepared": "2026-11-02",
            "scope": "Allowlisted public source and evidence working tree; excludes Git objects and local data; not a new inference benchmark",
            "license": "selected" if (ROOT / "LICENSE").exists() else "not_selected",
            "files": [{"path": p.relative_to(ROOT).as_posix(), "bytes": p.stat().st_size, "sha256": digest(p)} for p in files]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true", help="require existing manifest to match exactly")
    parser.add_argument("--export", type=Path, help="write a new deterministic source ZIP after checks")
    args = parser.parse_args()
    files = public_files()
    if not args.check and not MANIFEST.exists():
        # Reserve its Git-visible filename before validating the candidate set.
        MANIFEST.write_text("{}\n")
    has_git = validate(files)
    data = manifest(files)
    encoded = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    if args.check:
        if not MANIFEST.exists() or MANIFEST.read_text() != encoded:
            raise ValueError("stale source manifest; regenerate after reviewing changes")
    else:
        MANIFEST.write_text(encoded)
    if args.export:
        if args.export.exists():
            raise ValueError("choose a fresh export path; existing artifacts are preserved")
        args.export.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(args.export, "x", zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
            for p in sorted(files + [MANIFEST]):
                info = zipfile.ZipInfo(p.relative_to(ROOT).as_posix(), (2026, 11, 2, 0, 0, 0))
                info.create_system = 3
                mode = 0o755 if p.name in {"cxf.sh", "cxf.command"} else 0o644
                info.external_attr = (0o100000 | mode) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                archive.writestr(info, p.read_bytes())
        print(f"Exported {args.export.name}; sha256={digest(args.export)}")
    total = sum(p.stat().st_size for p in files)
    print(f"PASS: {len(files) + 1} public files; {total} payload bytes; content/link/hash checks; "
          f"Git candidate scope={'checked' if has_git else 'not applicable (clean export)'}")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as exc:
        raise SystemExit(str(exc))
