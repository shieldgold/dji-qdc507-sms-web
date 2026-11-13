#!/usr/bin/env python3
"""Static inspection of the captured QDC507 kernel; never contacts a device."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

IMAGE_SHA256 = "62daa067512597e59b952f45be8cd5b6d34fc75265d0f60c5a3b8b8f09ecea2a"
BASE = 0xc0008000
INIT_ENTRY = 0xc05dfc8c
DRIVER_RANGE = (0xc038f560, 0xc038ff04)


def find_u32_references(data, value):
    needle = struct.pack("<I", value)
    return [match.start() for match in re.finditer(re.escape(needle), data)
            if match.start() % 4 == 0]


def file_operation_callbacks(image, offset):
    return {name: struct.unpack_from("<I", image, offset + field)[0]
            for name, field in [("unlocked_ioctl", 40), ("open", 52), ("release", 60)]}


def walk_cfg(image, base, roots, ranges, exports):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    from capstone.arm import (ARM_INS_B, ARM_INS_BL, ARM_INS_BLX, ARM_INS_BX,
                              ARM_INS_POP, ARM_INS_MOV, ARM_OP_IMM, ARM_OP_REG,
                              ARM_OP_MEM, ARM_REG_PC, ARM_REG_LR, ARM_CC_AL)
    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    decoder.detail = True
    pending = list(roots)
    visited = set()
    calls, literals, unresolved, lines = [], [], [], []

    def admitted(address):
        return any(start <= address < end for start, end in ranges)

    def target(source, address, kind):
        address &= 0xffffffff
        if address in exports:
            calls.append({"site": hex(source), "target": hex(address),
                          "name": exports[address], "kind": kind})
        elif admitted(address):
            pending.append(address)
            if kind == "call":
                calls.append({"site": hex(source), "target": hex(address),
                              "name": None, "kind": kind})
        else:
            unresolved.append({"site": hex(source), "target": hex(address), "reason": "unknown direct target"})

    while pending:
        address = pending.pop()
        if address in visited:
            continue
        if not admitted(address) or not 0 <= address - base <= len(image) - 4 or address % 4:
            unresolved.append({"site": hex(address), "reason": "instruction outside admitted code ranges"})
            continue
        visited.add(address)
        instruction = next(decoder.disasm(image[address - base:address - base + 4], address), None)
        if instruction is None or instruction.size != 4:
            unresolved.append({"site": hex(address), "reason": "instruction decode failed"})
            continue
        lines.append((address, "%08x: %s %s" % (address, instruction.mnemonic, instruction.op_str)))
        for operand in instruction.operands:
            if operand.type == ARM_OP_MEM and operand.mem.base == ARM_REG_PC:
                slot = address + 8 + operand.mem.disp
                if base <= slot <= base + len(image) - 4:
                    value = struct.unpack_from("<I", image, slot - base)[0]
                    literals.append({"site": hex(address), "slot": hex(slot), "value": hex(value)})
        if instruction.id in {ARM_INS_BL, ARM_INS_BLX}:
            if instruction.operands[0].type == ARM_OP_IMM:
                target(address, instruction.operands[0].imm, "call")
            else:
                unresolved.append({"site": hex(address), "reason": "indirect call"})
            pending.append(address + 4)
            continue
        if instruction.id == ARM_INS_B:
            target(address, instruction.operands[0].imm, "branch_or_tail_call")
            if instruction.cc != ARM_CC_AL:
                pending.append(address + 4)
            continue
        returned = (instruction.id == ARM_INS_BX and instruction.operands[0].type == ARM_OP_REG
                    and instruction.operands[0].reg == ARM_REG_LR)
        returned |= (instruction.id == ARM_INS_POP and any(
            operand.type == ARM_OP_REG and operand.reg == ARM_REG_PC for operand in instruction.operands))
        returned |= (instruction.id == ARM_INS_MOV and len(instruction.operands) == 2
                     and instruction.operands[0].type == ARM_OP_REG and instruction.operands[0].reg == ARM_REG_PC
                     and instruction.operands[1].type == ARM_OP_REG and instruction.operands[1].reg == ARM_REG_LR)
        if returned:
            if instruction.cc != ARM_CC_AL:
                pending.append(address + 4)
            continue
        if ARM_REG_PC in instruction.regs_access()[1]:
            unresolved.append({"site": hex(address), "reason": "unresolved computed PC write"})
            continue
        pending.append(address + 4)
    return {"instruction_count": len(visited), "external_calls": sorted({row["name"] for row in calls if row["name"]}),
            "calls": sorted(calls, key=lambda row: row["site"]),
            "literals": sorted(literals, key=lambda row: row["site"]),
            "unresolved": unresolved, "disassembly": [line for _, line in sorted(lines)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("local", type=Path)
    parser.add_argument("--deps", type=Path, help="task-local Python dependency directory")
    args = parser.parse_args()
    if args.deps:
        sys.path.insert(0, str(args.deps))
    from elftools.elf.elffile import ELFFile
    root = args.local
    image = (root / "capture/kernel.Image").read_bytes()
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("This analysis is pinned to the captured QDC507 kernel, not arbitrary firmware")
    exports = json.loads((root / "inspection/exports.json").read_text())
    exports = {int(value["address"], 16): name for name, value in exports.items()}
    name_offset = image.index(b"quec_rawdata\0")
    name_address = BASE + name_offset
    references = find_u32_references(image, name_address)
    if references != [0xc05dfcbc - BASE]:
        raise ValueError("unexpected registration string references")
    fops = struct.unpack_from("<I", image, references[0] - 4)[0]
    callbacks = file_operation_callbacks(image, fops - BASE)
    expected = {"unlocked_ioctl": 0xc038fb6c, "open": 0xc038f794, "release": 0xc038f9d8}
    if callbacks != expected:
        raise ValueError("unexpected rawdata operation table")
    with (root / "qrawdata-analysis/fs-layout-probe.o").open("rb") as handle:
        elf = ELFFile(handle)
        if elf.elfclass != 32 or elf["e_machine"] != "EM_ARM":
            raise ValueError("layout probe must use the target ARM compiler")
        probe = list(struct.unpack("<4I", elf.get_section_by_name(".rodata").data()))
    if probe != [116, 40, 52, 60]:
        raise ValueError("candidate file_operations layout differs")
    crc_table = struct.unpack_from("<I", image, 0xc038fefc - BASE)[0]
    if crc_table - fops != probe[0]:
        raise ValueError("factory operation table extent differs from the compiler probe")
    partition_pointer = struct.unpack_from("<I", image, 0xc038f78c - BASE)[0]
    partition_offset = partition_pointer - BASE
    partition = image[partition_offset:image.index(b"\0", partition_offset)].decode("ascii")
    ioctl_base = struct.unpack_from("<I", image, 0xc038fcf8 - BASE)[0]
    if partition != "rawdata" or ioctl_base != 0x50001:
        raise ValueError("unexpected rawdata interface constants")
    operation_values = struct.unpack_from("<29I", image, fops - BASE)
    if [index * 4 for index, value in enumerate(operation_values) if value] != [40, 52, 60]:
        raise ValueError("additional operation callbacks require analysis")
    roots = [INIT_ENTRY] + list(callbacks.values())
    cfg = walk_cfg(image, BASE, roots, [DRIVER_RANGE, (INIT_ENTRY, INIT_ENTRY + 44)], exports)
    output = root / "qrawdata-analysis"
    output.mkdir(exist_ok=True)
    (output / "reachable-disassembly.txt").write_text("\n".join(cfg.pop("disassembly")) + "\n")
    network = [name for name in cfg["external_calls"] if re.search(
        r"(?:^|_)(?:skb|sock|netdev|netif|xfrm|nf|tun|netlink)(?:_|$)", name)]
    result = {
        "image_sha256": IMAGE_SHA256, "mapping_base": hex(BASE),
        "analysis_kind": "GNU objdump plus Capstone static instruction analysis, not a successful REA decompilation",
        "device_accessed": False, "runtime_verified": False,
        "proc_name": "quec_rawdata", "proc_path": "/proc/quec_rawdata",
        "registration": {"entry": hex(INIT_ENTRY), "callsite": "0xc05dfca8", "api": "proc_create_data",
                         "mode": 0, "parent": None, "data": None, "file_operations": hex(fops)},
        "callbacks": {name: hex(value) for name, value in callbacks.items()},
        "candidate_file_operations_layout": {"bytes": probe[0], "unlocked_ioctl": probe[1], "open": probe[2], "release": probe[3]},
        "factory_nonzero_operation_offsets": [40, 52, 60],
        "factory_next_crc_table_address": hex(crc_table),
        "factory_file_operations_extent": crc_table - fops,
        "ioctl_commands": {"get_record": hex(ioctl_base), "set_record": hex(ioctl_base + 1)},
        "ioctl_buffer": {"bytes": 136, "index_offset": 0, "length_offset": 4, "payload_offset": 8, "payload_capacity": 128},
        "record_lengths": list(struct.unpack_from("<4I", image, fops - BASE - 16)),
        "mtd_partition_name": partition, "cache_state_address": "0xc06896b8", "mutex_address": "0xc062ac40",
        "network_named_api_calls": network, "full_system_abi_proven": False,
        "control_flow": cfg,
        "limitations": ["Private routine names and C field names are analysis labels, not recovered original source symbols.",
                        "Documented exported kernel APIs are call-graph boundaries; their internals are not re-proven here.",
                        "This excludes observed direct rawdata/TUN coupling, not every possible unpublished firmware difference."]}
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"callbacks": result["callbacks"], "external_calls": cfg["external_calls"],
                      "instruction_count": cfg["instruction_count"], "unresolved": cfg["unresolved"],
                      "network_named_api_calls": network}, indent=2))
    if cfg["unresolved"] or network:
        raise SystemExit(1)


if __name__ == "__main__":
    main()
