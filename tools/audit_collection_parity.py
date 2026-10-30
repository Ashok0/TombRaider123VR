"""Record the source/config comparison underlying the human parity audit.

File differences are evidence for review, not automatically missing features.
Usage: python tools/audit_collection_parity.py [--reference PATH] [--output PATH]
"""
import argparse
import hashlib
import json
import re
from datetime import datetime, timezone
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def config_fields(path):
    text = re.sub(r"//[^\n]*", "", path.read_text(encoding="utf-8-sig"))
    return dict(re.findall(r"\b(?:bool|int|float)\s+(\w+)\s*=\s*([^;]+);", text))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference", type=Path, default=Path(r"C:\dev\TombRaider456VR"))
    parser.add_argument("--output", type=Path, default=Path("build/collection-parity/source-audit.json"))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    local = {str(p.relative_to(root / "src")): p for p in (root / "src").rglob("*") if p.is_file()}
    ref = {str(p.relative_to(args.reference / "src")): p for p in (args.reference / "src").rglob("*") if p.is_file()}
    assert local and ref, "Both source directories must exist"
    files = []
    for name in sorted(local.keys() | ref.keys()):
        a, b = local.get(name), ref.get(name)
        same = a and b and a.read_bytes().replace(b"\r\n", b"\n") == b.read_bytes().replace(b"\r\n", b"\n")
        files.append({"file": name, "status": "identical" if same else "different" if a and b else "local_only" if a else "reference_only",
                      "local_sha256": digest(a) if a else None, "reference_sha256": digest(b) if b else None})
    a, b = config_fields(root / "src/Config.h"), config_fields(args.reference / "src/Config.h")
    report = {
        "created_utc": datetime.now(timezone.utc).isoformat(),
        "local": str(root), "reference": str(args.reference.resolve()),
        "scope": "All files under both src trees; scalar Config.h defaults. Differences require semantic review.",
        "counts": {kind: sum(f["status"] == kind for f in files) for kind in ("identical", "different", "local_only", "reference_only")},
        "config_reference_only": {k: b[k] for k in sorted(b.keys() - a.keys())},
        "config_local_only": {k: a[k] for k in sorted(a.keys() - b.keys())},
        "config_default_differences": {k: {"local": a[k], "reference": b[k]} for k in sorted(a.keys() & b.keys()) if a[k] != b[k]},
        "files": files,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"output": str(args.output), "counts": report["counts"]}))


if __name__ == "__main__":
    main()
