# Which UV set feeds which sampler, per cloth shader

Measured 2026-09-08 by disassembling every cloth shader in `re/cloth-shaders/` - register table
from the CTAB, coordinate from the `texld` instructions. Reproduce with `tools/cloth_uv_table.py`.

**"Albedo is TEXCOORD0, pattern is TEXCOORD1" is NOT a rule of this engine.**

```
shader                     ps   Pattern_Map        Diffuse_Map              clamp registers
ir_at_sr3pccloth_*        [8][9] s2 <- TEXCOORD1   s0 <- computed (v6)      declared; runtime values not measured
ir_sr3pccloth_*            [6]  s0 <- TEXCOORD1   s3 <- computed (v5)      ClampU1/V1 declared, BOTH 0 at runtime = inactive
ir_sr3npcclothfull_*      [8][9] s0 <- TEXCOORD0   -  <- not sampled
ir_sr3npcclothpulse_*     [8][9] s0 <- TEXCOORD0   -  <- not sampled
ir_sr3npccloth_glow_*     [8][9] s0 <- TEXCOORD3   -  <- not sampled
```

## The player's own clothing - and the underwear in particular

The underwear reports `pattern at stage 0, albedo stage 3` at runtime, which matches
**`ir_sr3pccloth_*` shader [6]** exactly:

    Pattern_MapSampler  s0    <- TEXCOORD1        (dcl_texcoord1_pp v1)
    Normal_MapSampler   s1
    Sphere_MapSampler   s2
    Diffuse_MapSampler  s3    <- TEXCOORD5             (dcl_texcoord5_pp v5)
    Diffuse_Color_a/b/c c1/c2/c3      Diffuse_Color c14
    ClampU1 c12   ClampV1 c13

and its albedo is fetched as:

    mov_pp  r1.xz, c6                 ; c6.x = 1.0
    add_pp  r0.w, r1.x, -c13.x        ; 1 - ClampV1
    mul_pp  r2.w, r0.w, c9.y          ; * 512
    mad_pp  r0.w, r0.w, c9.y, c9.z    ; * 512 + 1      -> window [-(1-V1)*512, (1-V1)*512 + 1]
    max/min                           ; pin v5.y into it
    abs_pp  r0.w, c13.x
    cmp_pp  r3.y, -r0.w, v5.y, r2.w   ; ClampV1 == 0 -> take v5.y UNCLAMPED
    (same window for U, with no cmp escape)
    texld_pp r3, r3, s3               ; Diffuse_Map at v5 - which at runtime is NOT clamped
    mul_pp   r4.xyz, r3, c14     ; * Diffuse_Color
    texld_pp r6, v1, s0          ; Pattern_Map at TEXCOORD1
    mad/lrp/lrp                  ; the customisation lerp chain
    mul_pp   r4.xyz, r4, r5      ; and the two multiply

**The clamp is declared but not active.** Measured 2026-09-09 on the player's clothing:
`ClampU1 0.00000  ClampV1 0.00000`, U2/V2 not declared. `ClampV1 == 0` sends the `cmp` down the
unclamped branch; `ClampU1 == 0` opens the U window to `[-512, 513]`, which nothing leaves. The
coordinate reaches the sampler untouched and the sampler is WRAP (`INHERITED sampler0:
addressU=1`). The game wraps exactly as the conversion path does. An earlier version of this
file said "CLAMPED" and a padding pass was built on that reading of the register names.

**And in `ir_sr3pccloth_c` vs[0], TEXCOORD5 is TEXCOORD0:** `mul o6.xy, c1.x, v1` with
`c1.x = 1/1024` and no tiling. So on that permutation the albedo IS sampled at
`TEXCOORD0 * 1/1024`, which is precisely the matrix the fixed-function conversion applies.

**Whether the mesh lands on the islands is a per-draw fact, measured by the COVERAGE probe**
(wrap every vertex's `TEXCOORD0 * 1/1024` the way the sampler does, look up the texel):

    256x256 garment    0.3% of vertices on a black texel    <- TEXCOORD0 IS its albedo unwrap
    underwear 256x128 65.7%                                  <- it is NOT
    bra       128x256 63.6%                                  <- it is NOT

The overlays (`sr3-remix-hard-N-coverage.dds`, red = a vertex reached this texel) show the two
broken garments' vertices sprinkled evenly across the whole map - the signature of wrapping the
wrong unwrap over a texture, not of a lost clamp. Their diffuse coordinate comes from another
element of the vertex, i.e. the same pixel shader is paired with a different vertex-shader
permutation. The SCAN probe scores every short2 the stride can hold and names the one that lands
on the islands; BOUND SHADERS gives the vertex shader's bytecode size, which is unique across the
cloth family's permutations (`_c` 2032/1784/2340, `_s` 1364/1116/1688, `_bs` 1296/1044/1636,
`_mc` 2132/1884/2440) and says which file to disassemble.


## The pattern's coordinate, and how the two sets are related (2026-09-10)

`ir_sr3pccloth_c` vs[2] (the permutation bound on the player's clothing, 2340 bytes):

    mul r2.x, c6.x, v4.x        ; Pattern_Map_TilingU * TEXCOORD1.x
    mul r2.y, c7.x, v4.y        ; Pattern_Map_TilingV * TEXCOORD1.y
    mul o2.xy, r2, c1.x         ; * 1/1024   -> ps TEXCOORD1, the Pattern_Map coordinate
    mul o6.xy, c1.x, v1         ; TEXCOORD0 * 1/1024 -> ps TEXCOORD5, the Diffuse_Map coordinate

Tiling was (1.0, 1.0) on every garment measured. Both coordinates wrap.

Whether uv1 is a function of uv0 is a per-draw fact (`UV1 vs UV0 (REFERENCED ONLY)` in the log,
measured on the vertices the index buffer names): identity on the backpack, unrelated on the bra
and underwear. When unrelated, the MESH relates them: every triangle carries both, so
rasterising it in uv0 space with uv1 interpolated gives the pattern texel for each albedo texel
(`BakeDecal`). A texel claimed by two surfaces with different answers - the bra's cups - is a
conflict; conflicting triangles are separated into tiles (see YOUR-INSTRUCTIONS).
