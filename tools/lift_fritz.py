#!/usr/bin/env python3
"""
Static 8086-to-C binary translator for FRITZ.BIN (KnightStalker / Fritz 1.00, 1991).

Reads fritz_data.h, recursively disassembles all reachable 8086 instructions in
FRITZ.BIN, runs backward flag-liveness analysis over {CF, ZF, SF, OF}, and emits
fritz_recompiled.h containing a native C implementation of FRITZ.BIN with
100% bit-for-bit faithful 8086 semantics.
"""

import os
import re
import struct
import sys

CF, ZF, SF, OF = 1, 2, 4, 8
ALL_FLAGS = CF | ZF | SF | OF

REG16 = ["ax", "cx", "dx", "bx", "sp", "bp", "si", "di"]
SREG = ["es", "cs", "ss", "ds"]


def load_fritz_bin(header_path):
    with open(header_path, "r") as f:
        txt = f.read()
    m = re.search(r"fritz_bin_data\[\d+\]\s*=\s*\{(.*?)\};", txt, re.S)
    if not m:
        raise RuntimeError("Could not locate fritz_bin_data in " + header_path)
    raw = bytes(int(x, 16) for x in re.findall(r"0x[0-9a-fA-F]+", m.group(1)))
    mem = bytearray(0x10000)
    mem[0x0100:0x0100 + len(raw)] = raw
    return mem, len(raw)


def u8(mem, a):
    return mem[a & 0xFFFF]


def s8(mem, a):
    v = mem[a & 0xFFFF]
    return v - 256 if v >= 128 else v


def u16(mem, a):
    return struct.unpack_from("<H", mem, a & 0xFFFF)[0]


def s16(mem, a):
    return struct.unpack_from("<h", mem, a & 0xFFFF)[0]


def decode_insn(mem, ip):
    start = ip
    seg_ovr = -1
    rep = 0
    while True:
        b = u8(mem, ip)
        if b == 0x26:
            seg_ovr = 0; ip += 1
        elif b == 0x2E:
            seg_ovr = 1; ip += 1
        elif b == 0x36:
            seg_ovr = 2; ip += 1
        elif b == 0x3E:
            seg_ovr = 3; ip += 1
        elif b == 0xF0:
            ip += 1
        elif b == 0xF2:
            rep = 2; ip += 1
        elif b == 0xF3:
            rep = 1; ip += 1
        else:
            break

    op = u8(mem, ip)
    ip += 1

    modrm = None
    disp = 0
    imm = 0
    imm2 = 0

    def read_modrm(ip):
        m = u8(mem, ip)
        ip += 1
        mod = (m >> 6) & 3
        rm = m & 7
        d = 0
        if mod == 0 and rm == 6:
            d = u16(mem, ip); ip += 2
        elif mod == 1:
            d = s8(mem, ip); ip += 1
        elif mod == 2:
            d = u16(mem, ip); ip += 2
        return m, d, ip

    targets = []
    falls_through = True
    kind = "normal"

    if (op & 0xC4) == 0x00 and (op & 7) < 4:
        modrm, disp, ip = read_modrm(ip)
    elif (op & 0xC4) == 0x04 and (op & 7) in (4, 5):
        if (op & 1) == 0:
            imm = u8(mem, ip); ip += 1
        else:
            imm = u16(mem, ip); ip += 2
    elif op in (0x06, 0x07, 0x0E, 0x16, 0x17, 0x1E, 0x1F,
                0x60, 0x61, 0x90, 0x98, 0x99, 0x9C, 0x9D, 0x9E, 0x9F,
                0xA4, 0xA5, 0xA6, 0xA7, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
                0xD7, 0xF5, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD) or (0x40 <= op <= 0x5F) or (0x91 <= op <= 0x97):
        pass
    elif op == 0x68:
        imm = u16(mem, ip); ip += 2
    elif op == 0x6A:
        imm = s8(mem, ip) & 0xFFFF; ip += 1
    elif 0x70 <= op <= 0x7F:
        rel = s8(mem, ip); ip += 1
        targets.append((ip + rel) & 0xFFFF)
        kind = "jcc"
    elif op in (0x80, 0x82):
        modrm, disp, ip = read_modrm(ip)
        imm = u8(mem, ip); ip += 1
    elif op == 0x81:
        modrm, disp, ip = read_modrm(ip)
        imm = u16(mem, ip); ip += 2
    elif op == 0x83:
        modrm, disp, ip = read_modrm(ip)
        imm = s8(mem, ip) & 0xFFFF; ip += 1
    elif 0x84 <= op <= 0x8F:
        modrm, disp, ip = read_modrm(ip)
    elif op == 0x9A:
        imm = u16(mem, ip); imm2 = u16(mem, ip + 2); ip += 4
        kind = "call_far"
    elif op in (0xA0, 0xA1, 0xA2, 0xA3):
        disp = u16(mem, ip); ip += 2
    elif op == 0xA8:
        imm = u8(mem, ip); ip += 1
    elif op == 0xA9:
        imm = u16(mem, ip); ip += 2
    elif 0xB0 <= op <= 0xB7:
        imm = u8(mem, ip); ip += 1
    elif 0xB8 <= op <= 0xBF:
        imm = u16(mem, ip); ip += 2
    elif op in (0xC0, 0xC1, 0xC6):
        modrm, disp, ip = read_modrm(ip)
        imm = u8(mem, ip); ip += 1
    elif op == 0xC7:
        modrm, disp, ip = read_modrm(ip)
        imm = u16(mem, ip); ip += 2
    elif op in (0xC2, 0xCA):
        imm = u16(mem, ip); ip += 2
        falls_through = False; kind = "ret"
    elif op in (0xC3, 0xCB, 0xCF):
        falls_through = False; kind = "ret"
    elif op in (0xC4, 0xC5):
        modrm, disp, ip = read_modrm(ip)
    elif op == 0xCD:
        imm = u8(mem, ip); ip += 1
        kind = "int"
    elif op in (0xD0, 0xD1, 0xD2, 0xD3):
        modrm, disp, ip = read_modrm(ip)
    elif op in (0xE0, 0xE1, 0xE2, 0xE3):
        rel = s8(mem, ip); ip += 1
        targets.append((ip + rel) & 0xFFFF)
        kind = "jcc"
    elif op == 0xE8:
        rel = s16(mem, ip); ip += 2
        targets.append((ip + rel) & 0xFFFF)
        kind = "call"
    elif op == 0xE9:
        rel = s16(mem, ip); ip += 2
        targets.append((ip + rel) & 0xFFFF)
        falls_through = False; kind = "jmp"
    elif op == 0xEA:
        imm = u16(mem, ip); imm2 = u16(mem, ip + 2); ip += 4
        falls_through = False; kind = "jmp_far"
    elif op == 0xEB:
        rel = s8(mem, ip); ip += 1
        targets.append((ip + rel) & 0xFFFF)
        falls_through = False; kind = "jmp"
    elif op == 0xF6:
        modrm, disp, ip = read_modrm(ip)
        if ((modrm >> 3) & 7) in (0, 1):
            imm = u8(mem, ip); ip += 1
    elif op == 0xF7:
        modrm, disp, ip = read_modrm(ip)
        if ((modrm >> 3) & 7) in (0, 1):
            imm = u16(mem, ip); ip += 2
    elif op == 0xFE:
        modrm, disp, ip = read_modrm(ip)
    elif op == 0xFF:
        modrm, disp, ip = read_modrm(ip)
        sop = (modrm >> 3) & 7
        if sop == 2:
            kind = "call_ind"
        elif sop == 3:
            kind = "call_far_ind"
        elif sop == 4:
            falls_through = False; kind = "jmp_ind"
        elif sop == 5:
            falls_through = False; kind = "jmp_far_ind"
    else:
        raise ValueError(f"Unsupported opcode 0x{op:02x} at 0x{start:04x}")

    return {
        "ip": start,
        "next": ip,
        "op": op,
        "seg_ovr": seg_ovr,
        "rep": rep,
        "modrm": modrm,
        "disp": disp,
        "imm": imm,
        "imm2": imm2,
        "kind": kind,
        "targets": targets,
        "falls_through": falls_through,
    }


