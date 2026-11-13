"""Bounded ARM argument-origin analysis, not an emulator or ABI proof.

Calls are boundaries and clobber AAPCS caller-saved registers. Only identical
expressions survive joins. Stack spills, arbitrary arithmetic and indirect
callees are not inferred. Unmatched observations therefore remain unknown.
"""

from collections import deque
import struct


def walk_accesses(code, base, start, end, relocations=()):
    from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM
    from capstone.arm import (ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3,
                              ARM_REG_IP, ARM_REG_LR, ARM_REG_PC, ARM_OP_REG,
                              ARM_OP_IMM, ARM_OP_MEM, ARM_INS_BL, ARM_INS_BLX,
                              ARM_INS_B, ARM_INS_BX, ARM_INS_POP, ARM_INS_LDM,
                              ARM_INS_MOV, ARM_INS_MOVW, ARM_INS_ADD, ARM_INS_SUB,
                              ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH,
                              ARM_INS_LDRSH, ARM_INS_LDRSB, ARM_CC_AL, ARM_CC_LS,
                              ARM_SFT_LSL, ARM_INS_CMP)

    decoder = Cs(CS_ARCH_ARM, CS_MODE_ARM)
    decoder.detail = True
    arguments = [ARM_REG_R0, ARM_REG_R1, ARM_REG_R2, ARM_REG_R3]
    states = {start: {register: ("arg" + str(index), 0) for index, register in enumerate(arguments)}}
    pending = deque([start])
    sites, unresolved, switches = {}, set(), set()

    def instruction(address):
        return next(decoder.disasm(code[address - base:address - base + 4], address), None)

    def put(address, state):
        address &= 0xffffffff  # Capstone exposes some ARM immediates as signed.
        if not start <= address < end:
            return
        if address not in states:
            states[address] = state.copy()
            pending.append(address)
        else:
            merged = {register: value for register, value in states[address].items()
                      if state.get(register) == value}
            if merged != states[address]:
                states[address] = merged
                pending.append(address)

    while pending:
        address = pending.popleft()
        state = states[address].copy()
        ins = instruction(address)
        if ins is None:
            unresolved.add((address, "decode failed"))
            continue
        operands = ins.operands
        if ins.id in {ARM_INS_BL, ARM_INS_BLX}:
            for register in arguments + [ARM_REG_IP, ARM_REG_LR]:
                state.pop(register, None)
            put(address + 4, state)
            continue
        if ins.id == ARM_INS_B:
            if address not in relocations:
                put(operands[0].imm, state)
            if ins.cc != ARM_CC_AL:
                put(address + 4, state)
            continue
        returning = ins.id == ARM_INS_BX and operands[0].reg == ARM_REG_LR
        returning |= ins.id in {ARM_INS_POP, ARM_INS_LDM} and any(
            op.type == ARM_OP_REG and op.reg == ARM_REG_PC for op in operands)
        returning |= ins.id == ARM_INS_MOV and len(operands) == 2 and operands[0].reg == ARM_REG_PC and operands[1].type == ARM_OP_REG and operands[1].reg == ARM_REG_LR
        if returning:
            if ins.cc != ARM_CC_AL:
                put(address + 4, state)
            continue
        if ins.cc != ARM_CC_AL:
            put(address + 4, state)

        # The one admitted switch form: cmp rN,#N; ldrls pc,[pc,rN,lsl#2].
        # Every table target must remain inside this function bound.
        if ins.id == ARM_INS_LDR and operands[0].reg == ARM_REG_PC and ins.cc == ARM_CC_LS:
            memory = operands[1]
            previous = instruction(address - 4)
            if (memory.type == ARM_OP_MEM and memory.mem.base == ARM_REG_PC and
                    memory.shift.type == ARM_SFT_LSL and memory.shift.value == 2 and
                    previous and previous.id == ARM_INS_CMP and previous.operands[0].reg == memory.mem.index and
                    previous.operands[1].type == ARM_OP_IMM and 0 <= previous.operands[1].imm <= 32):
                count = previous.operands[1].imm + 1
                table = address + 8 + memory.mem.disp
                if 0 <= table - base <= len(code) - count * 4:
                    targets = struct.unpack_from("<%dI" % count, code, table - base)
                    if all(start <= target < end and target % 4 == 0 for target in targets):
                        switches.add(address)
                        for target in targets:
                            put(target, state)
                        continue

        writes = {register: None for register in ins.regs_access()[1]}
        if ins.id in {ARM_INS_MOV, ARM_INS_MOVW} and operands[0].type == ARM_OP_REG:
            if operands[1].type == ARM_OP_REG and not operands[1].shift.type:
                writes[operands[0].reg] = state.get(operands[1].reg)
            elif operands[1].type == ARM_OP_IMM:
                writes[operands[0].reg] = ("const", operands[1].imm)
        if ins.id in {ARM_INS_ADD, ARM_INS_SUB} and len(operands) == 3 and operands[1].type == ARM_OP_REG:
            origin = state.get(operands[1].reg)
            term = operands[2]
            number = term.imm if term.type == ARM_OP_IMM else None
            if term.type == ARM_OP_REG and state.get(term.reg, (None,))[0] == "const":
                number = state[term.reg][1]
                if term.shift.type == ARM_SFT_LSL:
                    number <<= term.shift.value
                elif term.shift.type:
                    number = None
            if origin and number is not None:
                writes[operands[0].reg] = (origin[0], origin[1] + number * (1 if ins.id == ARM_INS_ADD else -1))
        for operand in operands:
            if operand.type != ARM_OP_MEM:
                continue
            displacement = operand.mem.disp
            if operand.mem.index:
                index = state.get(operand.mem.index)
                if not index or index[0] != "const" or operand.shift.type not in {0, ARM_SFT_LSL}:
                    continue
                scaled = index[1] << operand.shift.value
                displacement += -scaled if operand.subtracted else scaled
            origin = state.get(operand.mem.base)
            if not origin or origin[0] == "const" or origin[0].count("[") >= 3:
                continue
            offset = origin[1] + displacement
            key = (origin[0], offset, decoder.insn_name(ins.id))
            sites.setdefault(key, set()).add(address)
            if ins.id in {ARM_INS_LDR, ARM_INS_LDRB, ARM_INS_LDRH, ARM_INS_LDRSH, ARM_INS_LDRSB}:
                writes[operands[0].reg] = (origin[0] + "[" + str(offset) + "]", 0)
        for register, value in writes.items():
            if value is None:
                state.pop(register, None)
            else:
                state[register] = value
        if ARM_REG_PC in writes:
            unresolved.add((address, "computed PC write"))
            continue
        put(address + 4, state)
    return {key: sorted(values) for key, values in sites.items()}, sorted(unresolved), sorted(switches)
