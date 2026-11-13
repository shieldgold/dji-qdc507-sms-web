"""Candidate DWARF layouts; these are not factory debug information."""

from pathlib import Path


def read_layouts(path):
    from elftools.elf.elffile import ELFFile

    def unqualified(die):
        while die and die.tag in {"DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type"}:
            die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
        return die

    def name(die):
        value = die.attributes.get("DW_AT_name") if die else None
        return value.value.decode() if value else None

    def members(die, prefix="", offset=0, depth=0):
        result = {}
        if depth > 8:
            return result
        for child in die.iter_children():
            if child.tag != "DW_TAG_member":
                continue
            location = child.attributes.get("DW_AT_data_member_location")
            if location and not isinstance(location.value, int):
                raise ValueError("Unsupported DWARF member location expression")
            start = offset + (location.value if location else 0)
            member_name = name(child)
            member_type = unqualified(child.get_DIE_from_attribute("DW_AT_type"))
            key = prefix + member_name if member_name else prefix.rstrip(".")
            if member_name:
                field = {"offset": start, "type": name(member_type)}
                if member_type and "DW_AT_byte_size" in member_type.attributes:
                    field['byte_size'] = member_type.attributes['DW_AT_byte_size'].value
                if member_type and member_type.tag == "DW_TAG_pointer_type":
                    pointed = unqualified(member_type.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in member_type.attributes else None
                    field["pointer_to"] = name(pointed)
                if "DW_AT_bit_size" in child.attributes:
                    width = child.attributes["DW_AT_bit_size"].value
                    field["bit_size"] = width
                    if "DW_AT_data_bit_offset" in child.attributes:
                        bit = child.attributes["DW_AT_data_bit_offset"].value
                        field["offset"] = offset + bit // 8
                        field["lsb"] = bit % 8
                    else:
                        storage = child.attributes["DW_AT_byte_size"].value
                        field["lsb"] = storage * 8 - child.attributes["DW_AT_bit_offset"].value - width
                result[key] = field
            if member_type and member_type.tag in {"DW_TAG_structure_type", "DW_TAG_union_type"}:
                nested_prefix = key + "." if member_name else prefix
                result.update(members(member_type, nested_prefix, start, depth + 1))
        return result

    layouts = {}
    with Path(path).open("rb") as handle:
        elf = ELFFile(handle)
        if elf.elfclass != 32 or elf["e_machine"] != "EM_ARM" or not elf.little_endian:
            raise ValueError("Expected little-endian ARM32 candidate probe")
        for unit in elf.get_dwarf_info().iter_CUs():
            for die in unit.iter_DIEs():
                type_name = name(die)
                if die.tag not in {"DW_TAG_structure_type", "DW_TAG_union_type"} or not type_name or "DW_AT_byte_size" not in die.attributes:
                    continue
                layouts[type_name] = {"size": die.attributes["DW_AT_byte_size"].value, "fields": members(die)}
    return layouts


def assert_same_allocated_sections(candidate, debug):
    """Adding debug information must not change any allocated section bytes."""
    from elftools.elf.elffile import ELFFile

    def sections(path):
        with Path(path).open("rb") as handle:
            elf = ELFFile(handle)
            return {section.name: (section["sh_type"], section["sh_size"], section.data())
                    for section in elf.iter_sections() if section["sh_flags"] & 2}

    if sections(candidate) != sections(debug):
        raise ValueError("Debug probe changed allocated TUN sections")