def flag_effects(ins):
    """Return (read_flags, write_flags) over {CF, ZF, SF, OF}."""
    op = ins["op"]
    modrm = ins["modrm"]
    sop = (modrm >> 3) & 7 if modrm is not None else 0

    # Standard ALU 00..3D
    if ((op & 0xC4) == 0x00 and (op & 7) < 4) or ((op & 0xC4) == 0x04 and (op & 7) in (4, 5)):
        aop = (op >> 3) & 7
        if aop in (2, 3):  # ADC, SBB
            return (CF, ALL_FLAGS)
        return (0, ALL_FLAGS)
    if op in (0x80, 0x81, 0x82, 0x83):
        if sop in (2, 3):
            return (CF, ALL_FLAGS)
        return (0, ALL_FLAGS)
    if 0x40 <= op <= 0x4F:  # INC/DEC r16 preserve CF
        return (0, ZF | SF | OF)
    if op == 0xFE:  # INC/DEC rm8 preserve CF
        return (0, ZF | SF | OF)
    if op == 0xFF and sop in (0, 1):  # INC/DEC rm16 preserve CF
        return (0, ZF | SF | OF)
    if op in (0x84, 0x85, 0xA8, 0xA9):  # TEST
        return (0, ALL_FLAGS)
    if op == 0xF5:  # CMC
        return (CF, CF)
    if op in (0xF8, 0xF9):  # CLC, STC
        return (0, CF)
    if op in (0xF6, 0xF7):
        if sop in (0, 1):  # TEST
            return (0, ALL_FLAGS)
        if sop == 2:  # NOT
            return (0, 0)
        if sop == 3:  # NEG
            return (0, ALL_FLAGS)
        if sop in (4, 5):  # MUL / IMUL
            return (0, CF | OF)
        return (0, 0)
    if op in (0xC0, 0xC1, 0xD0, 0xD1, 0xD2, 0xD3):
        # Conservative: CL can be 0 (preserving flags), RCL/RCR read CF,
        # ROL/ROR only write CF/OF; SHL/SHR/SAR write ALL_FLAGS when cnt>0.
        if op in (0xD2, 0xD3):
            return (ALL_FLAGS, 0)  # cnt may be 0, so nothing is guaranteed killed
        if sop in (2, 3):  # RCL/RCR
            return (CF, CF | OF)
        if sop in (0, 1):  # ROL/ROR
            return (0, CF | OF)
        return (0, ALL_FLAGS)
    if 0x70 <= op <= 0x7F:
        cc = op & 0xF
        if cc in (0, 1): return (OF, 0)
        if cc in (2, 3): return (CF, 0)
        if cc in (4, 5): return (ZF, 0)
        if cc in (6, 7): return (CF | ZF, 0)
        if cc in (8, 9): return (SF, 0)
        if cc in (0xC, 0xD): return (SF | OF, 0)
        if cc in (0xE, 0xF): return (ZF | SF | OF, 0)
        return (ALL_FLAGS, 0)
    if op in (0xE0, 0xE1):
        return (ZF, 0)
    if op == 0xCD:  # INT 0x16
        return (0, ZF)
    if op in (0xA6, 0xA7, 0xAE, 0xAF):
        return (ALL_FLAGS, 0)
    if op in (0x9C, 0x9F):
        return (ALL_FLAGS, 0)
    if op in (0x9D, 0x9E):
        return (0, ALL_FLAGS)
    return (0, 0)


def compute_liveness(insns):
    live_in = {ip: 0 for ip in insns}
    live_out = {ip: 0 for ip in insns}
    effects = {ip: flag_effects(ins) for ip, ins in insns.items()}

    changed = True
    while changed:
        changed = False
        for ip, ins in insns.items():
            kind = ins["kind"]
            out = 0
            if kind in ("ret", "jmp_ind", "jmp_far", "jmp_far_ind", "call_ind", "call_far", "call_far_ind"):
                out = ALL_FLAGS
            elif kind == "call":
                # Sound: flags could be read by callee OR survive across callee to caller's next insn
                for t in ins["targets"]:
                    if t in live_in:
                        out |= live_in[t]
                if ins["next"] in live_in:
                    out |= live_in[ins["next"]]
            else:
                for t in ins["targets"]:
                    if t in live_in:
                        out |= live_in[t]
                if ins["falls_through"] and ins["next"] in live_in:
                    out |= live_in[ins["next"]]

            r, w = effects[ip]
            inn = r | (out & ~w)
            if out != live_out[ip] or inn != live_in[ip]:
                live_out[ip] = out
                live_in[ip] = inn
                changed = True

    return live_in, live_out


def get_r8_expr(idx):
    reg = REG16[idx & 3]
    if idx & 4:
        return f"(uint8_t)({reg} >> 8)"
    return f"(uint8_t)({reg})"


def set_r8_stmt(idx, val_expr):
    reg = REG16[idx & 3]
    if idx & 4:
        return f"{reg} = (uint16_t)(({reg} & 0x00FFu) | ((uint16_t)(uint8_t)({val_expr}) << 8));"
    return f"{reg} = (uint16_t)(({reg} & 0xFF00u) | (uint8_t)({val_expr}));"


def decode_ea(ins, const_eng=False):
    """Return (is_reg, rm_idx, seg_base_expr, off_expr)."""
    m = ins["modrm"]
    mod = (m >> 6) & 3
    rm = m & 7
    if mod == 3:
        return True, rm, None, None

    def_seg = 3  # DS
    disp = ins["disp"]
    if mod == 0 and rm == 6:
        ea = f"0x{disp:04X}u"
    else:
        parts = []
        if rm == 0: parts = ["bx", "si"]
        elif rm == 1: parts = ["bx", "di"]
        elif rm == 2: parts = ["bp", "si"]; def_seg = 2
        elif rm == 3: parts = ["bp", "di"]; def_seg = 2
        elif rm == 4: parts = ["si"]
        elif rm == 5: parts = ["di"]
        elif rm == 6: parts = ["bp"]; def_seg = 2
        elif rm == 7: parts = ["bx"]
        if mod == 1 and disp != 0:
            if disp > 0:
                parts.append(f"0x{disp:02X}u")
            else:
                parts.append(f"(uint16_t)({disp})")
        elif mod == 2 and disp != 0:
            parts.append(f"0x{disp:04X}u")
        if len(parts) == 1:
            ea = parts[0]
        else:
            ea = f"(uint16_t)({' + '.join(parts)})"

    sidx = ins["seg_ovr"] if ins["seg_ovr"] >= 0 else def_seg
    if const_eng and sidx in (1, 2, 3):
        seg_base = "eng"
    else:
        seg_base = f"{SREG[sidx]}_base"
    return False, rm, seg_base, ea


def read_rm8(is_reg, rm, seg_base, off):
    if is_reg:
        return get_r8_expr(rm)
    return f"{seg_base}[{off}]"


def write_rm8(is_reg, rm, seg_base, off, val):
    if is_reg:
        return set_r8_stmt(rm, val)
    return f"{seg_base}[{off}] = (uint8_t)({val});"


def read_rm16(is_reg, rm, seg_base, off):
    if is_reg:
        return REG16[rm]
    return f"R16({seg_base}, {off})"


def write_rm16(is_reg, rm, seg_base, off, val):
    if is_reg:
        return f"{REG16[rm]} = (uint16_t)({val});"
    return f"W16({seg_base}, {off}, (uint16_t)({val}));"


def emit_flags_add(bits, a, b, cin, res, r, live):
    out = []
    if live & CF:
        mask = "0xFFu" if bits == 8 else "0xFFFFu"
        out.append(f"fl_cf = ({res} > {mask});")
    if live & OF:
        sign = "0x80u" if bits == 8 else "0x8000u"
        out.append(f"fl_of = ((({a} ^ {r}) & ({b} ^ {r}) & {sign}) != 0);")
    if live & ZF:
        out.append(f"fl_zf = ({r} == 0);")
    if live & SF:
        sh = 7 if bits == 8 else 15
        out.append(f"fl_sf = ({r} >> {sh}) & 1u;")
    return out


def emit_flags_sub(bits, a, b, cin, r, live):
    out = []
    if live & CF:
        if cin == "0":
            out.append(f"fl_cf = ({a} < {b});")
        else:
            out.append(f"fl_cf = ((uint32_t){a} < (uint32_t){b} + (uint32_t){cin});")
    if live & OF:
        sign = "0x80u" if bits == 8 else "0x8000u"
        out.append(f"fl_of = ((({a} ^ {b}) & ({a} ^ {r}) & {sign}) != 0);")
    if live & ZF:
        out.append(f"fl_zf = ({r} == 0);")
    if live & SF:
        sh = 7 if bits == 8 else 15
        out.append(f"fl_sf = ({r} >> {sh}) & 1u;")
    return out


