#!/usr/bin/env python3
"""Pinned, offline QDC507 TUN ABI evidence. Never contacts a device."""

import argparse
from bisect import bisect_right
import hashlib
import json
from pathlib import Path
import re
import struct
import sys

from qrawdata_inspect import BASE, IMAGE_SHA256

TUN_SHA256 = '2b44764dc5bee73faab5671bc5575cb08cb08c8cb68854fb8ef1a85b5ca8637d'


def prel31(word, address):
    return address + ((word & 0x7fffffff) ^ 0x40000000) - 0x40000000


def unwind_entries(image, base):
    """Find the longest ordered ARM exidx run; validate identity in main."""
    words = struct.unpack('<%dI' % (len(image) // 4), image[:len(image) // 4 * 4])
    best = []
    for parity in (0, 1):
        run = []
        for index in range(parity, len(words) - 1, 2):
            word, descriptor = words[index:index + 2]
            address = base + index * 4
            function = prel31(word, address)
            valid = (not word & 0x80000000 and base <= function < base + len(image) and function % 4 == 0 and
                     (descriptor == 1 or descriptor >> 24 in {0x80, 0x81, 0x82} or
                      (not descriptor & 0x80000000 and base <= prel31(descriptor, address + 4) < base + len(image))))
            if not valid or (run and function <= run[-1][1]):
                if len(run) > len(best):
                    best = run
                run = []
            if valid:
                run.append((address, function))
        if len(run) > len(best):
            best = run
    return best


def normalize_words(candidate, factory, relocations):
    """Mask only specified link-address bits, never field immediates."""
    left, right = bytearray(candidate), bytearray(factory)
    if len(left) != len(right) or len(left) % 4:
        raise ValueError('ARM function ranges must be equally sized and word-aligned')
    for relocation in relocations:
        offset, kind = relocation['offset'], relocation['type']
        if offset % 4 or not 0 <= offset <= len(left) - 4:
            raise ValueError('Relocation outside aligned function word')
        if kind in {1, 28, 29}:  # R_ARM_PC24, CALL, JUMP24
            for data in (left, right):
                word = struct.unpack_from('<I', data, offset)[0]
                struct.pack_into('<I', data, offset, word & 0xff000000)
        elif kind == 2:  # R_ARM_ABS32
            left[offset:offset + 4] = right[offset:offset + 4] = b'\0' * 4
        else:
            raise ValueError('Unsupported relocation type %d' % kind)
    return [offset for offset in range(0, len(left), 4) if left[offset:offset + 4] != right[offset:offset + 4]]


def branch_target(word, address):
    if word & 0x0e000000 != 0x0a000000 or word >> 28 == 15:
        raise ValueError('Expected direct ARM-state B/BL')
    return (address + 8 + (((word & 0xffffff) ^ 0x800000) - 0x800000) * 4) & 0xffffffff


def object_functions(path):
    from elftools.elf.elffile import ELFFile

    result = {}
    with Path(path).open('rb') as handle:
        elf = ELFFile(handle)
        if elf.elfclass != 32 or elf['e_machine'] != 'EM_ARM' or not elf.little_endian:
            raise ValueError('Expected ARM32 object')
        symbols = elf.get_section_by_name('.symtab')
        for symbol in symbols.iter_symbols():
            if symbol['st_info']['type'] != 'STT_FUNC' or not symbol['st_size']:
                continue
            section = elf.get_section(symbol['st_shndx'])
            start, size = symbol['st_value'], symbol['st_size']
            relocations = []
            for relocation_section in elf.iter_sections():
                if relocation_section['sh_type'] != 'SHT_REL' or elf.get_section(relocation_section['sh_info']).name != section.name:
                    continue
                for relocation in relocation_section.iter_relocations():
                    offset = relocation['r_offset'] - start
                    if 0 <= offset < size:
                        target = symbols.get_symbol(relocation['r_info_sym'])
                        relocations.append({'offset': offset, 'type': relocation['r_info_type'], 'symbol': target.name})
            result[symbol.name] = {'start': start, 'size': size, 'section': section.data(), 'relocations': relocations}
    return result


def project_fields(path, offset, roots, layouts):
    root = path.split('[', 1)[0]
    type_name = roots.get(root)
    # skb_shinfo is a cast of the end pointer in this ARM configuration.
    if path.startswith(root + '[156]') and type_name == 'sk_buff':
        type_name = 'skb_shared_info'
        suffix = path[len(root + '[156]'):]
    else:
        suffix = path[len(root):]
    for value in re.findall(r'\[(-?\d+)\]', suffix):
        candidates = {field.get('pointer_to') for field in layouts.get(type_name, {}).get('fields', {}).values()
                      if field['offset'] == int(value) and field.get('pointer_to') in layouts}
        if len(candidates) != 1:
            return None, []
        type_name = candidates.pop()
    names = [name for name, field in layouts.get(type_name, {}).get('fields', {}).items()
             if field['offset'] + field.get('lsb', 0) // 8 == offset and field.get('byte_size') != 0]
    return type_name, names


def manual_checks(image, exports, layouts):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    from capstone.arm import ARM_OP_MEM, ARM_OP_IMM

    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    decoder.detail = True
    checks = []

    def check(function, relative, expected, value, kind='memory displacement'):
        address = exports[function] + relative
        ins = next(decoder.disasm(image[address - BASE:address - BASE + 4], address))
        if kind == 'memory displacement':
            actual = [op.mem.disp for op in ins.operands if op.type == ARM_OP_MEM]
        else:
            actual = [op.imm for op in ins.operands if op.type == ARM_OP_IMM]
        if value not in actual:
            raise ValueError('Factory instruction differs for ' + expected)
        checks.append({'claim': expected, 'value': value, 'factory_site': hex(address),
                       'instruction': ins.mnemonic + ' ' + ins.op_str, 'kind': kind})

    device_size = layouts['net_device']['size']
    check('alloc_netdev_mqs', 0x6c, 'sizeof(net_device)', device_size, 'immediate')
    check('alloc_netdev_mqs', 0x68, 'aligned net_device private-data offset', (device_size + 31) & ~31, 'immediate')
    check('dev_get_stats', 0x48, 'net_device.stats', layouts['net_device']['fields']['stats']['offset'], 'immediate')
    check('sock_from_file', 0x0c, 'file.private_data', layouts['file']['fields']['private_data']['offset'])
    check('sock_create', 0x20, 'thread_info.task', layouts['thread_info']['fields']['task']['offset'])
    check('sock_create', 0x24, 'task_struct.nsproxy', layouts['task_struct']['fields']['nsproxy']['offset'])
    check('sock_create', 0x38, 'nsproxy.net_ns', layouts['nsproxy']['fields']['net_ns']['offset'])
    checksum = layouts['sk_buff']['fields']['ip_summed']
    if checksum['bit_size'] != 2:
        raise ValueError('Unexpected checksum bit width')
    check('skb_partial_csum_set', 0x64, 'sk_buff.ip_summed byte', checksum['offset'])
    check('skb_partial_csum_set', 0x6c, 'CHECKSUM_PARTIAL bit mask', ((1 << checksum['bit_size']) - 1) << checksum['lsb'], 'immediate')
    return checks


def inspect(local, targets):
    from abi_access import walk_accesses
    from abi_layouts import read_layouts, assert_same_allocated_sections

    image = (local / 'capture/kernel.Image').read_bytes()
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError('This inspection is pinned to the captured factory kernel')
    if hashlib.sha256((local / 'output/tun.ko').read_bytes()).hexdigest() != TUN_SHA256:
        raise ValueError('Candidate differs from the reviewed GCC 4.9.3 TUN artifact')
    assert_same_allocated_sections(local / 'build/drivers/net/tun.o', local / 'abi-analysis/tun-debug.o')
    layouts = read_layouts(local / 'abi-analysis/tun-debug.o')
    imports = set(json.loads((local / 'inspection/inspection.json').read_text())['required_imports'])
    exports = {name: int(value['address'], 16) for name, value in json.loads((local / 'inspection/exports.json').read_text()).items()}
    unwind = unwind_entries(image, BASE)
    if len(unwind) != 17720 or unwind[0][0] != 0xc05a0ef0:
        raise ValueError('Factory exidx recovery differs from the pinned image')
    boundaries = sorted(set([value for _, value in unwind] + list(exports.values())))
    objects = {name: object_functions(local / 'build' / name) for name in targets['objects']}
    chosen = []
    for object_name, functions in objects.items():
        names = (imports | set(targets['additional_exports'].get(object_name, []))) & functions.keys()
        for name in sorted(names):
            if name not in exports:
                raise ValueError('Missing factory export: ' + name)
            chosen.append((name, object_name, exports[name], 'export table'))
    factory_entries = dict(exports)
    for rule in targets['local_helpers']:
        functions = objects[rule['object']]
        caller = functions[rule['caller']]
        position = caller['start'] + rule['call_offset']
        candidate_word = struct.unpack_from('<I', caller['section'], position)[0]
        relocation = [rel for rel in caller['relocations'] if rel['offset'] == rule['call_offset']]
        if relocation:
            if len(relocation) != 1 or relocation[0]['type'] not in {1, 28, 29} or relocation[0]['symbol'] != rule['name']:
                raise ValueError('Unexpected local helper relocation')
            destination = functions[rule['name']]['start'] + branch_target(candidate_word, 0)
        else:
            destination = branch_target(candidate_word, position)
        if destination != functions[rule['name']]['start']:
            raise ValueError('Candidate helper attribution differs')
        address = factory_entries[rule['caller']] + rule['call_offset']
        factory_word = struct.unpack_from('<I', image, address - BASE)[0]
        entry = branch_target(factory_word, address)
        factory_entries[rule['name']] = entry
        chosen.append((rule['name'], rule['object'], entry, rule['caller'] + '+' + hex(rule['call_offset'])))

    rows, evidence, detail = [], {}, []
    for name, object_name, entry, attribution in chosen:
        function = objects[object_name][name]
        start, size, section, relocations = (function[key] for key in ('start', 'size', 'section', 'relocations'))
        candidate = section[start:start + size]
        factory = image[entry - BASE:entry - BASE + size]
        differences = normalize_words(candidate, factory, relocations)
        links = []
        # Relocation offsets correspond only when the whole normalized body matches.
        if not differences:
            for relocation in relocations:
                offset, kind, symbol = (relocation[key] for key in ('offset', 'type', 'symbol'))
                left = struct.unpack_from('<I', candidate, offset)[0]
                right = struct.unpack_from('<I', factory, offset)[0]
                value = branch_target(right, entry + offset) if kind != 2 else right
                expected = exports.get(symbol)
                if expected is not None and kind == 2:
                    expected = (expected + left) & 0xffffffff
                status = 'matches_export' if expected == value else 'not_independently_resolved' if expected is None else 'CONFLICT'
                links.append({'offset': offset, 'symbol': symbol, 'status': status, 'factory_value': hex(value)})
        candidate_accesses, candidate_unknown, candidate_switches = walk_accesses(
            section, 0, start, start + size, {start + rel['offset'] for rel in relocations})
        end = boundaries[bisect_right(boundaries, entry)]
        factory_accesses, factory_unknown, factory_switches = walk_accesses(image, BASE, entry, end)
        common = candidate_accesses.keys() & factory_accesses.keys()
        argument_types = targets['argument_types'].get(name, {})
        for path, offset, opcode in sorted(common):
            type_name, fields = project_fields(path, offset, argument_types, layouts)
            if not fields:
                continue
            key = (type_name, offset, opcode)
            observation = {'function': name, 'argument_path': path, 'candidate_site': hex(candidate_accesses[(path, offset, opcode)][0] - start),
                           'factory_site': hex(factory_accesses[(path, offset, opcode)][0])}
            evidence.setdefault(key, {'type': type_name, 'offset': offset, 'opcode': opcode, 'candidate_field_names': sorted(fields), 'observations': []})['observations'].append(observation)
        row = {'name': name, 'object': object_name, 'is_tun_import': name in imports,
               'factory_entry': hex(entry), 'attribution': attribution, 'candidate_bytes': size,
               'normalized_body_matches': not differences, 'different_word_count': len(differences),
               'link_address_checks': links, 'matching_argument_access_patterns': len(common),
               'candidate_only_access_patterns': sorted(candidate_accesses.keys() - factory_accesses.keys()),
               'factory_only_access_patterns': sorted(factory_accesses.keys() - candidate_accesses.keys()),
               'unresolved_candidate_control_flow': candidate_unknown, 'unresolved_factory_control_flow': factory_unknown,
               'candidate_switches_resolved': len(candidate_switches), 'factory_switches_resolved': len(factory_switches)}
        rows.append(row)
        detail.append({'function': name, 'candidate_different_word_offsets': differences,
                       'matching_accesses': [{'path': key[0], 'offset': key[1], 'opcode': key[2],
                                              'candidate_sites': [hex(value - start) for value in candidate_accesses[key]],
                                              'factory_sites': [hex(value) for value in factory_accesses[key]]} for key in sorted(common)]})
    selected_types = sorted({row['type'] for row in evidence.values()} | {'sk_buff', 'sock', 'socket', 'net_device', 'file', 'file_operations', 'skb_shared_info', 'proto', 'proto_ops', 'net_device_ops', 'rtnl_link_ops', 'ethtool_ops', 'task_struct', 'nsproxy', 'timer_list', 'miscdevice'})
    result = {'image_sha256': IMAGE_SHA256, 'candidate_sha256': TUN_SHA256,
              'device_accessed': False, 'runtime_verified': False, 'full_abi_compatibility_proven': False,
              'analysis_kind': 'ELF/DWARF and Capstone static analysis; not a successful REA decompilation',
              'debug_probe_allocated_sections_identical': True,
              'factory_exidx': {'entry_count': len(unwind), 'table_start': hex(unwind[0][0]), 'boundaries_may_merge_adjacent_functions': True},
              'function_count': len(rows), 'tun_import_function_count': sum(row['is_tun_import'] for row in rows),
              'normalized_body_match_count': sum(row['normalized_body_matches'] for row in rows),
              'matching_argument_access_pattern_count': sum(row['matching_argument_access_patterns'] for row in rows),
              'candidate_layouts': {name: layouts[name] for name in selected_types if name in layouts},
              'manual_factory_checks': manual_checks(image, exports, layouts),
              'field_offset_evidence': sorted(evidence.values(), key=lambda row: (row['type'], row['offset'], row['opcode'])),
              'functions': rows,
              'limitations': [
                  'Factory debug types and original source are unavailable; candidate type sizes are not factory size proofs unless manually corroborated.',
                  'A normalized body match masks relocation address bits; unresolved local targets/data addresses remain explicitly unverified.',
                  'Argument-origin analysis does not model stack spills, arbitrary arithmetic, indirect callees or all execution paths. Unmatched patterns are unknown, not automatically ABI conflicts.',
                  'Field names are inferred from corresponding source roles and candidate DWARF; a byte offset match does not establish all bit masks or complete structure layout.',
                  'Original exidx entries can merge adjacent functions; bounds stop at the next export/unwind entry and are not recovered source function sizes.',
                  'Differing normalized bodies have not all been proved semantically equivalent. No module load, TUN creation or packet test occurred.'
              ]}
    conflicts = [check for row in rows for check in row['link_address_checks'] if check['status'] == 'CONFLICT']
    if conflicts:
        raise ValueError('Export target conflict: ' + repr(conflicts))
    output = local / 'abi-analysis'
    (output / 'function-detail.json').write_text(json.dumps(detail, indent=2) + '\n')
    (output / 'candidate-layouts-full.json').write_text(json.dumps(layouts, indent=2) + '\n')
    (output / 'result.json').write_text(json.dumps(result, indent=2) + '\n')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('local', type=Path)
    parser.add_argument('--deps', type=Path)
    args = parser.parse_args()
    if args.deps:
        sys.path.insert(0, str(args.deps))
    targets = json.loads(Path(__file__).with_name('abi-targets.json').read_text())
    result = inspect(args.local, targets)
    print(json.dumps({key: result[key] for key in ['function_count', 'tun_import_function_count', 'normalized_body_match_count', 'matching_argument_access_pattern_count', 'device_accessed', 'runtime_verified', 'full_abi_compatibility_proven']}, indent=2))


if __name__ == '__main__':
    main()
