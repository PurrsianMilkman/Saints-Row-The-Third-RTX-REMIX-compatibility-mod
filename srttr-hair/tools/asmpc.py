"""Reader/patcher for Volition .asm_pc stream-index files (BEEFFEED).

SRTTR uses version 20 with 64-bit sizes; SRTT used version 11. The file tells the
streaming system how big each .str2_pc container is and how big every asset inside
it is, so a rebuilt container is only loadable once these numbers are updated.

    header
        u32 0xBEEFFEED
        u16 version
        u16 container_count
        u32 type_count
        type_count x (u16 len + chars + u8 index)      allocator names
        u32 prim_count
        prim_count x (u16 len + chars + u8 index)      primitive-type names

    container  x container_count
        u16 len + name              (no .str2_pc suffix)
        u8   kind                   (0x0a for custmesh containers)
        u16  alignment              (0x80)
        u16  file_count
        u32  header_size            (the container's data_at, e.g. 0x3000)
        14 bytes reserved (zero in every shipped record)
        u64  compressed_size        (total container payload, header excluded)
        u64  sizes[2 * file_count]  (cpu, gpu) per file, in file order
        file x file_count
            u16 len + name
            u16 type
            u16 flags
            u64 cpu_size
            u64 gpu_size
            u8  reserved

Patching is done in place: only the numbers that change are rewritten, so every
byte this module does not understand survives untouched.
"""
import struct

SIG = 0xBEEFFEED


class File:
    __slots__ = ("name", "type", "flags", "cpu", "gpu", "off_cpu", "off_gpu")

    def __init__(self, name, type_, flags, cpu, gpu, off_cpu, off_gpu):
        self.name, self.type, self.flags = name, type_, flags
        self.cpu, self.gpu = cpu, gpu
        self.off_cpu, self.off_gpu = off_cpu, off_gpu

    def __repr__(self):
        return f"<{self.name} type={self.type} cpu={self.cpu} gpu={self.gpu}>"


class Container:
    __slots__ = ("name", "kind", "align", "header_size", "comp", "files",
                 "off_comp", "off_sizes", "off_header_size")

    def __repr__(self):
        return f"<Container {self.name} files={len(self.files)} comp={self.comp}>"


class Asm:
    def __init__(self, data):
        self.d = bytearray(data)
        d = self.d
        if struct.unpack_from("<I", d, 0)[0] != SIG:
            raise ValueError("not an .asm_pc")
        self.version = struct.unpack_from("<H", d, 4)[0]
        n_cont = struct.unpack_from("<H", d, 6)[0]
        p = 8
        # three count-prefixed string tables: allocator, primitive and container names
        self.tables = []
        for _ in range(3):
            n = struct.unpack_from("<I", d, p)[0]
            p += 4
            names = []
            for _ in range(n):
                ln = struct.unpack_from("<H", d, p)[0]
                p += 2
                names.append(d[p:p + ln].decode("latin1"))
                p += ln + 1                  # trailing index byte
            self.tables.append(names)
        self.types, self.prims, self.kinds = self.tables
        self.containers = []
        for _ in range(n_cont):
            c = Container()
            ln = struct.unpack_from("<H", d, p)[0]
            p += 2
            c.name = d[p:p + ln].decode("latin1")
            p += ln
            c.kind = d[p]
            c.align = struct.unpack_from("<H", d, p + 1)[0]
            n_files = struct.unpack_from("<H", d, p + 3)[0]
            c.off_header_size = p + 5
            c.header_size = struct.unpack_from("<I", d, p + 5)[0]
            p += 23
            c.off_comp = p
            c.comp = struct.unpack_from("<Q", d, p)[0]
            p += 8
            c.off_sizes = p
            p += 16 * n_files
            c.files = []
            for _ in range(n_files):
                ln = struct.unpack_from("<H", d, p)[0]
                p += 2
                nm = d[p:p + ln].decode("latin1")
                p += ln
                ty, fl = struct.unpack_from("<2H", d, p)
                cpu, gpu = struct.unpack_from("<2Q", d, p + 4)
                c.files.append(File(nm, ty, fl, cpu, gpu, p + 4, p + 12))
                p += 21
            self.containers.append(c)
        self.end = p

    def by_name(self):
        return {c.name: c for c in self.containers}

    def check(self):
        """Every container's size table must agree with its per-file records."""
        bad = []
        for c in self.containers:
            want = []
            for f in c.files:
                want += [f.cpu, f.gpu]
            got = list(struct.unpack_from(f"<{2 * len(c.files)}Q", self.d, c.off_sizes))
            if got != want:
                bad.append((c.name, got, want))
        return bad

    def set_sizes(self, container, comp, per_file):
        """per_file: {asset name -> (cpu_size, gpu_size)}. Rewrites both copies."""
        c = container
        struct.pack_into("<Q", self.d, c.off_comp, comp)
        c.comp = comp
        for i, f in enumerate(c.files):
            if f.name not in per_file:
                continue
            cpu, gpu = per_file[f.name]
            f.cpu, f.gpu = cpu, gpu
            struct.pack_into("<Q", self.d, f.off_cpu, cpu)
            struct.pack_into("<Q", self.d, f.off_gpu, gpu)
            struct.pack_into("<2Q", self.d, c.off_sizes + 16 * i, cpu, gpu)

    def set_header_size(self, container, size):
        container.header_size = size
        struct.pack_into("<I", self.d, container.off_header_size, size)

    def bytes(self):
        return bytes(self.d)
