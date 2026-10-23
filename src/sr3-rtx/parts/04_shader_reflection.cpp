// parts/04_shader_reflection.cpp - CTAB walking: what each vertex/pixel shader reads.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 1351-2018 (old file:line citations map here).
// ---------------------------------------------------------------- shader reflection

// Saints Row uses "inferred lighting": each light is a volume draw whose pixel shader carries
// the light's parameters in its constants. Recognised by the constant names in the embedded
// CTAB, then re-emitted as a D3D9 light for Remix to convert into a real ray-traced light.
struct LightShader {
    enum Kind { NotLight, Point, Spot, Directional } kind = NotLight;
    int regPos = -1, regDir = -1, regColor = -1, regInfo = -1, regSpot = -1;
};

struct ShaderInfo {
    LightShader light;
    int albedoStage = -1;    // best colour-carrying sampler
    int albedoRank = 0;      // 0 => this shader binds no colour texture at all
    bool usesProjTM = false; // VS: false implies the shader emits clip space itself
    bool usesObjTM = false;  // VS: false implies the geometry is already in world space
    bool skinned = false;    // VS: has a Bone_weights palette
    // WHERE that palette lives, taken from the shader's own constant table rather than assumed.
    // The whole shim has hardcoded c52 since session 1, on the strength of ONE character
    // shader's CTAB. A vehicle shader is free to declare Bone_weights anywhere, and if it does,
    // every car part has been skinned by whatever happened to sit at c52 - which is a character.
    // -1 means this shader declares no palette.
    int boneReg = -1;
    // THE UV DIVIDE THIS SHADER APPLIES, read from its own def constants.
    //
    // kShortUVScale has been a hardcoded 1/1024 since session 1, on the strength of one shader
    // ending `mul o1.xy, r0, 0.0009765625`. The 2026-09-06 vertex audit shows that is not
    // universal: nine captured parts land inside the unit square, and the underwear spans
    // u[-1.072 2.787] - a range of 3.859 where its siblings span 0.98. Almost exactly four times.
    // A mesh whose shader divides by 4096, read at 1/1024, lands exactly there.
    //
    // 0 means this shader defs no such constant and the caller should keep its old default, so a
    // shader we cannot read behaves exactly as it does today.
    float uvDefScale = 0.0f;
    bool uvDefAmbiguous = false;   // more than one distinct candidate: do not guess between them
    // Does this vertex shader actually CONSUME blend weights? Read from its dcl instructions,
    // not from the vertex declaration - the two disagree, and that disagreement is the bug.
    //
    // Disassembled 2026-08-25. ir_sr3cardiffusespec_g_v shader[0], a vehicle:
    //
    //     dcl_position v0 / dcl_normal v1 / dcl_blendindices v2      <- no dcl_blendweight
    //     mul r2.x, c0.z, v2.x        (c0.z = 3, three registers per bone)
    //     mova a0.x, r2.x
    //     dp4 r0.x, c52[a0.x], r1     <- ONE bone, unweighted
    //     dp4 r1.x, c32, r0           <- then objTM
    //
    // ir_sr3npcclothfull_c shader[0], a character:
    //
    //     dcl_blendweight v4 / dcl_blendindices v5
    //     mul r2, v4.y, c52[a0.x]
    //     mad r2, v4.x, c52[a0.y], r2 ... four weighted bones
    //
    // The vertex DECLARATION on a vehicle draw still carries BLENDWEIGHT data, so deciding from
    // it makes us blend four bones where the game blends one. The other three indices are
    // whatever those bytes happen to hold, and they address a palette that belongs to somebody
    // else - so a rigid part is dragged bodily toward a character and follows it. Every vertex
    // shares the indices, which is why the mesh translates without deforming.
    bool usesBlendWeights = false;
    // Does this vertex shader consume BLENDINDICES at all?
    //
    // This is the question that decides whether a draw is skinned, and it is NOT the same as
    // "the vertex declaration carries BLENDINDICES". Disassembled 2026-08-26, the car body and
    // car glass ship in two variants over the SAME mesh:
    //
    //   ir_sr3cardiffusespec_g_v   dcl_position v0 / dcl_normal v1 / dcl_blendindices v2
    //                              mul r2.x, c0.z, v2.x / mova a0.x, r2.x
    //                              dp4 r0.x, c52[a0.x], r1      <- bone, THEN objTM
    //
    //   ir_sr3cardiffusespec_g_s   dcl_position v0 / dcl_normal v1     <- no blendindices
    //                              dp4 r1.x, c32, r0            <- objTM alone
    //
    // The static variant does not read a palette, does not declare one, and places the mesh by
    // objTM only. The declaration is shared with the vehicle variant, so it still carries
    // BLENDINDICES - which is all the shim looked at. boneReg is then -1, the code below fell
    // back to c52, and the part was posed by whatever palette the last CHARACTER draw left
    // there. That is a detached bumper following an NPC's skeleton while its physics body sits
    // exactly where it belongs, which is the reported bug verbatim.
    //
    // Read from the dcl stream, not from the constant table: a shader that declares no input
    // cannot index a palette no matter what CTAB parsing does or fails to do.
    bool usesBlendIndices = false;
    // Does this vertex shader read the MORPH stream - dcl_position1 / dcl_normal1?
    //
    // SR3 ships `_c` and `_mc` variants of every character shader. Disassembled 2026-08-27,
    // ir_sr3npcskinfull_mc shader[0]:
    //
    //     def c0, 0.000122070313, 2, -1, 3        (c0.x = 1/8192)
    //     dcl_position v0 / dcl_position1 v4 / dcl_normal v2 / dcl_normal1 v5
    //     mov r1.xyz, v4
    //     mad r1.xyz, r1, c0.x, v0        <- position = v0 + v4/8192, BEFORE the bone blend
    //     dp4 r2.x, r2, r1                <- then the bones, then objTM
    //
    // That is character customisation: the face and body are authored as a base mesh plus a
    // per-character delta in a second stream. Reading only v0 renders every NPC with the SAME
    // base head - a head that is not theirs, differs from NPC to NPC, sits slightly off the
    // neck, and no longer matches the surface its texture was authored for.
    bool usesMorph = false;
    // How many RENDER TARGETS this pixel shader writes.
    //
    // SR3 is a deferred renderer and draws each object twice. Disassembled 2026-08-28,
    // ir_sr3npcskinfull_mc - the head:
    //
    //   shader[6]  oC0.xy = normal, oC0.zw = const, oC1 = data, oC2 = data
    //              samples Blend_Map (ONE scalar, into a fresnel term) + Normal_Map
    //              -> writes NO colour at all
    //   shader[8]  oC0 = r2 * c37 (Tint_color)
    //              samples Diffuse_Map, Sphere_Map, IR_LBuffer
    //              -> the actual material pass
    //
    // The albedo ranker scores Blend_Map at 70 and hands the G-buffer pass a specular mask as its
    // base colour, which is a correctly placed head wearing the wrong texture. The world's
    // G-buffer shaders do the same thing and even sample Diffuse_Map first - then overwrite the
    // result with the normal map before using it, so that load is dead.
    //
    // 1,343 of this game's pixel shaders write three targets; 1,880 write one.
    unsigned rtCount = 0;
    bool isPixelShader = false;
    bool anySampler = false;
    // The sampler that WON the albedo ranking, not merely the first one declared. The tiling
    // pair that applies is the one named after this map.
    char albedoSampler[28] = {};
    // Every sampler by stage. firstSampler names one of them and albedoSampler names the winner;
    // neither answers "what else is bound and where", which is the question when the colour is in
    // a texture this shim is not choosing.
    char samplerName[8][20] = {};
    // Per-material texture tiling. Disassembly of the world shaders (2026-08-16) shows the
    // primary UV built as:
    //     mul r0.x, c<TilingU>.x, v1.x
    //     mul r0.y, c<TilingV>.x, v1.y
    //     mul o1.xy, r0, 0.0009765625      ; 1/1024
    // so uv = raw * tiling / 1024. The 1/1024 is a literal, but the tiling factors are
    // uniforms whose REGISTERS DIFFER BETWEEN SHADERS (c1/c2 in ir_bbsimple2_decal_s, c2/c3 in
    // ir_bbsimple_1uv_decal_s), so they have to be read from each shader's constant table
    // rather than assumed. Shaders with no tiling constants scale by 1/1024 alone.
    // EVERY pair is kept, with the name of the map it belongs to, because the choice cannot be
    // made here: the tiling constants live in the VERTEX shader and the albedo sampler that
    // decides which pair applies is in the PIXEL shader. Resolved per draw by TilingForAlbedo.
    struct TilingPair {
        char base[24];      // the map name, e.g. "Normal_Map" out of "Normal_Map_TilingU"
        int uReg;
        int vReg;
    };
    static constexpr int kMaxTilingPairs = 4;
    TilingPair tiling[kMaxTilingPairs]{};
    int tilingCount = 0;
    // The inferred-lighting stipple pattern. Its presence in a shader with NO colour sampler is
    // the signature of SR3's DSF/normal prepass - confirmed by disassembly, not inferred:
    // ir_bb_tod_window_bs.fxo_pc shader [6] samples Normal_Map (s0) and IR_Stipple_Pattern_2D
    // (s11), decodes a tangent-space normal and writes specular power, with no colour anywhere;
    // shader [8] of the SAME FILE is the material pass and carries Diffuse_Map, Decal_Map,
    // Specular_Map, IR_LBuffer and Tint_color. 415 pixel shaders across the game match [6]'s
    // shape (236 stipple alone, 179 stipple + Normal_Map).
    // SR3's sky IS geometry, drawn by the rfg-skybox family inherited from Red Faction
    // Guerrilla: rfg-skybox_s, -clouds_s, -clouds-2_s, -matte_s, -overhead_s, -simple_s, -stars,
    // -meteors. Most carry Diffuse_Map or Decal_Map and would convert normally, so "the sky is
    // black" is not explained by our classification and needs to be observed rather than argued.
    // Recognised by constants unique to that family.
    bool skyShader = false;
    bool hasStipple = false;
    // Every sampler this shader declares is a PREPASS utility - a stipple pattern, a normal map,
    // a depth or shadow map. Never surface colour, and never the inferred-lighting inputs a real
    // material samples. Measured 2026-08-18: all 83 such draws in a steady-state frame went to
    // the G16R16 prepass target and every one shared its exact vertex/primitive counts with a
    // properly coloured converted draw, i.e. each is a duplicate of a mesh we already convert.
    bool prepassSamplersOnly = false;
    bool samplesLBuffer = false;  // reads IR_LBuffer: the mark of a pass that produces colour
    bool samplesGBufferNormals = false;  // reads the G-buffer normals: a screen-space pass
    bool samplesParticleDepth = false;   // reads Depth_buffer: an rl_particle_* billboard
    int alphaThresholdReg = -1;  // Alpha_Threshold: the shader does its own cutout with texkill
    // Materials with NO diffuse map by design, whose albedo is a CONSTANT. The shader file names
    // say it outright - ir_bbsimple2_nodiffmap - and the disassembly ends with
    //     mul_pp oC0, r1, c37        ; r1 = lighting, c37 = Tint_color
    // so the constant IS the base colour, not a modulation on top of one. 117 of the 121 shaders
    // that sample IR_GBuffer_DSF_Data without a colour map carry Tint_color.
    //
    // This answers the question YOUR-INSTRUCTIONS carried for weeks as "where do those materials
    // get their colour?", and it is why they must NOT be hidden: unlike the normal prepass they
    // are unique geometry in the material pass - zero of the 150 shared a shape with any other
    // converted draw - so hiding them would punch holes in the world.
    int colourConstReg = -1;
    int colourConstRank = 0;
    // SR3's customisable clothing builds its base colour PER TEXEL from a mask texture and three
    // constants, which is why no texture-stage arrangement has ever produced correct clothing
    // colour. Read from ir_sr3npcclothfull_c.fxo_pc shader [8] rather than inferred:
    //
    //     texld r3, v0, s0                  ; Pattern_Map
    //     sum  = p.r + p.g + p.b
    //     dev  = |p.r-sum/3| + |p.g-sum/3| + |p.b-sum/3|
    //     test = sum - (dev*165.016495 + 256)/255
    //     test <  0 -> albedo = p.r^2.2*Diffuse_Color_a + p.g^2.2*Diffuse_Color_b
    //                                                   + p.b^2.2*Diffuse_Color_c
    //     test >= 0 -> albedo = saturate((p - 0.372549) * 1.59375) ^ 2.2
    //     mul oC0, r1, c37                  ; and the whole result * Tint_color
    //
    // So the three channels are not simple masks: they are gamma-2.2 weights, and a SELECTOR
    // switches whole texels to a desaturated branch - which is how trim and skin escape being
    // tinted by the three customisation colours. 34 shader entries across 19 files carry the
    // full set, the whole ir_*sr3pccloth* / ir_*sr3npccloth* family, player and NPC alike.
    char colourConstName[24] = {};        // which constant won the colour ranking
    // WHICH TEXCOORD FEEDS EACH SAMPLER, read from the shader's own texld instructions.
    //
    // This shim has assumed since the fork that the albedo is sampled with TEXCOORD0 and the
    // Pattern_Map with TEXCOORD1. Disassembling the player's cloth shaders shows that is not a
    // rule, it is a coincidence that holds for some variants:
    //
    //   ir_sr3pccloth_bs   [6]   texld r6, v1, s0     s0 <- TEXCOORD1
    //   ir_at_sr3pccloth_bs[8]   texld r5, v1, s2     s2 <- TEXCOORD1   (the pattern)
    //                            texld r4, r4, s0     s0 <- a COMPUTED coordinate built from
    //                                                       TEXCOORD6 and clamped against c2/c3
    //
    // So a garment's albedo can arrive through TEXCOORD6, and the CPU baker has been rasterising
    // into TEXCOORD0 space regardless - which is exactly why its islands need not land where the
    // mesh samples them. -1 means the coordinate is computed rather than taken straight from an
    // input register, and that is worth knowing too: a computed coordinate cannot be reproduced
    // by resampling and says so instead of being guessed at.
    int samplerUv[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    // true where the coordinate was a plain input register, false where it was COMPUTED and the
    // index below was traced back through the temporaries. The distinction is worth keeping: a
    // traced answer is an inference from a dataflow walk, not a direct reading, and a caller may
    // reasonably want to treat the two differently.
    bool samplerUvDirect[8] = {false, false, false, false, false, false, false, false};
    bool sawTexld = false;
    int patternStage = -1;                // the Pattern_Map sampler
    int diffuseColorReg[3] = {-1, -1, -1};   // Diffuse_Color_a, _b, _c
    // Diffuse_Color, a SEPARATE uniform (c11 in the player cloth shaders) that scales the diffuse
    // map before the customisation colours are applied. Not one of the three above.
    int diffuseColorMulReg = -1;
    // THE CLAMP REGISTERS the albedo's coordinate passes through - and what they actually do.
    //
    // ir_sr3pccloth_c vs[0]:   def c1, 0.0009765625, ...      (exactly 1/1024)
    //                          mul o6.xy, c1.x, v1            (v1 = the TEXCOORD0 attr, no tiling)
    //                          dcl_texcoord5 o6               (so o6 IS ps TEXCOORD5)
    // ir_sr3pccloth_c ps[6]:   mov r1.xz, c6                  (c6.x = 1.0)
    //                          add r0.w, r1.x, -c13.x         ; 1 - ClampV1
    //                          mul r2.w, r0.w, c9.y           ; c9.y = 512
    //                          mad r0.w, r0.w, c9.y, c9.z     ; c9.z = 1
    //                          max/min                        ; window [-(1-V1)*512, (1-V1)*512+1]
    //                          abs r0.w, c13.x
    //                          cmp r3.y, -r0.w, v5.y, r2.w    ; ClampV1 == 0 -> v5.y UNCLAMPED
    //                          (U has the same window and NO cmp escape)
    //                          texld r3, r3, s3               (the Diffuse_Map)
    //
    // Measured at runtime on the player's clothing (2026-09-09): ClampU1 = ClampV1 = 0.00000,
    // U2/V2 not declared. So V is passed through untouched and U is pinned to [-512, 513], which
    // nothing leaves. THE CLAMP IS NOT ACTIVE. The coordinate goes to the sampler as-is, and the
    // sampler is WRAP (INHERITED probe: addressU=1). The game wraps exactly as the conversion does.
    //
    // An earlier version of this comment said the opposite - "the game CLAMPS, the baker WRAPS,
    // these four registers are what it should have used" - and a whole padding pass was built on
    // it. It read the register NAMES and never decoded the arithmetic or the runtime values.
    //
    // What IS confirmed: kShortUVScale = 1/1024, and TEXCOORD0 * 1/1024 is the albedo coordinate
    // for the permutation above - the 256x256 garment lands 0.3% of its vertices on black
    // (COVERAGE probe). What is ALSO measured: the underwear lands 65.7% and the bra 63.6% of
    // theirs on black through the same coordinate, with the vertices sprinkled evenly over the
    // whole map. On those two draws TEXCOORD0 is not the albedo unwrap; the SCAN probe reports
    // which short2 in the vertex is, and BOUND SHADERS names the permutation to disassemble.
    int clampReg[4] = {-1, -1, -1, -1};   // ClampU1, ClampV1, ClampU2, ClampV2
    int tintColorReg = -1;
    // Hair renders white and neither of its textures holds the colour: the Dob_Map is white
    // strands on a green field and the Diffuse_Map is smooth directional data. ir_sr3pchair_c
    // multiplies its result by Hair_Spec_Color2 (`mul_pp r0.xyz, r0, c4`), so despite the name
    // these are the likeliest carriers of the chosen hair colour.
    int hairColorReg[2] = {-1, -1};
    // First sampler this shader declares, kept only so a blanked draw can say WHAT it sampled.
    // Over half of converted draws currently render untextured and the reason has been inferred
    // rather than observed.
    char firstSampler[28] = {};
    // D3DX writes the CTAB constant table sorted ALPHABETICALLY by name, never by register, so
    // firstSampler above is only whichever sampler name happens to sort first - not the shader's
    // primary texture. Measured on ir_bbstandard_bs: s1 Diffuse_Map, s2 Specular_Map, s3
    // Decal_Map, s4 Decal_Map_2, yet firstSampler comes out "Decal_MapSampler" because "Decal"
    // sorts before "Diffuse". lowestSampler instead names whichever sampler is bound to the
    // LOWEST register number - the one the shader actually samples first when it runs.
    char lowestSampler[28] = {};
    int lowestSamplerReg = 9999;   // sentinel, lowered as the CTAB walk finds smaller registers
    // Decal-named tests, computed ONCE here instead of by StrStrIA(...,"Decal") on every
    // converted draw. Three call sites ran that search per draw over these exact three fixed
    // ShaderInfo strings (SetupTextureStages's decalAlphaBlend on albedoSampler, and the
    // decalByLowestReg/decalByFirstAlpha pair in BeginFFP on lowestSampler/firstSampler) -
    // ~0.09 ms/frame of recomputing an answer that cannot change for the shader's lifetime.
    // isDecalAlbedo is the third one: the task that named isDecalLowest/isDecalFirst only
    // listed two fields, but SetupTextureStages's albedoSampler test is a distinct string from
    // both of those and needs its own cached answer to actually remove all three searches.
    bool isDecalLowest = false;
    bool isDecalFirst = false;
    bool isDecalAlbedo = false;
    // FIX A: does this VERTEX shader's own input declaration carry a usage-index-0 TEXCOORD at
    // all, and if not, what is the LOWEST texcoord usage index it does declare? Read from the
    // shader's own dcl_texcoordN INPUT instructions in ReflectShader - never from a pixel
    // shader's dcl_texcoordN, which names an INTERPOLATOR input (the vertex shader's output),
    // a completely different thing. See DecalUvSourceSet for what these decide.
    bool declaresTexcoord0 = false;
    int lowestDeclaredTexcoord = 99;   // sentinel: no texcoordN input declared at all
};

// Keyed on the shader's own address, and every entry holds a reference.
//
// Without that reference this is the 2026-08-18 use-after-free again, in its quietest form: a
// released shader's address can be handed straight back to a newly created one, and every lookup
// then answers with the PREVIOUS shader's reflection - wrong sampler names, wrong albedo rank,
// wrong skinned flag. It does not crash; it silently mis-classifies draws, which is far harder to
// find than a crash and matches the shape of "surfaces are the wrong colour".
//
// Nothing is ever erased, so the reference also pins the shader for the process lifetime. SR3 has
// 7,276 shaders in total, so the ceiling is bounded and small.
std::unordered_map<void*, ShaderInfo> g_shaders;

size_t ShaderLength(const DWORD* tokens) {
    for (size_t i = 1; i < 65536; ++i) {
        if (tokens[i] == 0x0000FFFF) return (i + 1) * sizeof(DWORD);
        if ((tokens[i] & 0xFFFF) == 0xFFFE) i += (tokens[i] >> 16) & 0x7FFF;
    }
    return 0;
}

// Which sampler carries base colour? Measured across every pixel shader in the game: stage 0
// holds Diffuse_Map 804 times but a NORMAL map 903 times (Normal_Map 415 + Damage_Normal_Map
// 488). Remix reads stage 0 as albedo, so those ~900 materials were being shaded with a
// tangent-space normal map - the green/orange "yellow geometry" the user identified. Many have
// no Diffuse sampler at all, so matching the name "Diffuse" alone finds nothing. Rank every
// sampler and take the best; rank 0 means no colour texture exists, and binding nothing
// (white) still beats binding a normal map.
int AlbedoRank(const char* name) {
    if (_strnicmp(name, "Diffuse", 7) == 0) return 100;
    // "Diffuse" is not always the prefix - Decal_diffuse_mapSampler and friends were scoring 0
    // and having their stage 0 blanked despite naming a diffuse map outright.
    if (StrStrIA(name, "diffuse")) return 95;
    if (StrStrIA(name, "Decal_Map")) return 90;
    if (StrStrIA(name, "Pattern_Map")) return 80;
    if (StrStrIA(name, "Blend_Map")) return 70;
    if (StrStrIA(name, "Grime") || StrStrIA(name, "Dirt_Rust")) return 60;
    if (StrStrIA(name, "base_sampler")) return 50;
    // Sky cloud layers. rfg-skybox-clouds-2_s samples Layer01_map + Layer23_map and nothing
    // else, so without this it scores 0 and the sky renders blank.
    if (StrStrIA(name, "Layer01_map") || StrStrIA(name, "Layer23_map")) return 45;
    if (StrStrIA(name, "Illumination") || StrStrIA(name, "Glow")) return 20;
    return 0;
}

ShaderInfo ReflectShader(const DWORD* tokens) {
    ShaderInfo result;
    LightShader& info = result.light;
    const size_t length = ShaderLength(tokens);
    if (!length) return result;

    const char* bytes = reinterpret_cast<const char*>(tokens);
    // D3DVS_VERSION packs 0xFFFE into the version token's top 16 bits, D3DPS_VERSION packs
    // 0xFFFF. Needed because THIS SAME FUNCTION reflects both kinds of shader -
    // Hook_CreatePixelShader only sets isPixelShader on the result AFTER calling it - and a
    // pixel shader's own dcl_texcoordN declares its INTERPOLATOR inputs (what the vertex shader
    // already output), not a vertex attribute. declaresTexcoord0 / lowestDeclaredTexcoord below
    // must only ever be filled from a genuine vertex shader's input declarations.
    const bool isVertexShader = (tokens[0] >> 16) == 0xFFFE;
    // Scan the instruction stream for dcl_blendweight on an INPUT register. Opcode 0x1F is DCL;
    // its first source token carries the D3DDECLUSAGE in the low five bits (BLENDWEIGHT == 1) and
    // its second is the destination register. Length lives in bits 24-27 for SM2+, so the walk
    // steps instruction by instruction rather than guessing.
    {
        const DWORD* t = tokens + 1;                       // skip the version token
        const DWORD* end = tokens + length / sizeof(DWORD);
        unsigned rtMask = 0;
        // WHICH INPUT REGISTER FEEDS EACH TEMPORARY.
        //
        // The underwear's shader does not sample its albedo from a texcoord directly:
        //
        //     ir_sr3pccloth_bs[6]:  ... clamp v5.xy against ClampU1/ClampV1 into r3 ...
        //                           texld_pp r3, r3, s3      ; Diffuse_Map
        //                           texld_pp r6, v1, s0      ; Pattern_Map on TEXCOORD1
        //
        // so reading only `texld` sources reports "computed" and says nothing. One backward step
        // is enough to recover it: carry, for every temporary, the input register that last fed
        // it. A clamp chain is a handful of moves and the taint survives all of them.
        //
        // This is deliberately simple - first input wins, and a temporary written from another
        // temporary inherits its origin. It cannot express a coordinate genuinely built from TWO
        // texcoords, and it does not pretend to: such a case would report whichever it met first,
        // which is why samplerUvDirect records that the answer was traced rather than read.
        int tempFrom[32];
        for (int i = 0; i < 32; ++i) tempFrom[i] = -1;
        while (t < end && *t != 0x0000FFFF) {               // 0xFFFF is END
            const DWORD op = *t & 0xFFFF;
            const DWORD len = (*t & 0x0F000000) >> 24;
            if (op == 0x1F && t + 2 < end) {                // D3DSIO_DCL
                const DWORD usage = t[1] & 0x1F;
                const DWORD regType = ((t[2] & 0x70000000) >> 28) | ((t[2] & 0x00001800) >> 8);
                if (usage == 1 /* D3DDECLUSAGE_BLENDWEIGHT */ && regType == 1 /* INPUT */)
                    result.usesBlendWeights = true;
                if (usage == 2 /* D3DDECLUSAGE_BLENDINDICES */ && regType == 1 /* INPUT */)
                    result.usesBlendIndices = true;
                // POSITION with usage index 1. The index lives in bits 16-19 of the same
                // token as the usage; index 2/3/4 are the instance transform, not a morph.
                if (usage == 0 /* D3DDECLUSAGE_POSITION */ && ((t[1] >> 16) & 0xF) == 1 &&
                    regType == 1 /* INPUT */)
                    result.usesMorph = true;
                // TEXCOORD input declarations, VERTEX SHADER ONLY (see isVertexShader above).
                // ir_at_sr3decalonly_{s,bs} VS 0-4, ir_sr3diffcol_normal_decal_{s,bs} VS 2-5,
                // ir_at_decalonly_cuberef_* VS 5 and ir_sr3fauxinterior_{s,bs} VS 0-1 build their
                // decal coordinate from TEXCOORD1 and declare NO usage-index-0 input at all - this
                // is exactly what lets DecalUvSourceSet notice them instead of blindly sampling
                // whatever index 0 happens to hold.
                if (usage == 5 /* D3DDECLUSAGE_TEXCOORD */ && regType == 1 /* INPUT */ &&
                    isVertexShader) {
                    const int idx = static_cast<int>((t[1] >> 16) & 0xF);
                    if (idx == 0) result.declaresTexcoord0 = true;
                    if (idx < result.lowestDeclaredTexcoord) result.lowestDeclaredTexcoord = idx;
                }
            }
            // D3DSIO_TEX / texld (0x42) in ps_3_0: dst, source coordinate, sampler.
            //
            // The coordinate's register type lives in the same split field every register uses -
            // bits 28-30 with bits 11-12 - and type 1 is D3DSPR_INPUT, the vN registers that
            // dcl_texcoordN declares. The sampler's index is the low bits of the third token.
            // A coordinate that is anything other than an input register was computed by the
            // shader and is recorded as -1.
            if (op == 0x42 && len >= 3 && t + 3 < end) {
                const DWORD srcTok = t[2], smpTok = t[3];
                const DWORD srcType = ((srcTok & 0x70000000) >> 28) | ((srcTok & 0x00001800) >> 8);
                const DWORD smpType = ((smpTok & 0x70000000) >> 28) | ((smpTok & 0x00001800) >> 8);
                const unsigned smp = smpTok & 0x7FF;
                const unsigned srcIdx = srcTok & 0x7FF;
                if (smpType == 10 /* D3DSPR_SAMPLER */ && smp < 8) {
                    result.sawTexld = true;
                    if (srcType == 1 /* D3DSPR_INPUT */) {
                        result.samplerUv[smp] = static_cast<int>(srcIdx);
                        result.samplerUvDirect[smp] = true;
                    } else if (srcType == 0 /* D3DSPR_TEMP */ && srcIdx < 32) {
                        result.samplerUv[smp] = tempFrom[srcIdx];   // -1 if nothing traced
                        result.samplerUvDirect[smp] = false;
                    } else {
                        result.samplerUv[smp] = -1;
                        result.samplerUvDirect[smp] = false;
                    }
                }
            }
            // Propagate the taint. AFTER the texld above has read its source, because a texld
            // writes a temporary too and would otherwise overwrite the origin it was about to be
            // asked for.
            if (op != 0x1F && op != 0x51 && len >= 1 && t + len < end) {
                const DWORD dTok = t[1];
                const DWORD dType = ((dTok & 0x70000000) >> 28) | ((dTok & 0x00001800) >> 8);
                int origin = -1;
                for (DWORD k = 2; k <= len && origin < 0; ++k) {
                    const DWORD sTok = t[k];
                    const DWORD sType = ((sTok & 0x70000000) >> 28) | ((sTok & 0x00001800) >> 8);
                    const unsigned sIdx = sTok & 0x7FF;
                    if (sType == 1 /* input */) origin = static_cast<int>(sIdx);
                    else if (sType == 0 /* temp */ && sIdx < 32) origin = tempFrom[sIdx];
                }
                if (dType == 0 /* temp */ && (dTok & 0x7FF) < 32)
                    tempFrom[dTok & 0x7FF] = origin;
            }
            // D3DSIO_DEF (0x51): `def cN, x, y, z, w`. One destination token, then four floats.
            //
            // A UV divide is a reciprocal power of two applied to a .xy write, so that is exactly
            // what is accepted here - between 1/256 and 1/16384, and a power of two to within a
            // rounding error. That is narrow enough that ordinary shader constants do not qualify,
            // and it is checked rather than assumed: if a shader defs two DIFFERENT such values
            // there is no way to tell which one scales the texcoords without following the
            // dataflow, so the result is marked ambiguous and the caller keeps its old default.
            // Guessing between them is how this project has lost days before.
            if (op == 0x51 && t + 5 < end) {
                float v[4];
                memcpy(v, &t[2], sizeof(v));
                for (int c = 0; c < 2; ++c) {          // x and y; a uv divide writes .xy
                    if (!(v[c] > 0.0f) || v[c] > 1.0f / 256.0f) continue;
                    const double inv = 1.0 / static_cast<double>(v[c]);
                    const unsigned n = static_cast<unsigned>(inv + 0.5);
                    if (n < 256u || n > 16384u) continue;
                    if (n & (n - 1u)) continue;                       // not a power of two
                    if (inv - n > 0.01 || n - inv > 0.01) continue;   // and exactly one
                    if (result.uvDefScale == 0.0f) result.uvDefScale = v[c];
                    else if (result.uvDefScale != v[c]) result.uvDefAmbiguous = true;
                }
            }
            // Count distinct COLOROUT destinations. The destination is the first token after the
            // opcode; register type 8 is D3DSPR_COLOROUT, split across bits 28-30 and 11-12 the
            // same way every other register type is.
            if (op != 0x1F && op != 0x51 && len >= 1 && t + 1 < end) {
                const DWORD d = t[1];
                const DWORD dt = ((d & 0x70000000) >> 28) | ((d & 0x00001800) >> 8);
                if (dt == 8) rtMask |= 1u << (d & 3);
            }
            // A comment block - CTAB is one, and it sits immediately after the version token -
            // carries its length in bits 16-30, NOT in the 24-27 field the instructions use.
            // Reading it from the wrong field walks into the middle of the constant table and
            // the dcl instructions are never reached at all.
            if (op == 0xFFFE) {
                // Capped against `end`. A corrupt length would otherwise walk the pointer far
                // past the buffer before the loop condition caught it.
                const DWORD skip = 1 + ((*t >> 16) & 0x7FFF);
                if (static_cast<size_t>(end - t) <= skip) break;
                t += skip;
                continue;
            }
            t += 1 + len;
            if (!len) break;                                // malformed; stop rather than spin
        }
        for (unsigned b = 0; b < 4; ++b) if (rtMask & (1u << b)) ++result.rtCount;
    }

    const char* ctab = nullptr;
    for (size_t i = 0; i + 4 <= length; ++i)
        if (memcmp(bytes + i, "CTAB", 4) == 0) { ctab = bytes + i; break; }
    if (!ctab) return result;

    const char* base = ctab + 4;
    const DWORD size = *reinterpret_cast<const DWORD*>(base);
    const DWORD count = *reinterpret_cast<const DWORD*>(base + 12);
    const DWORD infoOffset = *reinterpret_cast<const DWORD*>(base + 16);
    if (size != 28 || count > 256) return result;

    for (DWORD i = 0; i < count; ++i) {
        const char* entry = base + infoOffset + i * 20;
        if (entry + 20 > bytes + length) break;
        const DWORD nameOffset = *reinterpret_cast<const DWORD*>(entry);
        const WORD registerSet = *reinterpret_cast<const WORD*>(entry + 4);
        const WORD reg = *reinterpret_cast<const WORD*>(entry + 6);
        const char* name = base + nameOffset;
        if (name < bytes || name >= bytes + length) continue;

        if (StrStrIA(name, "Cloud_Fade_Height") || StrStrIA(name, "Layer_strengths") ||
            StrStrIA(name, "TOD_Light_Dir") || StrStrIA(name, "Layer01_map") ||
            StrStrIA(name, "Layer23_map") || StrStrIA(name, "Star_strength"))
            result.skyShader = true;

        if (!strcmp(name, "projTM")) result.usesProjTM = true;
        else if (!strcmp(name, "objTM")) result.usesObjTM = true;
        else if (!strcmp(name, "Bone_weights")) {
            result.skinned = true;
            result.boneReg = reg;
        }

        if (registerSet == 3) {   // D3DXRS_SAMPLER
            if (StrStrIA(name, "Stipple")) result.hasStipple = true;
            // Inferred lighting: a pass that produces visible colour reads the light buffer to
            // shade itself. That makes this the property that separates a material from a
            // G-buffer fill, without needing to recognise every sampler name in the game.
            if (StrStrIA(name, "IR_LBuffer")) result.samplesLBuffer = true;
            // Reading the G-buffer's NORMALS means reading the orientation of geometry that is
            // already on screen. Only a screen-space pass needs that - a real surface carries
            // its own normals and has no reason to ask what is behind it.
            if (StrStrIA(name, "IR_GBuffer_Normals")) result.samplesGBufferNormals = true;
            // `Depth_buffer` is the PARTICLE system's soft-fade sampler and nothing else uses
            // that spelling. Water reads `IR_GBuffer_Depth`, projectors read `Depth_map` - the
            // names are distinct, which is what makes this test exact rather than approximate.
            if (StrStrIA(name, "Depth_buffer")) result.samplesParticleDepth = true;
            // Positive identification, not "anything unrecognised". A shader is only treated as
            // a prepass if EVERY sampler it declares is one of these; an unknown sampler name
            // keeps it a material. That is the safeguard the old `albedoRank == 0` prepass test
            // lacked - it hid ~2,400 real draws a frame including 17,601-vertex terrain, and it
            // is recorded as a dead end for exactly that reason.
            const bool utility = StrStrIA(name, "Stipple") || StrStrIA(name, "Normal_Map") ||
                                 StrStrIA(name, "Depth_map") || StrStrIA(name, "shadow_map");
            if (!result.anySampler) result.prepassSamplersOnly = true;   // first sampler seen
            if (!utility) result.prepassSamplersOnly = false;
            result.anySampler = true;
            if (!result.firstSampler[0])
                strncpy_s(result.firstSampler, name, sizeof(result.firstSampler) - 1);
            if (reg < result.lowestSamplerReg) {
                result.lowestSamplerReg = reg;
                strncpy_s(result.lowestSampler, name, sizeof(result.lowestSampler) - 1);
            }
            if (reg >= 0 && reg < 8) strncpy_s(result.samplerName[reg], name, 19);
            if (!_stricmp(name, "Pattern_MapSampler")) result.patternStage = reg;
            const int rank = AlbedoRank(name);
            if (rank > result.albedoRank) {
                result.albedoRank = rank;
                result.albedoStage = reg;
                strncpy_s(result.albedoSampler, name, sizeof(result.albedoSampler) - 1);
            }
            continue;
        }

        // The constant that carries base colour for a no-diffuse-map material. Ranked, because a
        // shader can name several: the specific ones win over the generic Tint_color, which is
        // simply the most common. A shader combining two (albedo * tint) would need real
        // dataflow analysis to resolve; taking the highest-ranked single constant is an
        // approximation, and a flat colour that is close beats white that is certainly wrong.
        // The shader performs its OWN alpha cutout. `ir_at_*` - at for alpha test - foliage,
        // decals, windows and cloth all do this with `texkill` against Alpha_Threshold, and
        // there is no D3D9 alpha-test render state involved at all. Without noticing that, the
        // cutout rule below ("alpha test on AND ref > 0") never fires for them and every leaf
        // card renders as a solid opaque rectangle.
        if (!_stricmp(name, "Alpha_Threshold")) result.alphaThresholdReg = reg;

        if (registerSet == 2) {   // D3DXRS_FLOAT4
            int crank = 0;
            if (!_stricmp(name, "Diffuse_Color")) crank = 100;
            else if (!_stricmp(name, "Base_Paint_Color")) crank = 90;
            else if (!_stricmp(name, "Glass_Color")) crank = 80;
            else if (!_stricmp(name, "Base_Color")) crank = 70;
            else if (!_stricmp(name, "Draw_Color")) crank = 60;
            else if (!_stricmp(name, "Tint_color")) crank = 50;
            if (!_stricmp(name, "ClampU1")) result.clampReg[0] = reg;
            else if (!_stricmp(name, "ClampV1")) result.clampReg[1] = reg;
            else if (!_stricmp(name, "ClampU2")) result.clampReg[2] = reg;
            else if (!_stricmp(name, "ClampV2")) result.clampReg[3] = reg;
            else if (!_stricmp(name, "Diffuse_Color")) result.diffuseColorMulReg = reg;
            else if (!_stricmp(name, "Diffuse_Color_a")) result.diffuseColorReg[0] = reg;
            else if (!_stricmp(name, "Diffuse_Color_b")) result.diffuseColorReg[1] = reg;
            else if (!_stricmp(name, "Diffuse_Color_c")) result.diffuseColorReg[2] = reg;
            else if (!_stricmp(name, "Tint_color")) result.tintColorReg = reg;
            else if (!_stricmp(name, "Hair_Spec_Color1")) result.hairColorReg[0] = reg;
            else if (!_stricmp(name, "Hair_Spec_Color2")) result.hairColorReg[1] = reg;
            if (crank > result.colourConstRank) {
                result.colourConstRank = crank;
                result.colourConstReg = reg;
                // Inside the same test as the register, or the two describe different constants -
                // which is what made the first CHAR CONST report print "Tint_color(c14)".
                strncpy_s(result.colourConstName, name, sizeof(result.colourConstName) - 1);
            }
        }

        // Texture tiling. Several pairs coexist - Normal_Map, Decal_Map, Diffuse, Grime - and
        // each belongs to ONE map. Preferring Normal_Map_Tiling, as this did until 2026-08-20,
        // was measured wrong: it is the pair driving the vertex shader's primary UV output,
        // which feeds the NORMAL map, while the texture this shim binds as albedo is the
        // DIFFUSE map. A detail normal is tiled far more densely than the diffuse it detail-maps,
        // so the diffuse came out correspondingly too dense - the reported "some surfaces like
        // roads tiled too densely". 443 shaders carry a Normal_Map pair and only 44 carry a
        // diffuse one, so most of the world was being scaled by a factor that never applied to it.
        {
            const char* tiling = StrStrIA(name, "_Tiling");
            const bool isU = tiling && (StrStrIA(name, "TilingU") || StrStrIA(name, "tiling_u"));
            const bool isV = tiling && (StrStrIA(name, "TilingV") || StrStrIA(name, "tiling_v"));
            if (isU || isV) {
                char base[24] = {};
                const size_t len = min(static_cast<size_t>(tiling - name), sizeof(base) - 1);
                memcpy(base, name, len);
                int slot = -1;
                for (int t = 0; t < result.tilingCount; ++t)
                    if (!_stricmp(result.tiling[t].base, base)) { slot = t; break; }
                if (slot < 0 && result.tilingCount < ShaderInfo::kMaxTilingPairs) {
                    slot = result.tilingCount++;
                    strncpy_s(result.tiling[slot].base, base, sizeof(base) - 1);
                    result.tiling[slot].uReg = -1;
                    result.tiling[slot].vReg = -1;
                }
                if (slot >= 0) {
                    if (isU) result.tiling[slot].uReg = reg;
                    if (isV) result.tiling[slot].vReg = reg;
                }
            }
        }

        if (!strcmp(name, "IR_Light_Pos")) info.regPos = reg;
        else if (!strcmp(name, "IR_Light_Dir")) info.regDir = reg;
        else if (!strcmp(name, "IR_Light_Color")) info.regColor = reg;
        else if (!strcmp(name, "IR_Light_Info")) info.regInfo = reg;
        else if (!strcmp(name, "IR_Spot_Info")) info.regSpot = reg;
    }

    // Directional lights carry no IR_Light_Info (no distance falloff); spots add IR_Spot_Info.
    if (info.regPos >= 0 && info.regColor >= 0) {
        if (info.regSpot >= 0) info.kind = LightShader::Spot;
        else if (info.regInfo >= 0) info.kind = LightShader::Point;
        else info.kind = LightShader::Directional;
    }
    // Computed once here, after every sampler name in the CTAB has been walked (lowestSampler,
    // firstSampler and albedoSampler above are all only final once the loop is done), rather
    // than re-run by StrStrIA on every converted draw this shader is bound for.
    result.isDecalLowest = result.lowestSampler[0] != '\0' && StrStrIA(result.lowestSampler, "Decal") != nullptr;
    result.isDecalFirst = result.firstSampler[0] != '\0' && StrStrIA(result.firstSampler, "Decal") != nullptr;
    result.isDecalAlbedo = result.albedoSampler[0] != '\0' && StrStrIA(result.albedoSampler, "Decal") != nullptr;
    return result;
}

