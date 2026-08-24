"""Extract files from a Volition VPP_PC v6 packfile (Saints Row: The Third).

Format was derived by inspection of shaders.vpp_pc; see docs/shader-map.md.
Header field offsets are empirically validated (index_size == index_count * 24).

Usage:  python vpp_extract.py <packfile.vpp_pc> <output_dir> [--limit N] [--dry-run]
"""
import os
import struct
import sys
import zlib

MAGIC = 0x51890ACE
ALIGN = 2048
ENTRY_SIZE = 24
DIR_OFFSET = 0x800

# Header field offsets (see module docstring).
OFF_FLAGS = 0x14C
OFF_INDEX_COUNT = 0x154
OFF_PACKAGE_SIZE = 0x158
OFF_INDEX_SIZE = 0x15C
OFF_NAMES_SIZE = 0x160
OFF_DATA_SIZE = 0x164
OFF_COMPRESSED_SIZE = 0x168


def align_up(value, alignment=ALIGN):
    return (value + alignment - 1) // alignment * alignment


def u32(buf, offset):
    return struct.unpack_from("<I", buf, offset)[0]


class Packfile:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as handle:
            self.data = handle.read()

        magic = u32(self.data, 0)
        if magic != MAGIC:
            raise ValueError(f"not a VPP_PC file (magic 0x{magic:08X})")
        self.version = u32(self.data, 4)
        if self.version != 6:
            raise ValueError(f"unsupported VPP version {self.version} (only v6 known)")

        self.flags = u32(self.data, OFF_FLAGS)
        self.index_count = u32(self.data, OFF_INDEX_COUNT)
        self.package_size = u32(self.data, OFF_PACKAGE_SIZE)
        self.index_size = u32(self.data, OFF_INDEX_SIZE)
        self.names_size = u32(self.data, OFF_NAMES_SIZE)
        self.data_size = u32(self.data, OFF_DATA_SIZE)
        self.compressed_size = u32(self.data, OFF_COMPRESSED_SIZE)

        if self.index_size != self.index_count * ENTRY_SIZE:
            raise ValueError(
                f"index_size {self.index_size} != {self.index_count} * {ENTRY_SIZE}; "
                "header layout assumption is wrong"
            )

        self.names_offset = align_up(DIR_OFFSET + self.index_size)
        self.data_offset = align_up(self.names_offset + self.names_size)
        self.entries = self._read_entries()

    def _read_entries(self):
        entries = []
        names = self.data[self.names_offset:self.names_offset + self.names_size]
        for i in range(self.index_count):
            base = DIR_OFFSET + i * ENTRY_SIZE
            name_offset, _unk0, offset, size, compressed_size, _unk1 = struct.unpack_from(
                "<6I", self.data, base
            )
            end = names.find(b"\0", name_offset)
            name = names[name_offset:end].decode("ascii", "replace")
            entries.append(
                {
                    "name": name,
                    "offset": offset,          # offset within the *uncompressed* stream
                    "size": size,              # uncompressed size
                    "compressed_size": compressed_size,
                }
            )
        return entries

    def summary(self):
        total_compressed = sum(e["compressed_size"] for e in self.entries)
        return "\n".join(
            [
                f"file            : {os.path.basename(self.path)}",
                f"version         : {self.version}   flags: 0x{self.flags:08X}",
                f"files           : {self.index_count}",
                f"names table     : 0x{self.names_offset:X} ({self.names_size} bytes)",
                f"data section    : 0x{self.data_offset:X}",
                f"data size       : {self.data_size} uncompressed / {self.compressed_size} compressed",
                f"sum(entry comp) : {total_compressed}"
                + ("  [contiguous]" if total_compressed == self.compressed_size else "  [padded?]"),
            ]
        )

    def extract_all(self, out_dir, limit=None):
        """Each file is its own zlib stream, padded up to the 2048-byte alignment."""
        os.makedirs(out_dir, exist_ok=True)
        cursor = self.data_offset
        written = failed = 0

        for entry in self.entries[:limit]:
            block = self.data[cursor:cursor + entry["compressed_size"]]
            cursor += align_up(entry["compressed_size"])
            try:
                # Stored streams carry a zlib header but no adler32 trailer, so decode
                # the deflate payload directly rather than letting zlib demand one.
                decoder = zlib.decompressobj(-zlib.MAX_WBITS)
                payload = decoder.decompress(block[2:]) + decoder.flush()
            except zlib.error as exc:
                print(f"  FAIL {entry['name']}: {exc}")
                failed += 1
                continue
            if len(payload) != entry["size"]:
                print(f"  WARN {entry['name']}: got {len(payload)}, expected {entry['size']}")

            safe = entry["name"].replace("\\", "_").replace("/", "_")
            with open(os.path.join(out_dir, safe), "wb") as handle:
                handle.write(payload)
            written += 1

        return written, failed


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 1

    pack = Packfile(argv[0])
    print(pack.summary())

    limit = None
    if "--limit" in argv:
        limit = int(argv[argv.index("--limit") + 1])

    print("\nfirst 5 entries:")
    for entry in pack.entries[:5]:
        print(
            f"  {entry['name']:<40} off={entry['offset']:<10} "
            f"{entry['compressed_size']:>8} -> {entry['size']:>8}"
        )

    if "--dry-run" in argv:
        return 0

    out_dir = argv[1]
    print(f"\nextracting to {out_dir} ...")
    written, failed = pack.extract_all(out_dir, limit)
    print(f"done: {written} written, {failed} failed")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
