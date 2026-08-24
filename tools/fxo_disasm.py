"""Disassemble the DX9 shaders embedded in a .fxo_pc.

Uses D3DXDisassembleShader from the d3dx9_43.dll that ships with Windows, so no
DirectX SDK install is required.

Usage:  python fxo_disasm.py <file.fxo_pc> [shader_index] [--out FILE]
        python fxo_disasm.py <file.fxo_pc> --list
"""
import ctypes
import ctypes.wintypes as wt
import os
import struct
import sys

END_TOKEN = 0x0000FFFF
COMMENT_MASK = 0xFFFF
COMMENT_TOKEN = 0xFFFE
VERSION_TOKENS = {0xFFFE: "vs", 0xFFFF: "ps"}


def find_shaders(buf):
    """Yield (offset, end, label) for each compiled shader blob in the container."""
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
    # Nested/duplicate hits: keep only blobs that don't start inside a previous one.
    result = []
    for start, end, label in shaders:
        if result and start < result[-1][1]:
            continue
        result.append((start, end, label))
    return result


def walk_tokens(buf, start):
    """Return the offset just past the END token, or None if malformed."""
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


def disassemble(blob):
    d3dx = ctypes.windll.d3dx9_43
    buffer_ptr = ctypes.c_void_p()
    hr = d3dx.D3DXDisassembleShader(
        ctypes.c_char_p(blob), wt.BOOL(False), None, ctypes.byref(buffer_ptr)
    )
    if hr != 0 or not buffer_ptr:
        raise OSError(f"D3DXDisassembleShader failed (hr=0x{hr & 0xFFFFFFFF:08X})")

    # ID3DXBuffer vtable: 0 QueryInterface, 1 AddRef, 2 Release,
    #                     3 GetBufferPointer, 4 GetBufferSize
    vtable = ctypes.cast(buffer_ptr, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p)))[0]
    get_ptr = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vtable[3])
    get_size = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[4])
    release = ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vtable[2])

    data = ctypes.string_at(get_ptr(buffer_ptr), get_size(buffer_ptr))
    release(buffer_ptr)
    return data.decode("ascii", "replace")


def main(argv):
    if not argv:
        print(__doc__)
        return 1

    path = argv[0]
    with open(path, "rb") as handle:
        buf = handle.read()
    shaders = find_shaders(buf)

    if "--list" in argv:
        print(f"{os.path.basename(path)}: {len(shaders)} shaders")
        for i, (start, end, label) in enumerate(shaders):
            print(f"  [{i}] {label:<8} offset={start:<7} {end - start:>6} bytes")
        return 0

    index = int(argv[1]) if len(argv) > 1 and argv[1].isdigit() else 0
    start, end, label = shaders[index]
    text = disassemble(buf[start:end])

    if "--out" in argv:
        out = argv[argv.index("--out") + 1]
        with open(out, "w", encoding="utf-8") as handle:
            handle.write(text)
        print(f"wrote {out} ({label}, {len(text)} chars)")
    else:
        print(f"// {os.path.basename(path)} shader[{index}] {label}\n{text}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
