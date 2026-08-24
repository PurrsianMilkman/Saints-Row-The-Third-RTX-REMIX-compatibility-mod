"""Scan extracted .fxo_pc shaders for their D3D9 constant tables.

Each .fxo_pc holds several compiled DX9 shaders (technique variants). Every shader
keeps an intact CTAB, so constant *names* survive - which is how we locate the
camera matrices without touching the game binary.

Usage:  python fxo_scan.py <shader_dir> <out.csv>
"""
import csv
import os
import struct
import sys
from collections import Counter, defaultdict

REGISTER_SETS = {0: "bool", 1: "int4", 2: "float4", 3: "sampler"}
CONSTANT_INFO_SIZE = 20


def read_cstring(buf, offset):
    end = buf.find(b"\0", offset)
    return buf[offset:end].decode("ascii", "replace")


def parse_ctab(buf, ctab_pos):
    """Parse one constant table. Offsets are relative to the byte after 'CTAB'."""
    base = ctab_pos + 4
    size, creator, version, count, info_off, _flags, target = struct.unpack_from(
        "<7I", buf, base
    )
    if size != 28 or count > 512:
        return None

    constants = []
    for i in range(count):
        entry = base + info_off + i * CONSTANT_INFO_SIZE
        if entry + CONSTANT_INFO_SIZE > len(buf):
            break
        name_off, reg_set, reg_index, reg_count, _res, _type, _default = struct.unpack_from(
            "<IHHHHII", buf, entry
        )
        constants.append(
            {
                "name": read_cstring(buf, base + name_off),
                "set": REGISTER_SETS.get(reg_set, str(reg_set)),
                "index": reg_index,
                "count": reg_count,
            }
        )
    return {
        "version": version,
        "target": read_cstring(buf, base + target),
        "creator": read_cstring(buf, base + creator),
        "constants": constants,
    }


def scan_file(path):
    with open(path, "rb") as handle:
        buf = handle.read()
    tables = []
    pos = buf.find(b"CTAB")
    while pos != -1:
        table = parse_ctab(buf, pos)
        if table:
            tables.append(table)
        pos = buf.find(b"CTAB", pos + 4)
    return tables


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1
    shader_dir, out_csv = argv[0], argv[1]

    files = sorted(f for f in os.listdir(shader_dir) if f.endswith(".fxo_pc"))
    name_counter = Counter()
    matrix_regs = defaultdict(Counter)
    rows = []
    shader_total = 0

    for filename in files:
        for index, table in enumerate(scan_file(os.path.join(shader_dir, filename))):
            shader_total += 1
            for const in table["constants"]:
                name_counter[const["name"]] += 1
                if const["count"] >= 3 and const["set"] == "float4":
                    matrix_regs[const["name"]][const["index"]] += 1
                rows.append(
                    {
                        "file": filename,
                        "shader": index,
                        "target": table["target"],
                        "name": const["name"],
                        "set": const["set"],
                        "reg": const["index"],
                        "count": const["count"],
                    }
                )

    with open(out_csv, "w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle, fieldnames=["file", "shader", "target", "name", "set", "reg", "count"]
        )
        writer.writeheader()
        writer.writerows(rows)

    print(f"files: {len(files)}   shaders: {shader_total}   constants: {len(rows)}")
    print(f"csv:   {out_csv}\n")

    print("=== 25 most common constants ===")
    for name, hits in name_counter.most_common(25):
        print(f"  {hits:>6}  {name}")

    print("\n=== matrix-shaped constants (float4, >=3 registers) and their register slots ===")
    ranked = sorted(matrix_regs.items(), key=lambda kv: -sum(kv[1].values()))
    for name, regs in ranked[:20]:
        spread = ", ".join(f"c{reg}x{hits}" for reg, hits in regs.most_common(4))
        print(f"  {sum(regs.values()):>6}  {name:<32} {spread}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
