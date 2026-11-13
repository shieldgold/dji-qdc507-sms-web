#!/usr/bin/env python3
"""Offline checks only. This program never opens or changes a device."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import re
import struct
import zlib


class Elf32:
    def __init__(self, data):
        self.data = data
        if len(data) < 52 or data[:7] != b"\x7fELF\x01\x01\x01":
            raise ValueError("expected a little-endian ELF32 object")
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", data)
        if header[1:4] != (1, 40, 1):
            raise ValueError("expected a relocatable ARM module")
        self.flags = header[7]
        offset, size, count, names_index = header[6], header[11], header[12], header[13]
        if size != 40 or not count or offset + count * size > len(data):
            raise ValueError("invalid or truncated ELF section table")
        raw = [struct.unpack_from("<10I", data, offset + i * size) for i in range(count)]
        if names_index >= count:
            raise ValueError("invalid section name table")

        def payload(section):
            start, length = section[4:6]
            if section[1] == 8:  # SHT_NOBITS occupies no file bytes
                return b""
            if start + length > len(data):
                raise ValueError("truncated section")
            return data[start:start + length]

        names = payload(raw[names_index])
        self.sections = {}
        self.imports = set()
        self.weak_imports = set()
        for section in raw:
            name = self.cstring(names, section[0])
            content = payload(section)
            self.sections[name] = {"size": section[5], "data": content}
            if section[1] == 2:  # SHT_SYMTAB
                if section[6] >= count or section[9] != 16 or len(content) % 16:
                    raise ValueError("invalid symbol table")
                strings = payload(raw[section[6]])
                for pos in range(0, len(content), 16):
                    symbol = struct.unpack_from("<IIIBBH", content, pos)
                    if symbol[5] == 0 and symbol[0]:
                        name = self.cstring(strings, symbol[0])
                        (self.weak_imports if symbol[3] >> 4 == 2 else self.imports).add(name)
        self.info = {}
        for entry in self.sections.get(".modinfo", {}).get("data", b"").split(b"\0"):
            if b"=" in entry:
                key, value = entry.decode("ascii").split("=", 1)
                self.info.setdefault(key, []).append(value)
        if "vermagic" not in self.info or ".gnu.linkonce.this_module" not in self.sections:
            raise ValueError("module metadata is incomplete")

    @staticmethod
    def cstring(data, offset):
        if offset >= len(data):
            raise ValueError("string offset out of bounds")
        end = data.find(b"\0", offset)
        if end < 0:
            raise ValueError("unterminated ELF string")
        return data[offset:end].decode("ascii")


def config_values(text):
    values = {}
    for line in text.splitlines():
        if line.startswith("CONFIG_") and "=" in line:
            key, value = line.split("=", 1)
            values[key] = value
        elif line.startswith("# CONFIG_") and line.endswith(" is not set"):
            values[line[2:-11]] = "n"
    return values


PRIVATE_SOURCE_GAPS = {
    "CONFIG_QL_LPM", "CONFIG_QRAWDATA", "CONFIG_QSMD", "CONFIG_QSMEM",
    "CONFIG_QSTART", "CONFIG_QTZONE", "CONFIG_QUECTEL_AUDIO_DRIVER",
    "CONFIG_QUECTEL_DRIVER", "CONFIG_QUECTEL_MODEM_BOOT_TIME",
    "CONFIG_QUECTEL_MODEM_RESTART_LEVEL", "CONFIG_QUECTEL_PCM16K_SUPPORT",
    "CONFIG_QUECTEL_POWER_DRIVER", "CONFIG_QUECTEL_POWER_MANAGER",
}


def config_differences(target, built):
    differences = [{"key": key, "target": target.get(key), "build": built.get(key)}
                   for key in sorted(target.keys() | built.keys())
                   if target.get(key) != built.get(key)]
    unexpected = [item for item in differences
                  if not (item["key"] == "CONFIG_TUN" and item["target"] == "n" and item["build"] == "m")
                  and not (item["key"] in PRIVATE_SOURCE_GAPS and item["build"] is None)]
    return differences, unexpected


def extract_kernel(boot):
    if len(boot) < 40 or boot[:8] != b"ANDROID!":
        raise ValueError("expected a legacy Android boot image")
    size = struct.unpack_from("<I", boot, 8)[0]
    page = struct.unpack_from("<I", boot, 36)[0]
    if page < 512 or page & (page - 1) or page + size > len(boot):
        raise ValueError("invalid boot kernel bounds")
    kernel = boot[page:page + size]
    if kernel[36:40] != b"\x18\x28\x6f\x01":
        raise ValueError("expected an ARM zImage")
    start = 0
    while True:
        position = kernel.find(b"\x1f\x8b\x08", start)
        if position < 0:
            raise ValueError("no gzip-compressed kernel found")
        try:
            image = zlib.decompress(kernel[position:], 31)
        except zlib.error:
            start = position + 1
            continue
        if b"Linux version 3.18.44 " not in image:
            raise ValueError("unexpected embedded kernel version")
        return image


def exported_symbols(image, base, minimum_run=512):
    """Find long runs of ARM32 absolute {value, name} export entries.

    Both possible four-byte alignments are examined. Short pointer arrays are
    excluded; this is not a substitute for matching vendor headers or load tests.
    """
    runs = []
    for alignment in (0, 4):
        run = []
        for offset in range(alignment, len(image) - 7, 8):
            value, name_pointer = struct.unpack_from("<II", image, offset)
            index = name_pointer - base
            valid = base <= value < base + 64 * 1024 * 1024 and 0 <= index < len(image)
            if valid:
                end = image.find(b"\0", index, min(len(image), index + 201))
                name = image[index:end] if end >= 0 else b""
                valid = bool(re.fullmatch(rb"[A-Za-z_][A-Za-z0-9_]*", name))
            if valid:
                run.append((offset, value, name.decode("ascii")))
            else:
                if len(run) >= minimum_run:
                    runs.append(run)
                run = []
        if len(run) >= minimum_run:
            runs.append(run)
    result = {}
    for run in runs:
        for offset, value, name in run:
            if name in result:
                raise ValueError("ambiguous export tables")
            result[name] = {"address": hex(value), "table_offset": hex(offset)}
    return result


def compiler_versions(identities):
    return sorted({match.group(1) for text in identities
                   if (match := re.search(r"\)\s+(\d+\.\d+\.\d+)\b", text))})


def compare_modules(candidate, vendor, exports):
    errors = []
    if candidate.flags != vendor.flags:
        errors.append("ARM ELF ABI flags mismatch")
    if candidate.info["vermagic"] != vendor.info["vermagic"]:
        errors.append("vermagic mismatch")
    module_section = ".gnu.linkonce.this_module"
    candidate_size = candidate.sections[module_section]["size"]
    vendor_size = vendor.sections[module_section]["size"]
    if candidate_size != vendor_size:
        errors.append("struct module size mismatch")
    missing = sorted(candidate.imports - set(exports))
    if missing:
        errors.append("required symbols are not exported by the captured kernel")
    compiler_section = ".comment"
    compiler = sorted(set(candidate.sections.get(compiler_section, {}).get("data", b"").split(b"\0")) - {b""})
    vendor_compiler = sorted(set(vendor.sections.get(compiler_section, {}).get("data", b"").split(b"\0")) - {b""})
    compiler = [value.decode("ascii") for value in compiler]
    vendor_compiler = [value.decode("ascii") for value in vendor_compiler]
    versions = compiler_versions(compiler)
    vendor_versions = compiler_versions(vendor_compiler)
    prerelease = any("prerelease" in text.lower() for text in compiler)
    return {"errors": errors, "vermagic": candidate.info["vermagic"][0],
            "arm_elf_flags": hex(candidate.flags),
            "struct_module_bytes": candidate_size, "vendor_struct_module_bytes": vendor_size,
            "required_imports": sorted(candidate.imports), "missing_exports": missing,
            "weak_imports": sorted(candidate.weak_imports),
            "candidate_compiler": compiler,
            "vendor_compiler": vendor_compiler,
            "candidate_compiler_versions": versions,
            "vendor_compiler_versions": vendor_versions,
            "candidate_compiler_is_prerelease": prerelease,
            "compiler_version_matches": bool(versions) and versions == vendor_versions and not prerelease,
            "compiler_identical": bool(compiler and vendor_compiler) and compiler == vendor_compiler,
            "runtime_verified": False}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--candidate", type=Path)
    parser.add_argument("--build-config", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    captured_config = gzip.decompress((args.capture / "config.gz").read_bytes())
    config = config_values(captured_config.decode())
    if config.get("CONFIG_MODVERSIONS", "n") != "n":
        raise ValueError("symbol CRC validation is required for CONFIG_MODVERSIONS")
    image = extract_kernel((args.capture / "boot-partition.bin").read_bytes())
    marker = image.find(b"IKCFG_ST")
    if marker < 0 or gzip.decompress(image[marker + 8:image.index(b"IKCFG_ED", marker)]) != captured_config:
        raise ValueError("boot image configuration differs from the running kernel")
    base = int(config["CONFIG_PAGE_OFFSET"], 0) + 0x8000
    exports = exported_symbols(image, base)
    if not {"misc_register", "alloc_netdev_mqs", "sock_alloc_send_pskb"} <= exports.keys():
        raise ValueError("kernel export extraction failed sanity checks")
    vendor = Elf32((args.capture / "vendor-test-iosched.ko").read_bytes())
    vendor_missing = sorted(vendor.imports - exports.keys())
    if vendor_missing:
        raise ValueError("export extraction does not resolve vendor imports: " + repr(vendor_missing))
    (args.output / "kernel.Image").write_bytes(image)
    (args.output / "exports.json").write_text(json.dumps(exports, indent=2, sort_keys=True) + "\n")
    # These are reconstructed names, not the original vendor Module.symvers.
    # Zero CRCs are appropriate only because target CONFIG_MODVERSIONS is off.
    (args.output / "Module.symvers").write_text("".join(
        "0x00000000\t" + name + "\tvmlinux\tEXPORT_SYMBOL\n" for name in sorted(exports)))
    result = {"kernel_config_matches_running_capture": True,
              "kernel_export_count": len(exports), "vendor_import_count": len(vendor.imports),
              "kernel_image_sha256": hashlib.sha256(image).hexdigest(),
              "runtime_verified": False}
    if args.candidate:
        candidate_data = args.candidate.read_bytes()
        result.update(compare_modules(Elf32(candidate_data), vendor, exports))
        result["candidate_sha256"] = hashlib.sha256(candidate_data).hexdigest()
        result["candidate_bytes"] = len(candidate_data)
    if args.build_config:
        built = config_values(args.build_config.read_text())
        differences, unexpected = config_differences(config, built)
        result["config_differences"] = differences
        result["unexpected_config_differences"] = unexpected
        if unexpected:
            result.setdefault("errors", []).append("unexpected kernel configuration differences")
    if args.candidate:
        result["static_checks_passed"] = not result["errors"]
        result["full_abi_compatibility_proven"] = False
        result["limitations"] = [
            "No module upload, load, TUN creation, packet transfer or stability test was performed.",
            "Matching vermagic, module size and export names do not prove every vendor structure layout.",
        ]
        if not result["compiler_version_matches"]:
            result["limitations"].append("The compiler differs from the original vendor compiler.")
        elif not result["compiler_identical"]:
            result["limitations"].append("GCC versions match, but the vendor compiler build and downstream patches are not identical or fully known.")
        if any(item["key"] in PRIVATE_SOURCE_GAPS for item in result.get("config_differences", [])):
            result["limitations"].append("The public source lacks 13 captured Quectel-private configuration options.")
    (args.output / "inspection.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))
    if result.get("errors"):
        raise SystemExit(1)


if __name__ == "__main__":
    main()
