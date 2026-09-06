"""Batch-disassemble every DX9 shader container in a directory and extract facts.

Writes one <name>.asm per .fxo_pc (all technique variants concatenated) and a
single _facts.json describing every shader blob: pipeline stage, declared vertex
inputs/outputs, samplers, named constants from the CTAB, an instruction
histogram and a set of behaviour flags.

Uses D3DXDisassembleShader out of the in-box d3dx9_43.dll, same as fxo_disasm.py.

Usage:  python fxo_batch_disasm.py <shader_dir> <asm_out_dir> <facts.json>
"""
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import struct
import sys

END_TOKEN = 0x0000FFFF
COMMENT_MASK = 0xFFFF
COMMENT_TOKEN = 0xFFFE
VERSION_TOKENS = {0xFFFE: "vs", 0xFFFF: "ps"}


def find_shaders(buf):
    shaders = []
    for pos in range(0, len(buf) - 4, 4):
        word = struct.unpack_from("<I", buf, pos)[0]
        kind = VERSION_TOKENS.get(word >> 16)
        if not kind or word == 0xFFFFFFFF:
            continue
        major, minor = (word >> 8) & 0xFF, word & 0xFF
        if major not in (1, 2, 3):
            continue
        end = walk_tokens(buf, pos)
        if end:
            shaders.append((pos, end, f"{kind}_{major}_{minor}"))
    result = []
    for start, end, label in shaders:
        if result and start < result[-1][1]:
            continue
        result.append((start, end, label))
    return result


def walk_tokens(buf, start):
    pos = start + 4
    while pos + 4 <= len(buf):
        word = struct.unpack_from("<I", buf, pos)[0]
        if word == END_TOKEN:
            return pos + 4
        if (word & COMMENT_MASK) == COMMENT_TOKEN:
            pos += 4 + ((word >> 16) & 0x7FFF) * 4
        else:
            pos += 4
    return None


_d3dx = ctypes.windll.d3dx9_43


def disassemble(blob):
    buffer_ptr = ctypes.c_void_p()
    hr = _d3dx.D3DXDisassembleShader(
        ctypes.c_char_p(blob), wt.BOOL(False), None, ctypes.byref(buffer_ptr)
    )
    if hr != 0 or not buffer_ptr:
        raise OSError(f"D3DXDisassembleShader failed (hr=0x{hr & 0xFFFFFFFF:08X})")
    vtable = ctypes.cast(buffer_ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[2])
    data = ctypes.string_at(get_ptr(buffer_ptr), get_size(buffer_ptr))
    release(buffer_ptr)
    # The listing carries the raw creator string, which can hold stray bytes,
    # and D3DX NUL-terminates the buffer and uses CRLF line endings.
    text = data.decode("ascii", "replace")
    text = text.replace(chr(0xFFFD), "").replace(chr(0), "")
    return text.replace(chr(13) + chr(10), chr(10))


REG_ROW = re.compile(r"^//\s+(\S+)\s+([a-z]\d+)\s+(\d+)\s*$")
DCL_IN = re.compile(r"^\s+dcl(_\w+)?\s+(v\d+)")
DCL_OUT = re.compile(r"^\s+dcl(_\w+)?\s+(o\w+)")
DCL_SAMP = re.compile(r"^\s+dcl_(2d|cube|volume)\s+(s\d+)")
OPCODE = re.compile(r"^\s+([a-z][a-z0-9_]*)")

NON_ALU = {"def", "defi", "defb", "dcl", "vs", "ps"}

FLAG_NEEDLES = (
    ("texkill", "texkill"),
    ("depth_output", "oDepth"),
    ("sincos", "sincos"),
    ("dynamic_branch", "if_"),
    ("static_branch", "\n    if "),
    ("loop", "\n    rep "),
    ("loop", "\n    loop "),
    ("bias_sample", "texldb"),
    ("lod_sample", "texldl"),
    ("proj_sample", "texldp"),
    ("derivatives", "dsx"),
    ("indexed_const", "mova"),
    ("centroid", "_centroid"),
    ("pp_precision", "_pp"),
)


