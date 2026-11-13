#!/usr/bin/env python3
"""Download pinned compiler inputs to the computer; never accesses a module."""

import argparse
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    packages = json.loads(Path(__file__).with_name("gcc493-inputs.json").read_text())
    for item in packages:
        path = args.output / item["filename"]
        if path.exists() and hashlib.sha256(path.read_bytes()).hexdigest() == item["sha256"]:
            continue
        temporary = path.with_suffix(path.suffix + ".part")
        digest = hashlib.sha256()
        with urllib.request.urlopen(item["url"], timeout=60) as response, temporary.open("wb") as output:
            while chunk := response.read(1024 * 1024):
                output.write(chunk)
                digest.update(chunk)
        if digest.hexdigest() != item["sha256"] or temporary.stat().st_size != item["bytes"]:
            temporary.unlink()
            raise ValueError("compiler input checksum mismatch: " + item["filename"])
        temporary.replace(path)
    (args.output / "SHA256SUMS").write_text("".join(
        item["sha256"] + "  " + item["filename"] + "\n" for item in packages))
    print("Verified", len(packages), "pinned packages")


if __name__ == "__main__":
    main()