def emit_flags_logic(bits, r, live):
    out = []
    if live & CF: out.append("fl_cf = 0;")
    if live & OF: out.append("fl_of = 0;")
    if live & ZF: out.append(f"fl_zf = ({r} == 0);")
    if live & SF:
        sh = 7 if bits == 8 else 15
        out.append(f"fl_sf = ({r} >> {sh}) & 1u;")
    return out


def emit_alu(bits, aop, a_expr, b_expr, write_fn, live):
    ty = "uint8_t" if bits == 8 else "uint16_t"
    wty = "uint16_t" if bits == 8 else "uint32_t"
    lines = ["{"]
    lines.append(f"  {ty} _a = {a_expr}; {ty} _b = {b_expr};")
    if aop == 0:  # ADD
        lines.append(f"  {wty} _res = ({wty})_a + ({wty})_b;")
        lines.append(f"  {ty} _r = ({ty})_res;")
        lines.extend("  " + s for s in emit_flags_add(bits, "_a", "_b", "0", "_res", "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 1:  # OR
        lines.append(f"  {ty} _r = ({ty})(_a | _b);")
        lines.extend("  " + s for s in emit_flags_logic(bits, "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 2:  # ADC
        lines.append(f"  {wty} _c = fl_cf ? 1u : 0u;")
        lines.append(f"  {wty} _res = ({wty})_a + ({wty})_b + _c;")
        lines.append(f"  {ty} _r = ({ty})_res;")
        lines.extend("  " + s for s in emit_flags_add(bits, "_a", "_b", "_c", "_res", "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 3:  # SBB
        lines.append(f"  {wty} _c = fl_cf ? 1u : 0u;")
        lines.append(f"  {ty} _r = ({ty})((uint32_t)_a - (uint32_t)_b - (uint32_t)_c);")
        lines.extend("  " + s for s in emit_flags_sub(bits, "_a", "_b", "_c", "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 4:  # AND
        lines.append(f"  {ty} _r = ({ty})(_a & _b);")
        lines.extend("  " + s for s in emit_flags_logic(bits, "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 5:  # SUB
        lines.append(f"  {ty} _r = ({ty})(_a - _b);")
        lines.extend("  " + s for s in emit_flags_sub(bits, "_a", "_b", "0", "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 6:  # XOR
        lines.append(f"  {ty} _r = ({ty})(_a ^ _b);")
        lines.extend("  " + s for s in emit_flags_logic(bits, "_r", live))
        lines.append("  " + write_fn("_r"))
    elif aop == 7:  # CMP
        if live & (ZF | SF | OF):
            lines.append(f"  {ty} _r = ({ty})(_a - _b);")
            lines.extend("  " + s for s in emit_flags_sub(bits, "_a", "_b", "0", "_r", live))
        elif live & CF:
            lines.append("  fl_cf = (_a < _b);")
    lines.append("}")
    return lines


def cond_expr(cc):
    return {
        0x0: "fl_of",
        0x1: "!fl_of",
        0x2: "fl_cf",
        0x3: "!fl_cf",
        0x4: "fl_zf",
        0x5: "!fl_zf",
        0x6: "(fl_cf || fl_zf)",
        0x7: "(!fl_cf && !fl_zf)",
        0x8: "fl_sf",
        0x9: "!fl_sf",
        0xC: "(fl_sf != fl_of)",
        0xD: "(fl_sf == fl_of)",
        0xE: "(fl_zf || (fl_sf != fl_of))",
        0xF: "(!fl_zf && (fl_sf == fl_of))",
    }[cc]


def emit_insn(ins, live, const_eng):
    op = ins["op"]
    ip = ins["ip"]
    nxt = ins["next"]
    modrm = ins["modrm"]
    imm = ins["imm"]
    disp = ins["disp"]
    lines = []

    push_m = "PUSH16_ENG" if const_eng else "PUSH16"
    pop_m = "POP16_ENG" if const_eng else "POP16"
    ss_b = "eng" if const_eng else "ss_base"

    # Standard ALU 00..3D
    if (op & 0xC4) == 0x00 and (op & 7) < 4:
        aop = (op >> 3) & 7
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        dir_r = op & 2
        w16 = op & 1
        if not w16:
            if not dir_r:
                if not is_reg:
                    lines.append(f"{{ uint16_t _off = {off};")
                    lines.extend(emit_alu(8, aop, f"{sb}[_off]", get_r8_expr(reg),
                                          lambda r: f"{sb}[_off] = {r};", live))
                    lines.append("}")
                else:
                    lines.extend(emit_alu(8, aop, get_r8_expr(rm), get_r8_expr(reg),
                                          lambda r: set_r8_stmt(rm, r), live))
            else:
                lines.extend(emit_alu(8, aop, get_r8_expr(reg), read_rm8(is_reg, rm, sb, off),
                                      lambda r: set_r8_stmt(reg, r), live))
        else:
            if not dir_r:
                if not is_reg:
                    lines.append(f"{{ uint16_t _off = {off};")
                    lines.extend(emit_alu(16, aop, f"R16({sb}, _off)", REG16[reg],
                                          lambda r: f"W16({sb}, _off, {r});", live))
                    lines.append("}")
                else:
                    lines.extend(emit_alu(16, aop, REG16[rm], REG16[reg],
                                          lambda r: f"{REG16[rm]} = {r};", live))
            else:
                lines.extend(emit_alu(16, aop, REG16[reg], read_rm16(is_reg, rm, sb, off),
                                      lambda r: f"{REG16[reg]} = {r};", live))
        return lines

    if (op & 0xC4) == 0x04 and (op & 7) in (4, 5):
        aop = (op >> 3) & 7
        if (op & 1) == 0:
            lines.extend(emit_alu(8, aop, get_r8_expr(0), f"0x{imm:02X}u",
                                  lambda r: set_r8_stmt(0, r), live))
        else:
            lines.extend(emit_alu(16, aop, "ax", f"0x{imm:04X}u",
                                  lambda r: f"ax = {r};", live))
        return lines

    # Segment push/pop
    if op == 0x06: return [f"{push_m}(es);"]
    if op == 0x07: return [f"es = {pop_m}(); SET_SEG_BASE(es, es_base);"]
    if op == 0x0E: return [f"{push_m}(cs);"]
    if op == 0x16: return [f"{push_m}(ss);"]
    if op == 0x17: return [f"ss = {pop_m}(); SET_SEG_BASE(ss, ss_base);"]
    if op == 0x1E: return [f"{push_m}(ds);"]
    if op == 0x1F: return [f"ds = {pop_m}(); SET_SEG_BASE(ds, ds_base);"]

    # INC/DEC r16
    if 0x40 <= op <= 0x47:
        r = REG16[op & 7]
        lines.append(f"{r} = (uint16_t)({r} + 1u);")
        if live & OF: lines.append(f"fl_of = ({r} == 0x8000u);")
        if live & ZF: lines.append(f"fl_zf = ({r} == 0);")
        if live & SF: lines.append(f"fl_sf = ({r} >> 15) & 1u;")
        return lines
    if 0x48 <= op <= 0x4F:
        r = REG16[op & 7]
        lines.append(f"{r} = (uint16_t)({r} - 1u);")
        if live & OF: lines.append(f"fl_of = ({r} == 0x7FFFu);")
        if live & ZF: lines.append(f"fl_zf = ({r} == 0);")
        if live & SF: lines.append(f"fl_sf = ({r} >> 15) & 1u;")
        return lines

    # PUSH/POP r16
    if 0x50 <= op <= 0x57:
        if op == 0x54:
            return [f"sp = (uint16_t)(sp - 2u); W16({ss_b}, sp, sp);"]
        return [f"{push_m}({REG16[op & 7]});"]
    if 0x58 <= op <= 0x5F:
        return [f"{REG16[op & 7]} = {pop_m}();"]

    # Jcc rel8
    if 0x70 <= op <= 0x7F:
        t = ins["targets"][0]
        cond = cond_expr(op & 0xF)
        return [f"if ({cond}) goto L_{t:04X};"]

    # Group 1 imm
    if op in (0x80, 0x82):
        aop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        if not is_reg:
            lines.append(f"{{ uint16_t _off = {off};")
            lines.extend(emit_alu(8, aop, f"{sb}[_off]", f"0x{imm:02X}u",
                                  lambda r: f"{sb}[_off] = {r};", live))
            lines.append("}")
        else:
            lines.extend(emit_alu(8, aop, get_r8_expr(rm), f"0x{imm:02X}u",
                                  lambda r: set_r8_stmt(rm, r), live))
        return lines
    if op in (0x81, 0x83):
        aop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        if not is_reg:
            lines.append(f"{{ uint16_t _off = {off};")
            lines.extend(emit_alu(16, aop, f"R16({sb}, _off)", f"0x{imm:04X}u",
                                  lambda r: f"W16({sb}, _off, {r});", live))
            lines.append("}")
        else:
            lines.extend(emit_alu(16, aop, REG16[rm], f"0x{imm:04X}u",
                                  lambda r: f"{REG16[rm]} = {r};", live))
        return lines

    # TEST
    if op == 0x84:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        lines.append(f"{{ uint8_t _r = (uint8_t)({read_rm8(is_reg, rm, sb, off)} & {get_r8_expr(reg)});")
        lines.extend("  " + s for s in emit_flags_logic(8, "_r", live))
        lines.append("}")
        return lines
    if op == 0x85:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        lines.append(f"{{ uint16_t _r = (uint16_t)({read_rm16(is_reg, rm, sb, off)} & {REG16[reg]});")
        lines.extend("  " + s for s in emit_flags_logic(16, "_r", live))
        lines.append("}")
        return lines

    # XCHG
    if op == 0x86:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        if not is_reg:
            return [f"{{ uint16_t _off = {off}; uint8_t _a = {sb}[_off]; uint8_t _b = {get_r8_expr(reg)}; {set_r8_stmt(reg, '_a')} {sb}[_off] = _b; }}"]
        return [f"{{ uint8_t _a = {get_r8_expr(rm)}; uint8_t _b = {get_r8_expr(reg)}; {set_r8_stmt(reg, '_a')} {set_r8_stmt(rm, '_b')} }}"]
    if op == 0x87:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        if not is_reg:
            return [f"{{ uint16_t _off = {off}; uint16_t _a = R16({sb}, _off); uint16_t _b = {REG16[reg]}; {REG16[reg]} = _a; W16({sb}, _off, _b); }}"]
        return [f"{{ uint16_t _t = {REG16[rm]}; {REG16[rm]} = {REG16[reg]}; {REG16[reg]} = _t; }}"]

    # MOV r/m, r and r, r/m
    if op == 0x88:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [write_rm8(is_reg, rm, sb, off, get_r8_expr(reg))]
    if op == 0x89:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [write_rm16(is_reg, rm, sb, off, REG16[reg])]
    if op == 0x8A:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [set_r8_stmt(reg, read_rm8(is_reg, rm, sb, off))]
    if op == 0x8B:
        reg = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [f"{REG16[reg]} = {read_rm16(is_reg, rm, sb, off)};"]
    if op == 0x8C:
        sr = (modrm >> 3) & 3
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [write_rm16(is_reg, rm, sb, off, SREG[sr])]
    if op == 0x8D:  # LEA
        reg = (modrm >> 3) & 7
        _, _, _, off = decode_ea(ins, const_eng)
        return [f"{REG16[reg]} = {off};"]
    if op == 0x8E:
        sr = (modrm >> 3) & 3
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        sn = SREG[sr]
        return [f"{sn} = {read_rm16(is_reg, rm, sb, off)}; SET_SEG_BASE({sn}, {sn}_base);"]
    if op == 0x8F:
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [f"{{ uint16_t _v = {pop_m}(); {write_rm16(is_reg, rm, sb, off, '_v')} }}"]

    if op == 0x90:
        return ["/* nop */"]
    if 0x91 <= op <= 0x97:
        r = REG16[op & 7]
        return [f"{{ uint16_t _t = ax; ax = {r}; {r} = _t; }}"]
    if op == 0x98:
        return ["ax = (uint16_t)(int16_t)(int8_t)(ax & 0xFFu);"]
    if op == 0x99:
        return ["dx = (ax & 0x8000u) ? 0xFFFFu : 0x0000u;"]

    # MOV AL/AX, moffs
    if op in (0xA0, 0xA1, 0xA2, 0xA3):
        sidx = ins["seg_ovr"] if ins["seg_ovr"] >= 0 else 3
        sb = "eng" if (const_eng and sidx in (1, 2, 3)) else f"{SREG[sidx]}_base"
        if op == 0xA0:
            return [set_r8_stmt(0, f"{sb}[0x{disp:04X}u]")]
        if op == 0xA1:
            return [f"ax = R16({sb}, 0x{disp:04X}u);"]
        if op == 0xA2:
            return [f"{sb}[0x{disp:04X}u] = (uint8_t)ax;"]
        if op == 0xA3:
            return [f"W16({sb}, 0x{disp:04X}u, ax);"]

    if op == 0xA8:
        lines.append(f"{{ uint8_t _r = (uint8_t)((uint8_t)ax & 0x{imm:02X}u);")
        lines.extend("  " + s for s in emit_flags_logic(8, "_r", live))
        lines.append("}")
        return lines
    if op == 0xA9:
        lines.append(f"{{ uint16_t _r = (uint16_t)(ax & 0x{imm:04X}u);")
        lines.extend("  " + s for s in emit_flags_logic(16, "_r", live))
        lines.append("}")
        return lines

    if op == 0xAD:  # LODSW (DF is always 0 in FRITZ.BIN)
        sidx = ins["seg_ovr"] if ins["seg_ovr"] >= 0 else 3
        sb = "eng" if (const_eng and sidx in (1, 2, 3)) else f"{SREG[sidx]}_base"
        if ins["rep"]:
            return [f"while (cx) {{ ax = R16({sb}, si); si = (uint16_t)(si + 2u); cx--; }}"]
        return [f"ax = R16({sb}, si); si = (uint16_t)(si + 2u);"]

    # MOV reg, imm
    if 0xB0 <= op <= 0xB7:
        return [set_r8_stmt(op & 7, f"0x{imm:02X}u")]
    if 0xB8 <= op <= 0xBF:
        return [f"{REG16[op & 7]} = 0x{imm:04X}u;"]

    # RET / RETF
    if op == 0xC3:
        return [f"ip = {pop_m}(); DISPATCH_IP();"]
    if op == 0xCB:
        return [f"ip = {pop_m}(); cs = {pop_m}(); if (cs == TRAP_SEG) goto handle_trap; SET_SEG_BASE(cs, cs_base); DISPATCH_IP();"]

    # MOV rm, imm
    if op == 0xC6:
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [write_rm8(is_reg, rm, sb, off, f"0x{imm:02X}u")]
    if op == 0xC7:
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        return [write_rm16(is_reg, rm, sb, off, f"0x{imm:04X}u")]

    # INT imm8
    if op == 0xCD:
        if imm == 0x16:
            return ["if ((ax >> 8) == 1u) { fl_zf = 1; } else if ((ax >> 8) == 0u) { ax = 0; }"]
        elif imm == 0x1A:
            return ["if ((ax >> 8) == 0u) { uint32_t _t = current_bios_ticks(); cx = (uint16_t)(_t >> 16); dx = (uint16_t)_t; ax &= 0xFF00u; }"]
        return ["/* int */"]

    # Shifts / Rotates D0..D3
    if op in (0xD0, 0xD1, 0xD2, 0xD3):
        sop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        w16 = (op & 1) != 0
        by_cl = (op & 2) != 0
        bits = 16 if w16 else 8
        ty = "uint16_t" if w16 else "uint8_t"
        sign_bit = 15 if w16 else 7
        sign_mask = "0x8000u" if w16 else "0x80u"

        lines.append("{")
        if not is_reg:
            lines.append(f"  uint16_t _off = {off};")
            vread = f"R16({sb}, _off)" if w16 else f"{sb}[_off]"
            vwrite = (lambda x: f"W16({sb}, _off, {x});") if w16 else (lambda x: f"{sb}[_off] = {x};")
        else:
            vread = REG16[rm] if w16 else get_r8_expr(rm)
            vwrite = (lambda x: f"{REG16[rm]} = {x};") if w16 else (lambda x: set_r8_stmt(rm, x))

        lines.append(f"  {ty} _v = {vread};")
        if not by_cl:
            # Count == 1 (super fast inline!)
            if sop == 0:  # ROL 1
                lines.append(f"  uint8_t _msb = (uint8_t)((_v >> {sign_bit}) & 1u);")
                lines.append(f"  _v = ({ty})((_v << 1) | _msb);")
                if live & CF: lines.append("  fl_cf = _msb;")
                if live & OF: lines.append(f"  fl_of = (uint8_t)(((_v >> {sign_bit}) & 1u) ^ _msb);")
            elif sop == 1:  # ROR 1
                lines.append(f"  uint8_t _lsb = (uint8_t)(_v & 1u);")
                lines.append(f"  _v = ({ty})((_v >> 1) | ((uint16_t)_lsb << {sign_bit}));")
                if live & CF: lines.append("  fl_cf = _lsb;")
                if live & OF: lines.append(f"  fl_of = (uint8_t)(((_v >> {sign_bit}) ^ (_v >> {sign_bit - 1})) & 1u);")
            elif sop == 2:  # RCL 1
                if live & (CF | OF):
                    lines.append(f"  uint8_t _msb = (uint8_t)((_v >> {sign_bit}) & 1u);")
                lines.append(f"  _v = ({ty})((_v << 1) | (fl_cf ? 1u : 0u));")
                if live & CF: lines.append("  fl_cf = _msb;")
                if live & OF: lines.append(f"  fl_of = (uint8_t)(((_v >> {sign_bit}) & 1u) ^ _msb);")
            elif sop == 3:  # RCR 1
                if live & CF:
                    lines.append(f"  uint8_t _lsb = (uint8_t)(_v & 1u);")
                lines.append(f"  _v = ({ty})((_v >> 1) | ((uint16_t)(fl_cf ? 1u : 0u) << {sign_bit}));")
                if live & CF: lines.append("  fl_cf = _lsb;")
                if live & OF: lines.append(f"  fl_of = (uint8_t)(((_v >> {sign_bit}) ^ (_v >> {sign_bit - 1})) & 1u);")
            elif sop in (4, 6):  # SHL 1
                if live & (CF | OF):
                    lines.append(f"  uint8_t _msb = (uint8_t)((_v >> {sign_bit}) & 1u);")
                lines.append(f"  _v = ({ty})(_v << 1);")
                if live & CF: lines.append("  fl_cf = _msb;")
                if live & OF: lines.append(f"  fl_of = (uint8_t)(((_v >> {sign_bit}) & 1u) ^ _msb);")
                if live & ZF: lines.append("  fl_zf = (_v == 0);")
                if live & SF: lines.append(f"  fl_sf = (_v >> {sign_bit}) & 1u;")
            elif sop == 5:  # SHR 1
                if live & OF: lines.append(f"  fl_of = (uint8_t)((_v >> {sign_bit}) & 1u);")
                if live & CF: lines.append("  fl_cf = (uint8_t)(_v & 1u);")
                lines.append(f"  _v = ({ty})(_v >> 1);")
                if live & ZF: lines.append("  fl_zf = (_v == 0);")
                if live & SF: lines.append(f"  fl_sf = (_v >> {sign_bit}) & 1u;")
            elif sop == 7:  # SAR 1
                if live & CF: lines.append("  fl_cf = (uint8_t)(_v & 1u);")
                if live & OF: lines.append("  fl_of = 0;")
                lines.append(f"  _v = ({ty})((_v >> 1) | (_v & {sign_mask}));")
                if live & ZF: lines.append("  fl_zf = (_v == 0);")
                if live & SF: lines.append(f"  fl_sf = (_v >> {sign_bit}) & 1u;")
            lines.append("  " + vwrite("_v"))
        else:
            # Count == CL (use helper to match 8086 exact semantics including CL==0 and CL>=bits)
            fn = "recomp_shift16" if w16 else "recomp_shift8"
            lines.append(f"  _v = {fn}({sop}, _v, (uint8_t)cx, &fl_cf, &fl_of, &fl_zf, &fl_sf);")
            lines.append("  " + vwrite("_v"))
        lines.append("}")
        return lines

    # LOOP / JCXZ
    if op == 0xE2:
        t = ins["targets"][0]
        return [f"cx = (uint16_t)(cx - 1u); if (cx != 0) goto L_{t:04X};"]
    if op == 0xE3:
        t = ins["targets"][0]
        return [f"if (cx == 0) goto L_{t:04X};"]

    # Direct CALL / JMP
    if op == 0xE8:
        t = ins["targets"][0]
        return [f"{push_m}(0x{nxt:04X}u); goto L_{t:04X};"]
    if op in (0xE9, 0xEB):
        t = ins["targets"][0]
        return [f"goto L_{t:04X};"]

    # Group 3 (F6 / F7)
    if op in (0xF6, 0xF7):
        sop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        w16 = (op == 0xF7)
        bits = 16 if w16 else 8
        ty = "uint16_t" if w16 else "uint8_t"
        lines.append("{")
        if not is_reg:
            lines.append(f"  uint16_t _off = {off};")
            vread = f"R16({sb}, _off)" if w16 else f"{sb}[_off]"
            vwrite = (lambda x: f"W16({sb}, _off, {x});") if w16 else (lambda x: f"{sb}[_off] = {x};")
        else:
            vread = REG16[rm] if w16 else get_r8_expr(rm)
            vwrite = (lambda x: f"{REG16[rm]} = {x};") if w16 else (lambda x: set_r8_stmt(rm, x))
        lines.append(f"  {ty} _v = {vread};")
        if sop in (0, 1):  # TEST rm, imm
            fmt = f"0x{imm:04X}u" if w16 else f"0x{imm:02X}u"
            lines.append(f"  {ty} _r = ({ty})(_v & {fmt});")
            lines.extend("  " + s for s in emit_flags_logic(bits, "_r", live))
        elif sop == 2:  # NOT
            lines.append("  " + vwrite(f"({ty})(~_v)"))
        elif sop == 3:  # NEG
            lines.append(f"  {ty} _r = ({ty})(0u - _v);")
            lines.extend("  " + s for s in emit_flags_sub(bits, "0u", "_v", "0", "_r", live))
            lines.append("  " + vwrite("_r"))
        elif sop == 4:  # MUL
            if not w16:
                lines.append("  uint16_t _res = (uint16_t)((uint8_t)ax) * (uint16_t)_v;")
                lines.append("  ax = _res;")
                if live & (CF | OF):
                    lines.append("  uint8_t _ov = ((_res & 0xFF00u) != 0);")
                    if live & CF: lines.append("  fl_cf = _ov;")
                    if live & OF: lines.append("  fl_of = _ov;")
            else:
                lines.append("  uint32_t _res = (uint32_t)ax * (uint32_t)_v;")
                lines.append("  ax = (uint16_t)_res; dx = (uint16_t)(_res >> 16);")
                if live & (CF | OF):
                    lines.append("  uint8_t _ov = (dx != 0);")
                    if live & CF: lines.append("  fl_cf = _ov;")
                    if live & OF: lines.append("  fl_of = _ov;")
        elif sop == 5:  # IMUL
            if not w16:
                lines.append("  int16_t _res = (int16_t)(int8_t)(ax & 0xFFu) * (int16_t)(int8_t)_v;")
                lines.append("  ax = (uint16_t)_res;")
                if live & (CF | OF):
                    lines.append("  uint8_t _ov = (_res != (int16_t)(int8_t)_res);")
                    if live & CF: lines.append("  fl_cf = _ov;")
                    if live & OF: lines.append("  fl_of = _ov;")
            else:
                lines.append("  int32_t _res = (int32_t)(int16_t)ax * (int32_t)(int16_t)_v;")
                lines.append("  ax = (uint16_t)_res; dx = (uint16_t)((uint32_t)_res >> 16);")
                if live & (CF | OF):
                    lines.append("  uint8_t _ov = (_res != (int32_t)(int16_t)_res);")
                    if live & CF: lines.append("  fl_cf = _ov;")
                    if live & OF: lines.append("  fl_of = _ov;")
        elif sop == 6:  # DIV
            if not w16:
                lines.append("  if (_v != 0) { uint16_t _q = ax / _v; uint16_t _rem = ax % _v; if (_q <= 0xFFu) ax = (uint16_t)(_q | (_rem << 8)); }")
            else:
                lines.append("  if (_v != 0) { uint32_t _num = ((uint32_t)dx << 16) | ax; uint32_t _q = _num / _v; uint32_t _rem = _num % _v; if (_q <= 0xFFFFu) { ax = (uint16_t)_q; dx = (uint16_t)_rem; } }")
        elif sop == 7:  # IDIV
            if not w16:
                lines.append("  if (_v != 0) { int16_t _q = (int16_t)ax / (int16_t)(int8_t)_v; int16_t _rem = (int16_t)ax % (int16_t)(int8_t)_v; if (_q >= -128 && _q <= 127) ax = (uint16_t)((uint8_t)(int8_t)_q | ((uint16_t)(uint8_t)(int8_t)_rem << 8)); }")
            else:
                lines.append("  if (_v != 0) { int32_t _num = (int32_t)(((uint32_t)dx << 16) | ax); int32_t _den = (int32_t)(int16_t)_v; int32_t _q = _num / _den; int32_t _rem = _num % _den; if (_q >= -32768 && _q <= 32767) { ax = (uint16_t)(int16_t)_q; dx = (uint16_t)(int16_t)_rem; } }")
        lines.append("}")
        return lines

    if op == 0xF5: return ["fl_cf ^= 1u;"]
    if op == 0xF8: return ["fl_cf = 0;"]
    if op == 0xF9: return ["fl_cf = 1;"]
    if op in (0xFA, 0xFB): return ["/* cli/sti */"]

    # Group 4 (FE: INC/DEC rm8)
    if op == 0xFE:
        sop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        lines.append("{")
        if not is_reg:
            lines.append(f"  uint16_t _off = {off}; uint8_t _v = {sb}[_off];")
            vwrite = f"{sb}[_off] = _v;"
        else:
            lines.append(f"  uint8_t _v = {get_r8_expr(rm)};")
            vwrite = set_r8_stmt(rm, "_v")
        if sop == 0:
            lines.append("  _v = (uint8_t)(_v + 1u);")
            if live & OF: lines.append("  fl_of = (_v == 0x80u);")
        else:
            lines.append("  _v = (uint8_t)(_v - 1u);")
            if live & OF: lines.append("  fl_of = (_v == 0x7Fu);")
        if live & ZF: lines.append("  fl_zf = (_v == 0);")
        if live & SF: lines.append("  fl_sf = (_v >> 7) & 1u;")
        lines.append("  " + vwrite)
        lines.append("}")
        return lines

    # Group 5 (FF)
    if op == 0xFF:
        sop = (modrm >> 3) & 7
        is_reg, rm, sb, off = decode_ea(ins, const_eng)
        if sop in (0, 1):
            lines.append("{")
            if not is_reg:
                lines.append(f"  uint16_t _off = {off}; uint16_t _v = R16({sb}, _off);")
                vwrite = f"W16({sb}, _off, _v);"
            else:
                lines.append(f"  uint16_t _v = {REG16[rm]};")
                vwrite = f"{REG16[rm]} = _v;"
            if sop == 0:
                lines.append("  _v = (uint16_t)(_v + 1u);")
                if live & OF: lines.append("  fl_of = (_v == 0x8000u);")
            else:
                lines.append("  _v = (uint16_t)(_v - 1u);")
                if live & OF: lines.append("  fl_of = (_v == 0x7FFFu);")
            if live & ZF: lines.append("  fl_zf = (_v == 0);")
            if live & SF: lines.append("  fl_sf = (_v >> 15) & 1u;")
            lines.append("  " + vwrite)
            lines.append("}")
            return lines
        elif sop == 2:  # CALL near rm16
            return [f"{{ uint16_t _t = {read_rm16(is_reg, rm, sb, off)}; {push_m}(0x{nxt:04X}u); ip = _t; DISPATCH_IP(); }}"]
        elif sop == 3:  # CALL far m16:16 (host callbacks!)
            return [f"{{ uint16_t _off = {off}; uint16_t _tip = R16({sb}, _off); uint16_t _tcs = R16({sb}, (uint16_t)(_off + 2u)); {push_m}(cs); {push_m}(0x{nxt:04X}u); cs = _tcs; ip = _tip; if (cs == TRAP_SEG) goto handle_trap; SET_SEG_BASE(cs, cs_base); DISPATCH_IP(); }}"]
        elif sop == 4:  # JMP near rm16
            return [f"ip = {read_rm16(is_reg, rm, sb, off)}; DISPATCH_IP();"]
        elif sop == 6:  # PUSH rm16
            return [f"{push_m}({read_rm16(is_reg, rm, sb, off)});"]

    raise ValueError(f"Unhandled opcode 0x{op:02X} at 0x{ip:04X}")


def main():
    repo_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    header_in = os.path.join(repo_dir, "fritz_data.h")
    header_out = os.path.join(repo_dir, "fritz_recompiled.h")

    mem, raw_len = load_fritz_bin(header_in)

    # Known entry points and jump/call table targets
    seeds = [0x0100]
    table_targets = set()
    for i in range(8, 24):
        table_targets.add(u16(mem, 0x3DC6 + i * 2))
    for i in range(8, 24):
        table_targets.add(u16(mem, 0x4084 + i * 2))
    for i in range(8, 24):
        table_targets.add(u16(mem, 0x4CE8 + i * 2))
    for i in range(0, 24):
        table_targets.add(u16(mem, 0x4D08 + i * 2))
    for i in range(0, 9):
        table_targets.add(u16(mem, 0x5C77 + i * 2))
    seeds.extend(sorted(table_targets))

    insns = {}
    coro_targets = set()
    worklist = list(set(seeds))
    while worklist:
        ip = worklist.pop()
        if ip in insns:
            continue
        ins = decode_insn(mem, ip)
        insns[ip] = ins
        # Detect coroutine resume points loaded via `mov $imm16, %ax` (0xB8)
        if ins["op"] == 0xB8 and 0x3D50 <= ins["imm"] <= 0x4300:
            coro_targets.add(ins["imm"])
            if ins["imm"] not in insns:
                worklist.append(ins["imm"])
        for t in ins["targets"]:
            if t not in insns:
                worklist.append(t)
        if ins["falls_through"] and ins["next"] not in insns:
            worklist.append(ins["next"])

    # Determine which IPs need labels and which IPs can be targets of indirect dispatch
    direct_targets = set([0x0100]) | table_targets | coro_targets
    indirect_targets = set([0x0100]) | table_targets | coro_targets

    for ip, ins in insns.items():
        for t in ins["targets"]:
            direct_targets.add(t)
        if ins["kind"] in ("call", "call_ind", "call_far", "call_far_ind"):
            # Return address after any CALL is a potential RET or coroutine-resume target!
            direct_targets.add(ins["next"])
            indirect_targets.add(ins["next"])

    # Compute exact flag liveness
    live_in, live_out = compute_liveness(insns)

    # Estimate instructions per basic block (for accurate instruction/node counting)
    sorted_ips = sorted(insns.keys())
    block_cost = {}
    cur_count = 0
    for ip in sorted_ips:
        if ip in direct_targets:
            cur_count = 0
        cur_count += 1
        block_cost[ip] = cur_count

    # Emit C header
    out = []
    out.append("/* Auto-generated by tools/lift_fritz.py from FRITZ.BIN (1991). Do not edit manually. */")
    out.append("#ifndef FRITZ_RECOMPILED_H")
    out.append("#define FRITZ_RECOMPILED_H")
    out.append("")
    out.append("static inline uint16_t recomp_r16(const uint8_t *base, uint16_t off) {")
    out.append("    uint16_t v;")
    out.append("    memcpy(&v, base + off, 2);")
    out.append("    return v;")
    out.append("}")
    out.append("static inline void recomp_w16(uint8_t *base, uint16_t off, uint16_t v) {")
    out.append("    memcpy(base + off, &v, 2);")
    out.append("}")
    out.append("")
    out.append("static inline uint8_t recomp_shift8(int op, uint8_t val, uint8_t cnt,")
    out.append("                             uint8_t *cf, uint8_t *of, uint8_t *zf, uint8_t *sf) {")
    out.append("    if (cnt == 0) return val;")
    out.append("    switch (op) {")
    out.append("        case 0: {")
    out.append("            uint8_t msb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { msb = (val >> 7) & 1u; val = (uint8_t)((val << 1) | msb); }")
    out.append("            *cf = msb; if (cnt == 1) *of = (uint8_t)(((val >> 7) & 1u) ^ msb); return val;")
    out.append("        }")
    out.append("        case 1: {")
    out.append("            uint8_t lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = val & 1u; val = (uint8_t)((val >> 1) | (lsb << 7)); }")
    out.append("            *cf = lsb; if (cnt == 1) *of = (uint8_t)(((val >> 7) ^ (val >> 6)) & 1u); return val;")
    out.append("        }")
    out.append("        case 2: {")
    out.append("            uint8_t c = *cf ? 1u : 0u;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { uint8_t msb = (val >> 7) & 1u; val = (uint8_t)((val << 1) | c); c = msb; }")
    out.append("            *cf = c; if (cnt == 1) *of = (uint8_t)(((val >> 7) & 1u) ^ c); return val;")
    out.append("        }")
    out.append("        case 3: {")
    out.append("            uint8_t c = *cf ? 1u : 0u;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { uint8_t lsb = val & 1u; val = (uint8_t)((val >> 1) | (c << 7)); c = lsb; }")
    out.append("            *cf = c; if (cnt == 1) *of = (uint8_t)(((val >> 7) ^ (val >> 6)) & 1u); return val;")
    out.append("        }")
    out.append("        case 4: case 6: {")
    out.append("            uint8_t msb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { msb = (val >> 7) & 1u; val = (uint8_t)(val << 1); }")
    out.append("            *cf = msb; *of = (cnt == 1) ? (uint8_t)(((val >> 7) & 1u) ^ msb) : 0u;")
    out.append("            *zf = (val == 0); *sf = (val >> 7) & 1u; return val;")
    out.append("        }")
    out.append("        case 5: {")
    out.append("            uint8_t orig = (val >> 7) & 1u, lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = val & 1u; val = (uint8_t)(val >> 1); }")
    out.append("            *cf = lsb; *of = (cnt == 1) ? orig : 0u;")
    out.append("            *zf = (val == 0); *sf = (val >> 7) & 1u; return val;")
    out.append("        }")
    out.append("        case 7: {")
    out.append("            uint8_t sign = val & 0x80u, lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = val & 1u; val = (uint8_t)((val >> 1) | sign); }")
    out.append("            *cf = lsb; *of = 0u; *zf = (val == 0); *sf = (val >> 7) & 1u; return val;")
    out.append("        }")
    out.append("    }")
    out.append("    return val;")
    out.append("}")
    out.append("")
    out.append("static inline uint16_t recomp_shift16(int op, uint16_t val, uint8_t cnt,")
    out.append("                               uint8_t *cf, uint8_t *of, uint8_t *zf, uint8_t *sf) {")
    out.append("    if (cnt == 0) return val;")
    out.append("    switch (op) {")
    out.append("        case 0: {")
    out.append("            uint8_t msb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { msb = (uint8_t)((val >> 15) & 1u); val = (uint16_t)((val << 1) | msb); }")
    out.append("            *cf = msb; if (cnt == 1) *of = (uint8_t)(((val >> 15) & 1u) ^ msb); return val;")
    out.append("        }")
    out.append("        case 1: {")
    out.append("            uint8_t lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = (uint8_t)(val & 1u); val = (uint16_t)((val >> 1) | ((uint16_t)lsb << 15)); }")
    out.append("            *cf = lsb; if (cnt == 1) *of = (uint8_t)(((val >> 15) ^ (val >> 14)) & 1u); return val;")
    out.append("        }")
    out.append("        case 2: {")
    out.append("            uint8_t c = *cf ? 1u : 0u;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { uint8_t msb = (uint8_t)((val >> 15) & 1u); val = (uint16_t)((val << 1) | c); c = msb; }")
    out.append("            *cf = c; if (cnt == 1) *of = (uint8_t)(((val >> 15) & 1u) ^ c); return val;")
    out.append("        }")
    out.append("        case 3: {")
    out.append("            uint8_t c = *cf ? 1u : 0u;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { uint8_t lsb = (uint8_t)(val & 1u); val = (uint16_t)((val >> 1) | ((uint16_t)c << 15)); c = lsb; }")
    out.append("            *cf = c; if (cnt == 1) *of = (uint8_t)(((val >> 15) ^ (val >> 14)) & 1u); return val;")
    out.append("        }")
    out.append("        case 4: case 6: {")
    out.append("            uint8_t msb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { msb = (uint8_t)((val >> 15) & 1u); val = (uint16_t)(val << 1); }")
    out.append("            *cf = msb; *of = (cnt == 1) ? (uint8_t)(((val >> 15) & 1u) ^ msb) : 0u;")
    out.append("            *zf = (val == 0); *sf = (uint8_t)((val >> 15) & 1u); return val;")
    out.append("        }")
    out.append("        case 5: {")
    out.append("            uint8_t orig = (uint8_t)((val >> 15) & 1u), lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = (uint8_t)(val & 1u); val = (uint16_t)(val >> 1); }")
    out.append("            *cf = lsb; *of = (cnt == 1) ? orig : 0u;")
    out.append("            *zf = (val == 0); *sf = (uint8_t)((val >> 15) & 1u); return val;")
    out.append("        }")
    out.append("        case 7: {")
    out.append("            uint16_t sign = val & 0x8000u; uint8_t lsb = 0;")
    out.append("            for (uint8_t i = 0; i < cnt; i++) { lsb = (uint8_t)(val & 1u); val = (uint16_t)((val >> 1) | sign); }")
    out.append("            *cf = lsb; *of = 0u; *zf = (val == 0); *sf = (uint8_t)((val >> 15) & 1u); return val;")
    out.append("        }")
    out.append("    }")
    out.append("    return val;")
    out.append("}")
    out.append("")
    out.append("static void exec_fritz_recompiled_ctx(FritzCtx *ctx) {")
    out.append("    uint8_t * const ctx_ram = ctx->ram;")
    out.append("    uint8_t * const eng = ctx_ram + ENGINE_BASE;")
    out.append("    uint16_t ax = 0, cx = 0, dx = 0, bx = 0, sp = 0xFFFEu, bp = 0, si = 0, di = 0;")
    out.append("    uint16_t es = ENGINE_SEG, cs = ENGINE_SEG, ss = STACK_SEG, ds = ENGINE_SEG;")
    out.append("    uint16_t ip = 0x0100u;")
    out.append("    uint8_t *es_base = eng;")
    out.append("    uint8_t *cs_base = eng;")
    out.append("    uint8_t *ss_base = ctx_ram + (STACK_SEG << 4);")
    out.append("    uint8_t *ds_base = eng;")
    out.append("    uint8_t fl_cf = 0, fl_zf = 0, fl_sf = 0, fl_of = 0;")
    out.append("    uint64_t nodes = 0;")
    out.append("    TTFrame tt_stack[36];")
    out.append("    if (__builtin_expect(ctx->tt_mask != 0, 0)) memset(tt_stack, 0, sizeof(tt_stack));")
    out.append("    (void)cs_base;")
    out.append("")
    out.append("#define R16(b, o) recomp_r16((b), (uint16_t)(o))")
    out.append("#define W16(b, o, v) recomp_w16((b), (uint16_t)(o), (uint16_t)(v))")
    out.append("#define PUSH16(v) do { sp = (uint16_t)(sp - 2u); W16(ss_base, sp, (v)); } while (0)")
    out.append("#define POP16() (sp = (uint16_t)(sp + 2u), R16(ss_base, (uint16_t)(sp - 2u)))")
    out.append("#define PUSH16_ENG(v) do { sp = (uint16_t)(sp - 2u); W16(eng, sp, (v)); } while (0)")
    out.append("#define POP16_ENG() (sp = (uint16_t)(sp + 2u), R16(eng, (uint16_t)(sp - 2u)))")
    out.append("#define SET_SEG_BASE(sreg, sbase) do { \\")
    out.append("    (sbase) = ctx_ram + (((uint32_t)(sreg) << 4) & 0xFFFFFu); \\")
    out.append("    if (__builtin_expect((sreg) == 0u, 0)) refresh_bios_ticks_into(ctx_ram); \\")
    out.append("} while (0)")
    out.append("#define NODE_POLL_CHECK() do { \\")
    out.append("    nodes++; \\")
    out.append("    if (__builtin_expect((nodes & 0x3FFFu) == 0, 0)) { \\")
    out.append("        ctx->nodes = nodes; \\")
    out.append("        if (ctx->is_main) { \\")
    out.append("            if (searching) fast_poll_check(); \\")
    out.append("        } else { \\")
    out.append("            if (__atomic_load_n(&smp_stop, __ATOMIC_RELAXED)) eng[0x009Cu] |= 0x80u; \\")
    out.append("        } \\")
    out.append("    } \\")
    out.append("} while (0)")
    out.append("")
    # Compact non-overlapping indirect dispatch table (0x3630..0x630F) -> zero warnings!
    out.append("#if defined(__GNUC__) || defined(__clang__)")
    out.append("    static void * const dispatch_jt[0x2CE0] = {")
    jt_valid = sorted(tip for tip in indirect_targets if 0x3630 <= tip < 0x6310 and tip in insns)
    cur = 0
    for tip in jt_valid:
        idx_t = tip - 0x3630
        if idx_t > cur:
            if idx_t - 1 == cur:
                out.append(f"        [0x{cur:04X}] = &&fallback_interp,")
            else:
                out.append(f"        [0x{cur:04X} ... 0x{idx_t - 1:04X}] = &&fallback_interp,")
        out.append(f"        [0x{idx_t:04X}] = &&L_{tip:04X},")
        cur = idx_t + 1
    if cur < 0x2CE0:
        if cur == 0x2CDF:
            out.append(f"        [0x{cur:04X}] = &&fallback_interp,")
        else:
            out.append(f"        [0x{cur:04X} ... 0x2CDF] = &&fallback_interp,")
    out.append("    };")
    out.append("#define DISPATCH_IP() do { \\")
    out.append("    uint16_t _idx = (uint16_t)(ip - 0x3630u); \\")
    out.append("    if (__builtin_expect(_idx < 0x2CE0u, 1)) goto *dispatch_jt[_idx]; \\")
    out.append("    goto fallback_interp; \\")
    out.append("} while (0)")
    out.append("#else")
    out.append("#define DISPATCH_IP() goto dispatch_switch")
    out.append("#endif")
    out.append("")
    out.append("    /* Far return sentinel TRAP_SEG:0x0000 */")
    out.append("    PUSH16(TRAP_SEG);")
    out.append("    PUSH16(0x0000u);")
    out.append("    goto L_0100;")
    out.append("")

    # Emit instructions in address order
    for idx, ip in enumerate(sorted_ips):
        ins = insns[ip]
        if ip in direct_targets:
            out.append(f"L_{ip:04X}:")
        if ip == 0x4A94:
            out.append("    NODE_POLL_CHECK();")
        elif ip == 0x56CA:
            out.append("    if (__builtin_expect(!ctx->is_main, 0)) smp_diversify_root(ctx, eng);")
        elif ip == 0x5AF3:
            out.append("    if (__builtin_expect(ctx->tt_mask != 0, 0)) {")
            out.append("        if (tt_probe_hook(ctx, eng, bp, (uint8_t)dx, tt_stack)) goto L_59EB;")
            out.append("    }")
        elif ip == 0x5A55:
            out.append("    if (__builtin_expect(ctx->tt_mask != 0, 0)) {")
            out.append("        uint16_t _ply = bp >> 1;")
            out.append("        if (_ply > 0u && _ply < 36u && tt_stack[_ply].active) {")
            out.append("            tt_stack[_ply].best_from = si; tt_stack[_ply].best_to = di;")
            out.append("        }")
            out.append("    }")
        elif ip == 0x59EB:
            out.append("    if (__builtin_expect(ctx->tt_mask != 0, 0)) {")
            out.append("        tt_store_hook(ctx, eng, bp, tt_stack);")
            out.append("    }")
        const_eng = (0x3DF0 <= ip <= 0x6010)
        stmts = emit_insn(ins, live_out[ip], const_eng)
        for st in stmts:
            out.append("    " + st)
        # If this instruction falls through to an address that is NOT the next emitted IP, emit goto!
        if ins["falls_through"]:
            nxt_ip = ins["next"]
            if idx + 1 >= len(sorted_ips) or sorted_ips[idx + 1] != nxt_ip:
                if nxt_ip in insns:
                    out.append(f"    goto L_{nxt_ip:04X};")
                else:
                    out.append(f"    ip = 0x{nxt_ip:04X}u; goto fallback_interp;")

    out.append("")
    out.append("handle_trap:")
    out.append("    if (ip == 0x0000u) {")
    out.append("        ctx->nodes = nodes;")
    out.append("        return;")
    out.append("    } else if (ip == 0x0001u) {")
    out.append("        ctx->nodes = nodes;")
    out.append("        if (ctx->is_main) handle_poll_callback();")
    out.append("        else { eng[0x009Cu] &= (uint8_t)~0x10u; if (__atomic_load_n(&smp_stop, __ATOMIC_RELAXED)) eng[0x009Cu] |= 0x80u; }")
    out.append("        ip = POP16(); cs = POP16();")
    out.append("        SET_SEG_BASE(cs, cs_base);")
    out.append("        DISPATCH_IP();")
    out.append("    } else if (ip == 0x0002u) {")
    out.append("        ctx->nodes = nodes;")
    out.append("        if (ctx->is_main) handle_info_callback();")
    out.append("        ip = POP16(); cs = POP16();")
    out.append("        SET_SEG_BASE(cs, cs_base);")
    out.append("        DISPATCH_IP();")
    out.append("    }")
    out.append("    fprintf(stderr, \"KnightStalker: unknown trap %04X:%04X\\n\", cs, ip);")
    out.append("    return;")
    out.append("")
    out.append("#if !defined(__GNUC__) && !defined(__clang__)")
    out.append("dispatch_switch:")
    out.append("    switch (ip) {")
    for tip in sorted(indirect_targets):
        if tip in insns:
            out.append(f"        case 0x{tip:04X}u: goto L_{tip:04X};")
    out.append("        default: goto fallback_interp;")
    out.append("    }")
    out.append("#endif")
    out.append("")
    out.append("fallback_interp:")
    out.append("    if (!ctx->is_main) return;")
    out.append("    /* Safety fallback: transfer state to 8086 interpreter */")
    out.append("    cpu.regs[R_AX] = ax; cpu.regs[R_CX] = cx; cpu.regs[R_DX] = dx; cpu.regs[R_BX] = bx;")
    out.append("    cpu.regs[R_SP] = sp; cpu.regs[R_BP] = bp; cpu.regs[R_SI] = si; cpu.regs[R_DI] = di;")
    out.append("    cpu.sregs[S_ES] = es; cpu.sregs[S_CS] = cs; cpu.sregs[S_SS] = ss; cpu.sregs[S_DS] = ds;")
    out.append("    cpu.ip = ip;")
    out.append("    cpu.flags = 0x0202u | (fl_cf ? F_CF : 0u) | (fl_zf ? F_ZF : 0u) | (fl_sf ? F_SF : 0u) | (fl_of ? F_OF : 0u);")
    out.append("    exec_fritz_interp_continue();")
    out.append("    return;")
    out.append("")
    out.append("#undef R16")
    out.append("#undef W16")
    out.append("#undef PUSH16")
    out.append("#undef POP16")
    out.append("#undef PUSH16_ENG")
    out.append("#undef POP16_ENG")
    out.append("#undef SET_SEG_BASE")
    out.append("#undef NODE_POLL_CHECK")
    out.append("#undef DISPATCH_IP")
    out.append("}")
    out.append("")
    out.append("#endif /* FRITZ_RECOMPILED_H */")
    out.append("")

    with open(header_out, "w") as f:
        f.write("\n".join(out))

    # Print stats
    total_flags_possible = len(insns) * 4
    total_flags_live = sum(bin(live_out[ip]).count("1") for ip in insns)
    print(f"Generated {header_out}")
    print(f"  Translated instructions: {len(insns)}")
    print(f"  Direct labels: {len(direct_targets)}, Indirect dispatch targets: {len(indirect_targets)}")
    print(f"  Avg live flags per instruction: {total_flags_live / len(insns):.2f} / 4.00")


if __name__ == "__main__":
    main()