def parse_shader(text, label):
    """Pull structured facts out of one D3DX disassembly listing."""
    facts = {
        "target": label,
        "params": [],
        "inputs": [],
        "outputs": [],
        "samplers": [],
        "instr_slots": None,
        "opcodes": {},
        "flags": [],
    }
    in_registers = False
    for line in text.splitlines():
        if line.startswith("//   Name"):
            in_registers = True
            continue
        if in_registers:
            if line.startswith("//   ---"):
                continue
            match = REG_ROW.match(line)
            if match:
                facts["params"].append(
                    {"name": match.group(1), "reg": match.group(2), "size": int(match.group(3))}
                )
                continue
            if facts["params"]:
                in_registers = False
            continue
        if "instruction slots used" in line:
            found = re.search(r"(\d+)\s+instruction", line)
            if found:
                facts["instr_slots"] = int(found.group(1))
            continue
        if not line.startswith("    "):
            continue
        stripped = line.rstrip()
        match = DCL_SAMP.match(stripped)
        if match:
            facts["samplers"].append({"stage": match.group(2), "kind": match.group(1)})
            continue
        match = DCL_IN.match(stripped)
        if match:
            facts["inputs"].append(
                {"reg": match.group(2), "semantic": (match.group(1) or "")[1:] or "?"}
            )
            continue
        match = DCL_OUT.match(stripped)
        if match:
            facts["outputs"].append(
                {"reg": match.group(2), "semantic": (match.group(1) or "")[1:] or "?"}
            )
            continue
        match = OPCODE.match(stripped)
        if match:
            op = match.group(1)
            base = op.split("_")[0]
            if base in NON_ALU or base.startswith("dcl"):
                continue
            facts["opcodes"][base] = facts["opcodes"].get(base, 0) + 1

    for flag, needle in FLAG_NEEDLES:
        if needle in text and flag not in facts["flags"]:
            facts["flags"].append(flag)

    facts["const_regs"] = sorted({int(m) for m in re.findall(r"\bc(\d+)\b", text)})
    facts["color_outputs"] = sorted(set(re.findall(r"\boC\d\b", text)))
    return facts


def main(argv):
    if len(argv) < 3:
        print(__doc__)
        return 1
    src_dir, asm_dir, facts_path = argv[0], argv[1], argv[2]
    os.makedirs(asm_dir, exist_ok=True)

    files = sorted(f for f in os.listdir(src_dir) if f.endswith((".fxo_pc", ".fxo")))
    all_facts = {}
    errors = []
    total = 0

    for filename in files:
        path = os.path.join(src_dir, filename)
        with open(path, "rb") as handle:
            buf = handle.read()
        blobs = find_shaders(buf)
        stem = filename.rsplit(".fxo", 1)[0]
        chunks = [f"// ==== {filename} : {len(blobs)} shader blobs, {len(buf)} bytes ====\n"]
        shader_facts = []
        for i, (start, end, label) in enumerate(blobs):
            try:
                text = disassemble(buf[start:end])
            except OSError as exc:
                errors.append(f"{filename}[{i}]: {exc}")
                continue
            chunks.append(
                f"\n// ---------------- shader[{i}] {label} "
                f"offset={start} size={end - start} ----------------\n{text}"
            )
            fact = parse_shader(text, label)
            fact["index"] = i
            fact["bytes"] = end - start
            shader_facts.append(fact)
            total += 1
        asm_path = os.path.join(asm_dir, stem + ".asm")
        with open(asm_path, "w", encoding="utf-8", newline=chr(10)) as handle:
            handle.write("".join(chunks))
        all_facts[stem] = {
            "file": filename,
            "file_bytes": len(buf),
            "blob_count": len(blobs),
            "shaders": shader_facts,
        }

    with open(facts_path, "w", encoding="utf-8") as handle:
        json.dump(all_facts, handle, indent=1)

    print(f"containers: {len(files)}   shaders disassembled: {total}")
    print(f"asm:   {asm_dir}")
    print(f"facts: {facts_path}")
    if errors:
        print(f"{len(errors)} failures:")
        for line in errors[:20]:
            print("  " + line)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
