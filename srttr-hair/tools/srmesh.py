"""Saints Row 3 / SR3 Remastered character mesh (.ccmesh_pc + .gcmesh_pc) reader.

Layout, verified against cf_hair_longhiponybangs in both games:

.ccmesh_pc  mesh block (starts at the 'version 9' u32, after the bone-remap table)
    +0x00 u32  version (9)
    +0x04 u32  mesh hash  (repeated as the first u32 of the .gcmesh_pc)
    ...       index_count, index_offset, vertex_count, vertex_stride, vertex_offset
              -- 32-bit fields in the original, 64-bit in the Remaster

.gcmesh_pc
    +0x00 u32  mesh hash
    index_offset   u16 indices, index_count of them
    vertex_offset  vertex_count records of vertex_stride (32) bytes:
        +0x00 float3  position          (object space, metres)
        +0x0C 4 x u8  packed normal
        +0x10 4 x u8  packed tangent
        +0x14 4 x u8  bone weights (sum 255)
        +0x18 4 x u8  bone indices (255 = unused)
        +0x1C 2 x i16 UV (fixed point)

Both games use the SAME vertex layout and stride, so geometry is interchangeable.
"""
import struct

STRIDE = 32


class Mesh:
    def __init__(self, cc, gc):
        self.cc, self.gc = cc, gc
        self.hash = struct.unpack_from("<I", gc, 0)[0]
        # locate the mesh block in the ccmesh: the u32 pair (9, hash)
        blk = None
        for o in range(0, len(cc) - 8, 4):
            if struct.unpack_from("<I", cc, o)[0] == 9 and \
               struct.unpack_from("<I", cc, o + 4)[0] == self.hash:
                blk = o
                break
        if blk is None:
            raise ValueError("mesh block not found")
        self.blk = blk
        # 32-bit layout puts gcmesh size at +0x0C, 64-bit at +0x10
        if struct.unpack_from("<I", cc, blk + 0x0C)[0] == len(gc):
            self.wide = False
            self.n_idx = struct.unpack_from("<I", cc, blk + 0x30)[0]
            self.o_idx = struct.unpack_from("<I", cc, blk + 0x38)[0]
            n_groups = struct.unpack_from("<I", cc, blk + 0x20)[0]
            self.groups = []
            for i in range(n_groups):
                o = blk + 0x80 + i * 0x18
                cnt = struct.unpack_from("<I", cc, o)[0]
                stride = cc[o + 4]
                off = struct.unpack_from("<I", cc, o + 0x10)[0]
                self.groups.append((cnt, stride, off))
            hair = max((g for g in self.groups if g[1] == STRIDE),
                       key=lambda g: g[0], default=None)
            if hair is None:
                raise ValueError("no stride-32 vertex group")
            self.n_vtx, self.stride, self.o_vtx = hair
        elif struct.unpack_from("<Q", cc, blk + 0x10)[0] == len(gc):
            self.wide = True
            self.n_idx = struct.unpack_from("<Q", cc, blk + 0x40)[0]
            self.o_idx = struct.unpack_from("<Q", cc, blk + 0x48)[0]
            n_groups = struct.unpack_from("<Q", cc, blk + 0x30)[0]
            self.groups = []
            for i in range(n_groups):
                o = blk + 0x90 + i * 0x18
                cnt = struct.unpack_from("<I", cc, o)[0]
                stride = cc[o + 4]
                off = struct.unpack_from("<Q", cc, o + 0x10)[0]
                self.groups.append((cnt, stride, off))
            # Multi-part styles interleave accessory groups (stride 36 - glasses,
            # chopsticks, hat brims) with the hair itself, and the accessory is not
            # always first. The hair is the largest stride-32 group; picking it by
            # position instead reshapes the glasses onto the scalp.
            hair = max((g for g in self.groups if g[1] == STRIDE),
                       key=lambda g: g[0], default=None)
            if hair is None:
                raise ValueError("no stride-32 vertex group")
            self.n_vtx, self.stride, self.o_vtx = hair
        else:
            raise ValueError("gcmesh size field not found")

    def lods(self):
        """[(index_start, index_count, vertex_start, vertex_end)] per LOD.

        Each entry is five u32: a leading zero then the index and vertex ranges.
        The table's offset within the mesh block shifts with the number of vertex
        buffer groups (multi-part styles such as hair+glasses carry two), so it is
        found by its structure rather than at a fixed offset: ranges must start at
        zero, be contiguous, sum to the index count and stay inside the vertex count.
        """
        if not self.wide:
            return [(0, self.n_idx, 0, self.n_vtx - 1)]
        for n in range(1, 9):
            need = 20 * n
            for base in range(self.blk, len(self.cc) - need, 4):
                ent = []
                good = True
                for i in range(n):
                    z, istart, icount, vstart, vend = struct.unpack_from(
                        "<5I", self.cc, base + i * 20)
                    if z != 0 or icount == 0 or vend >= self.n_vtx or vend < vstart:
                        good = False
                        break
                    ent.append((istart, icount, vstart, vend))
                if not good or ent[0][0] != 0 or ent[0][2] != 0:
                    continue
                if sum(e[1] for e in ent) != self.n_idx:
                    continue
                if any(ent[i][0] + ent[i][1] != ent[i + 1][0] for i in range(n - 1)):
                    continue
                if any(ent[i][3] >= ent[i + 1][2] for i in range(n - 1)):
                    continue
                return ent
        raise ValueError("LOD table not found")

    def positions(self):
        return [struct.unpack_from("<3f", self.gc, self.o_vtx + i * self.stride)
                for i in range(self.n_vtx)]

    def uvs(self):
        return [struct.unpack_from("<2h", self.gc, self.o_vtx + i * self.stride + 0x1C)
                for i in range(self.n_vtx)]

    def indices(self):
        return struct.unpack_from(f"<{self.n_idx}H", self.gc, self.o_idx)

    def tris(self, lo=0, hi=None):
        """Decode the index buffer as a TRIANGLE STRIP.

        Both games store one long strip per LOD, with degenerate (repeated-index)
        triangles used to stitch separate hair cards together. Reading it as a
        triangle list silently drops two thirds of the surface, which shows up as
        a mesh full of holes - that is how the strip layout was spotted.

        Multi-part styles (hair+glasses, hair+chopsticks) carry a second vertex
        buffer group that this reader does not expose, and the shared index buffer
        addresses it too; those triangles are dropped rather than indexing past the
        end of the first group.
        """
        idx = self.indices()[lo:hi if hi is not None else self.n_idx]
        n = self.n_vtx
        out = []
        for i in range(len(idx) - 2):
            a, b, c = idx[i], idx[i + 1], idx[i + 2]
            if a == b or b == c or a == c:
                continue                      # stitch
            if a >= n or b >= n or c >= n:
                continue                      # belongs to another buffer group
            out.append((a, b, c) if i % 2 == 0 else (a, c, b))
        return out

    def __repr__(self):
        return (f"<Mesh {'64' if self.wide else '32'}-bit hash=0x{self.hash:08X} "
                f"verts={self.n_vtx} stride={self.stride} idx={self.n_idx} "
                f"(idx@{self.o_idx} vtx@{self.o_vtx})>")
