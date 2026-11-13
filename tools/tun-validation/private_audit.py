#!/usr/bin/env python3
"""Run a local-only private-config differential audit in the build container."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

from inspect_tun import PRIVATE_SOURCE_GAPS, config_values


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def macro_header(values):
    lines = ["/* Differential experiment only; not recovered vendor code. */"]
    for key, value in sorted(values.items()):
        if value not in {"y", "m", "n"} or not re.fullmatch(r"CONFIG_[A-Z0-9_]+", key):
            raise ValueError("invalid private config value")
        lines.extend(["#undef " + key, "#undef " + key + "_MODULE"])
        if value == "y":
            lines.append("#define " + key + " 1")
        elif value == "m":
            lines.append("#define " + key + "_MODULE 1")
    return "\n".join(lines) + "\n"


def compiler_arguments(command, output):
    args = [arg for arg in shlex.split(command) if not arg.startswith("-Wp,-MD,")]
    if "-c" not in args or "-o" not in args or not args[0].endswith("arm-linux-gnueabi-gcc"):
        raise ValueError("unexpected compiler command")
    args[args.index("-o") + 1] = str(output)
    return args


def sdk_references(sdk):
    references = {key: [] for key in sorted(PRIVATE_SOURCE_GAPS)}
    expression = re.compile(r"\b(?:CONFIG_)?(" + "|".join(
        re.escape(key.removeprefix("CONFIG_")) for key in sorted(PRIVATE_SOURCE_GAPS)) + r")(?:_MODULE)?\b")
    for path in sorted(sdk.rglob("*")):
        if not path.is_file() or not (path.suffix in {".c", ".h", ".S"}
                                      or path.name in {"Makefile", "Kbuild"} or path.name.startswith("Kconfig")):
            continue
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            for match in expression.finditer(line):
                references["CONFIG_" + match.group(1)].append(
                    {"path": str(path.relative_to(sdk)), "line": number, "text": line.strip()})
    return references


def dependency_paths(command_file, build):
    text = command_file.read_text()
    block = text.split("deps_drivers/net/tun.o :=", 1)[1].split("\n\n", 1)[0]
    paths = set()
    for line in block.splitlines():
        value = line.strip().rstrip("\\").strip()
        if value and not value.startswith("$("):
            path = Path(value)
            if not path.is_absolute():
                path = build / path
            if not path.is_file():
                raise ValueError("missing dependency: " + str(path))
            paths.add(path)
    return sorted(paths)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--local", type=Path, default=Path("/work"))
    args = parser.parse_args()
    root = args.local
    output = root / "private-audit"
    output.mkdir(exist_ok=True)
    source = root / "sources/quectel-kernel"
    sdk = root / "sources/quectel-ec20-private"
    build = root / "build"
    values = config_values(gzip.decompress((root / "capture/config.gz").read_bytes()).decode())
    if not PRIVATE_SOURCE_GAPS <= values.keys():
        raise ValueError("target capture lacks an expected private key")
    private = {key: values[key] for key in sorted(PRIVATE_SOURCE_GAPS)}
    command_file = build / "drivers/net/.tun.o.cmd"
    command = command_file.read_text().splitlines()[0].split(":=", 1)[1].strip()
    if not command.startswith("/opt/gcc493/bin/"):
        raise ValueError("GCC 4.9.3 must produce the baseline first")
    variants = [("baseline", {})] + [(key, {key: value}) for key, value in private.items()] + [("all_private", private)]
    results = []
    for name, restored in variants:
        folder = output / name
        folder.mkdir(exist_ok=True)
        header = folder / "restored.h"
        header.write_text(macro_header(restored))
        extra = ["-include", str(header)] if restored else []
        obj = folder / "tun.o"
        argv = compiler_arguments(command, obj)
        argv[1:1] = extra
        subprocess.run(argv, cwd=build, check=True, capture_output=True)
        preprocessed = folder / "tun.i"
        argv = compiler_arguments(command, preprocessed)
        argv[argv.index("-c")] = "-E"
        argv[1:1] = ["-P"] + extra
        subprocess.run(argv, cwd=build, check=True, capture_output=True)
        row = {"name": name, "values": restored, "object_sha256": digest(obj),
               "preprocessed_sha256": digest(preprocessed)}
        results.append(row)
        print(name, row["object_sha256"], flush=True)
    baseline = results[0]
    for row in results:
        row["object_identical_to_baseline"] = row["object_sha256"] == baseline["object_sha256"]
        row["preprocessed_identical_to_baseline"] = row["preprocessed_sha256"] == baseline["preprocessed_sha256"]
    references = sdk_references(sdk)
    dependencies = dependency_paths(command_file, build)
    dependency_private_refs = []
    pattern = re.compile(r"\b(" + "|".join(sorted(PRIVATE_SOURCE_GAPS)) + r")(?:_MODULE)?\b")
    for path in dependencies:
        for number, line in enumerate(path.read_text(errors="replace").splitlines(), 1):
            if pattern.search(line):
                dependency_private_refs.append({"path": str(path), "line": number})
    sdk_network_refs = [ref for refs in references.values() for ref in refs
                        if ref["path"].startswith(("net/", "include/"))]
    passed = all(row["object_identical_to_baseline"] and row["preprocessed_identical_to_baseline"]
                 for row in results)
    result = {
        "scope": "The compiled public 3.18.44 candidate and a separate EC20 SDK used only as source evidence",
        "sdk_commit": "dcd4977901517beb21973c520727b34fcc44a15a", "sdk_kernel_version": "3.18.20",
        "private_config_values": private, "variants": results,
        "all_private_restorations_identical": passed,
        "baseline_matches_kbuild_object": baseline["object_sha256"] == digest(build / "drivers/net/tun.o"),
        "tun_dependency_file_count": len(dependencies), "tun_dependency_private_references": dependency_private_refs,
        "sdk_references": references, "sdk_network_or_include_references": sdk_network_refs,
        "source_unresolved_keys": [key for key, refs in sorted(references.items()) if not refs],
        "target_vendor_patch_identity_verified": False, "runtime_verified": False,
        "limitations": ["A macro differential cannot reconstruct private code absent from the candidate source.",
                        "The separate EC20 SDK is not established as the exact QDC507 firmware source."]}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ["all_private_restorations_identical",
          "baseline_matches_kbuild_object", "tun_dependency_file_count", "source_unresolved_keys"]}, indent=2))
    if not passed or not result["baseline_matches_kbuild_object"]:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
