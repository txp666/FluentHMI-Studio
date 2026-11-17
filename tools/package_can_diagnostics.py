#!/usr/bin/env python3
"""Build the Windows diagnostic ZIP around the standard verified portable package."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import zipfile
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    args = parser.parse_args()
    if sys.platform != "win32":
        parser.error("this package is built and verified on Windows")
    if not re.fullmatch(r"[0-9a-f]{40}", args.commit):
        parser.error("--commit must be the full source commit SHA")
    build = args.build_dir.resolve()
    output = args.output_dir.resolve()
    cache = (build / "CMakeCache.txt").read_text(encoding="utf-8")
    if not re.search(r"^FHMI_CAN_DIAGNOSTICS:BOOL=ON$", cache, re.MULTILINE):
        parser.error("configure with -DFHMI_CAN_DIAGNOSTICS=ON before packaging")
    version = (build / "FluentHMIStudio-version.txt").read_text().strip() + "-can-diagnostics"
    subprocess.run([sys.executable, str(ROOT / "scripts/package.py"), "--build-dir", str(build),
                    "--output-dir", str(output), "--config", "Release", "--version", version], check=True)
    name = f"FluentHMI-Studio-v{version}-windows-x64"
    archive = output / f"{name}.zip"
    metadata = {"version": version, "source_commit": args.commit,
                "built_at_utc": datetime.now(timezone.utc).isoformat(),
                "status_queries_default": False, "trace_format": "JSONL",
                "status_override_environment": "FHMI_CAN_STATUS_QUERIES",
                "trace_directory_environment": "FHMI_CAN_TRACE_DIR"}
    with zipfile.ZipFile(archive, "a", compression=zipfile.ZIP_DEFLATED) as bundle:
        for source in sorted((ROOT / "diagnostics/can").iterdir()):
            if source.suffix.lower() not in (".cmd", ".txt"):
                continue
            # cmd.exe expects CRLF. The README remains UTF-8.
            text = source.read_text(encoding="utf-8")
            payload = text.replace("\r\n", "\n").replace("\n", "\r\n").encode("utf-8")
            bundle.writestr(f"{name}/{source.name}", payload)
        bundle.writestr(f"{name}/diagnostic-build.json", json.dumps(metadata, indent=2) + "\n")
    digest = hashlib.sha256()
    with archive.open("rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    archive.with_name(archive.name + ".sha256").write_text(
        f"{digest.hexdigest()}  {archive.name}\n", encoding="utf-8", newline="\n")
    print(f"Diagnostic package: {archive}")


if __name__ == "__main__":
    main()
