// parts/02_settings.cpp - the Settings struct, its defaults, and LoadSettings.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 162-1151 (old file:line citations map here).
// ---------------------------------------------------------------- settings

// SETTLED BEHAVIOUR - measured, no longer configurable.
//
// A toggle is a hypothesis that has not been settled. Once it is settled it belongs in the code
// with the measurement beside it, because a switch nobody varies is a switch nobody re-checks -
// and stale switches interacting is exactly what this fork was created to escape (the pre-fork
// build carried ~20 of them). Four were removed on 2026-08-17 with the evidence recorded here.
//
//   skipDepthOnly   REMOVED - inert. D3DRS_COLORWRITEENABLE == 0 occurs ZERO times in a complete
//                   frame dump, so the test never fired. It also read that state through
//                   ShadowGetRS, which returns 0 on a failed query - meaning the only way it
//                   could ever fire was a false positive that would skip the entire world.
//   finiteFar       REMOVED - inert on SR3. Its projection is near=0.15 far=5000 (Q=1.00003),
//                   a perfectly ordinary finite projection, and the substitution counter read 0
//                   across every run. The mechanism is real on SR2; this engine does not need it.
//   startupDelayMs  REMOVED - always 0.
//   skipComposite   REMOVED - proven harmful. The composite quad is the only draw that writes the
//                   back buffer, so dropping it left stale content there and the image froze at
//                   a healthy 56 fps.
constexpr bool kPerspectiveOnly = true;   // auxiliary cameras are not the visible scene
constexpr bool kMainCameraOnly = true;    // SR3 renders 3-4 cameras/frame; only one is the player's
// uv = raw * tiling / 1024, read out of the vertex-shader disassembly:
//     mul r0.x, c<TilingU>.x, v1.x ; mul o1.xy, r0, 0.0009765625
constexpr float kShortUVScale = 1.0f / 1024.0f;
// The UV divide for the CURRENT draw: the vertex shader's own def constant when it has an
// unambiguous one, and the historic 1/1024 when it does not. Both the fixed-function conversion
// and the API character capture ask through here, so the two can never drift apart again - which
// is the bug that put the underwear's UVs at four times their proper size in BOTH copies.
float CurrentShortUVScale();
// Forward-declared (defined below, with FIX A): SetupTextureStages' texture-matrix scale
// (2026-09-18 audit fix) needs this before its own definition is reached.
int DecalUvSourceSet();

// The float2 texture-coordinate stream; see the uv conversion further down.
constexpr DWORD kUvStreamDefault = 7;              // clamped to the device's stream count at init
// One below the uv stream's own default, so a device with room for both never collides them -
// see g_skinFFStreamsOk (HookDevice) for what happens when there is not room for both.
constexpr DWORD kSkinFFStreamDefault = 6;
constexpr UINT kUvBytesPerVertex = 8;              // float2
constexpr size_t kMaxUvBufferBytes = 48u << 20;    // total held across all converted buffers


struct Settings {
    bool ffp = true;             // master switch; 0 = passive observer, for A/B against no conversion
    bool convertSkinned = true;  // characters need CPU skinning first (phase 2); ON by default as of 2026-09-17, synced to the deployed ini - 0 means no characters render
    // Adopt the frame's main camera by WHAT IT IS, not by what arrives first. See BeginFFP.
    bool mainCameraScreenSizedOnly = true;
    bool skipMirrored = true;    // skip passes whose handedness differs from the main camera
    bool cacheMeshAlbedo = false; // pin each mesh to its first-seen texture (streaming eviction); OFF by default as of 2026-09-17, synced to the deployed ini
    bool skipUntextured = true;  // don't convert passes whose shader samples nothing (the prepass)
    // Don't convert a draw into a render target that cannot hold colour. 0 restores the old
    // behaviour, which is the A/B for the doubled characters.
    bool skipNonColourTargets = true;
    // Don't convert a draw whose PIXEL SHADER writes more than one render target. 0 restores
    // the old behaviour, which is the A/B for the head texture and the head z-fighting.
    bool skipDeferredGBuffer = true;
    // CHANGE 1, 2026-09-18: don't convert a VFX mask material (ir_vfxmask / ir_vfxmasktod)
    // whose whole shape lives in Alpha_MaskSampler - a sampler this shim's own AlbedoRank
    // scores 0, so SetupTextureStages never binds it and disables every stage past 0 anyway.
    // With no Alpha_Threshold declared and no Decal-named albedo, ALPHAARG1 falls back to
    // D3DTA_TFACTOR, TFACTOR is pinned to 0xFFFFFFFF, and additive blending hands Remix the
    // full diffuse texture at alpha 1.0 - a soft light shaft rendered as an opaque slab. See
    // Classify's rule immediately before its final return. shader_constants.csv shows Alpha_
    // Mask is named ONLY by this one shader family in the whole game (grepped 2026-09-18), so
    // the rule cannot reach a character, a vehicle or a wall. ON by default: a bug fix, not a
    // guess.
    bool skipMaskOnlyVfx = true;
    int screenSpaceMode = 0;     // 0 pass, 1 ortho demote, 2 mark the ones sampling a render target; default 0 as of 2026-09-17, synced to the deployed ini
    // Demote the HUD to a UI overlay. The HUD arrives through DrawPrimitiveUP, which nothing
    // hooked until 2026-09-04, so it has been reaching Remix unclassified since the fork.
    // 0 restores that - the A/B for anything this changes.
    bool uiDemoteUP = true;
    // REBUILD the HUD as fixed function instead of merely demoting it. Remix declines a shader
    // draw when rtx.useVertexCapture is off; a fixed-function draw it accepts. This is what lets
    // the HUD survive capture-off, which is the configuration that finally removed the fullscreen
    // quads from in front of the camera.
    bool uiConvertUP = true;
    // 1 restores the double draw that put the menu, the map and the HUD in the world: after a
    // HUD draw was rebuilt as fixed function, the game's own draw still went out (a missing
    // brace) with float4 PIXEL positions through whatever vertex processing the bridge had
    // bound - (2560, 1440, 0) as world coordinates, a thousand units above the player.
    bool uiRawAfterHud = false;
    // KEEP the game's pixel shader on the rebuilt HUD draw. Nulling it is what this shim does for
    // WORLD geometry, where Remix wants the albedo texture rather than the shader - but for the
    // UI the pixel shader IS the content, and throwing it away is what produced a greyscale
    // video, missing text and blank menu backgrounds. See the note above DrawHudFixedFunction.
    bool uiKeepPixelShader = true;
    // Re-issue the CHARACTER ATLAS composites with fixed-function vertex processing and the
    // game's own pixel shader, so Remix executes them with vertex capture off. This is the same
    // technique that brought the menu video back in colour, applied to the draw that builds the
    // character's skin.
    bool compositeFfp = true;
    // Generate the albedo for the PLAYER's clothing family - a separate Diffuse_Map plus a
    // Pattern_Map on a second UV set - in the DIFFUSE map's own texture space, whenever the
    // pattern is a single uniform colour. See ClothAlbedoUniform.
    bool clothUniformFromDiffuse = true;
    // Give hair its STRAND detail by modulating the hair colour with the Dob_Map's strand
    // channel, instead of shipping one flat colour. 0 restores the flat constant.
    bool hairStrandsFromDob = true;
    // Colour hair the way ir_sr3pchair_c ps[8] does: HSV from Hair_Parameters/Hair_Spec_Alpha with
    // the Dob_Map's B channel as the brightness/saturation mask, through the cloth exposure curve.
    // 0 restores the older generator (Hair_Spec_Color2 * Dob.R), which used a SPECULAR colour.
    bool hairHsvAlbedo = true;
    // Bake the skin shader's matcap tone into the albedo: Diffuse * Diffuse_Color *
    // lerp(Sphere_Map_1, Sphere_Map_2, Blend_Map.r) * 2, matcaps averaged over their disc. Without
    // it the head binds the bare greyscale Diffuse_Map: no skin tone, and painted-on hair white.
    bool skinMatcapAlbedo = true;
    // Skin textures built per frame at most; a draw over budget keeps its raw diffuse for a frame.
    int skinGenPerFrame = 2;
    // Also bake the ONE-matcap form (ir_sr3shaderball: lerp(1, Sphere_Map, Sphere_Map_Amount)).
    bool skinSingleMatcap = true;
    // Last resort for a garment whose pattern is a DECAL on an independent uv set: take the
    // pattern's dominant colour for the whole surface. An approximation, and labelled as one -
    // it gets the garment's colour right and loses the decal. 0 leaves such garments untinted,
    // which is what they were.
    bool clothDominantPattern = true;
    // Fill the UNUSED atlas space in a generated albedo with the garment own colour instead of
    // leaving it black. A converted draw has no pixel shader, so the game clamp that keeps a
    // sample inside its island is gone - see the note above the fill.
    bool clothPadUnused = true;
    // THE CUTOUT. The bra's and the underwear's diffuse maps carry a transparent cutout in their
    // alpha - 63-90% of each (verified against every cm_bra_f_* and cm_unwr_f_* in the game
    // files; DXT5 at draw time) - and ir_sr3pccloth_c ps[6] emits that alpha as the pixel's
    // (`mul oC0, r3, c37` with r3.w straight from the Diffuse_Map fetch), which the game then
    // BLENDS (src=5 dst=6, measured). The garment is a template mesh cut to shape by its texture.
    // The generated copy carried that alpha correctly, and then two things threw it away: the
    // dilation wrote 0xFF000000 over every filled texel, and the converted draw took TFACTOR
    // alpha, so the template's spare quads rendered as opaque black squares. This keeps the
    // alpha through the dilation and alpha-tests the converted draw so Remix sees a cutout. The
    // DXT1 decoder's punch-through texel (index 3 of a 3-colour block) is fixed to alpha 0 on the
    // same switch; it was wrong, it just was not what these two garments hit.
    bool clothCutout = true;
    // THE DECAL, through the mesh. The pattern rides TEXCOORD1 and the albedo TEXCOORD0, two
    // unrelated unwraps - but a triangle relates them: rasterise each triangle in the albedo's
    // space and interpolate its uv1 to find the pattern texel for every albedo texel it covers.
    // Only triangles with a visible vertex are rasterised; the cut template geometry, which is
    // what made uv0 look like "3.7 tiles of overlap", never writes. Conflicts between visible
    // triangles are counted and logged, not assumed away. Falls back to the dominant colour
    // wherever the mesh does not reach.
    bool clothMeshDecal = true;
    // TILES. The bra's two cups share ONE island of the diffuse (mirrored uv0) but carry the star
    // at mirrored positions in uv1, so they disagree about 433 island texels and only one cup can
    // win in a single texture. When the mesh bake finds components that conflict, each conflicting
    // component gets its own TILE: the generated texture is tiles*dw wide, the skinned copy's u0
    // is shifted by whole tiles per component, and the draw's texture matrix is scaled by 1/tiles.
    // Cut geometry keeps its within-tile position, so it lands on transparent texels in every tile.
    bool clothDecalTiles = true;
    // THE COLOUR CURVE. ir_sr3pccloth_c ps[6] ends `mul oC0, r3, c37` with Tint_color = (5,5,5)
    // measured, into an HDR target the game tonemaps. clothTintScale (x2) stood in for that
    // linearly, and a linear scale keeps a colour's hue: the underwear's C = (0.059, 0.220, 0.298)
    // has G/B = 0.74 and stays BLUE, while the game shows cyan because at x5 the G and B channels
    // saturate together. Reinhard on Tint*C - x/(1+x) per channel - reproduces that: underwear
    // light cyan, bra fuchsia, armband and backpack yellow, and mid-range colours within a few
    // percent of the x2 brightness the corset was judged right at. The Tint is read from the
    // shader constant, not assumed. Player-family generator only; hair and NPC paths unchanged.
    bool clothColourCurve = true;
    // Take the UV divide from the vertex shader's own def constant instead of assuming 1/1024.
    // 0 restores the hardcoded value, which is the A/B for anything this changes.
    // Default 0 (hardcoded 1/1024) as of 2026-09-17, synced to the deployed ini.
    bool uvScaleFromShader = false;
    // Census of every draw we PASS THROUGH. With capture off a passed-through shader draw is
    // invisible, so this names exactly what is being lost.
    bool passCensus = true;
    // One-shot dump of every draw into a character-atlas-sized render target. The atlas is
    // composited at character load, not per frame, so no frame dump has ever caught it.
    bool atlasCompositeProbe = true;
    bool compositeToTexturePass = false; // never SKIP a composite into an off-screen texture; OFF by default as of 2026-09-17, synced to the deployed ini - reverted 2026-08-28, see the ini
    bool markAuxCamera = true;   // mark the 512x288 reflection pass; ON by default as of 2026-09-17, synced to the deployed ini - turned on 2026-08-18 once the post chain was confirmed hidden
    bool clearBackBuffer = true; // nothing writes it once the composite is marked - see the clear
    bool hideLightVolumes = true;// light volumes (their light is already injected separately)
    bool injectLights = true;
    bool rankAlbedo = true;      // use CTAB sampler names to choose base colour
    bool excludeRTAlbedo = true; // never use a render-target texture as base colour
    // A texture with fewer than 3 colour channels (D3DFMT_A8 and similar) carries no colour
    // at all, only alpha. SetupTextureStages used to select its colour anyway whenever it was
    // the chosen albedo (COLORARG1=D3DTA_TEXTURE), and an alpha-only format samples RGB=0 in
    // fixed function, so the surface rendered solid BLACK under its own alpha mask. Measured
    // 2026-09-18 on the decal probe: fmt=28 (D3DFMT_A8), stage=2, albedoSampler='Decal_MapSampler'
    // - a static world decal, not a particle. ON takes COLORARG1 from D3DTA_TFACTOR instead
    // when this happens, so the colour comes from whatever this branch already puts in
    // TEXTUREFACTOR (opaque white here; see SetupTextureStages for what TFACTOR does and does
    // not carry for this population) while the texture stays bound for its alpha. OFF restores
    // today's behaviour for an A/B. Default ON: this is a bug fix, not a guess.
    bool albedoRejectColourlessTexture = true;
    // Instrumentation only - it only logs and counts. Detects a converted draw whose chosen
    // albedo (EffectiveAlbedo's pick, not just g_curTexture[0]) differs from what the SAME draw
    // picked last frame - the shape of bug behind surfaces flickering through unrelated images.
    bool albedoChurnProbe = true;
    float lightScale = 1.0f;
    float lightRangeScale = 1.0f;
    int hiddenPassMode = 2;      // 0 pass, 1 ortho demote, 2 skip, 3 bind the marker texture; default 2 as of 2026-09-17, synced to the deployed ini - mode 3's marker subsystem is retired, see CreateMarkerTexture
    // --- diagnostics ---
    int dumpFrame = 60;          // frames to wait after the camera appears before dumping one; default 60 as of 2026-09-17, synced to the deployed ini
    bool logLayouts = true;      // one line per distinct vertex layout, capped
    bool shapeProbe = true;      // one frame of world-space bounds for draws near the camera
    bool skinProbe = true;       // one frame of skinned-draw layout, bones and objTM
    bool rigidSkinProbe = true;  // one frame of the weightless BLENDINDICES layout
    bool remixShortUV = true;    // re-declare short2 texcoords as short2n so Remix reads them
    // FIX 3: UvBufferFor's static (whole-buffer) and dynamic (snooped/ring) decodes both assumed
    // stream 0 was bound at byte offset 0 - a pooled buffer at a non-zero offset fed every vertex
    // ANOTHER vertex's texcoords. See UvBufferFor's own comment for the phase/base split. Off
    // restores the exact old, offset-blind addressing.
    bool uvHonourStreamOffset = true;
    // FIX A: sample a decal draw's albedo from vertex TEXCOORD1 when its vertex shader declares
    // that but no TEXCOORD0 at all. See DecalUvSourceSet.
    bool decalUvFromDeclaredSet = true;
    // 2026-09-18 AUDIT FIX: SetupTextureStages computed its short2 texture-matrix scale from
    // g_curLayout.texcoordType (set 0) even on a FIX A set-1 draw, whose texcoordType is -1 for
    // want of any stream-0 TEXCOORD0 - so su/sv fell back to 1.0, D3DTTFF_DISABLE let the raw
    // short values UvBufferFor deliberately left unscaled reach the sampler directly, and 1024
    // raw units became 1024 UV: about a thousand texture repeats, a fine stripe pattern standing
    // in for the real image. ir_sr3fauxinterior (fake window interiors) is one of the FIX A
    // families. ON takes the scale from whichever set the conversion actually used; OFF restores
    // today's texcoordType-only behaviour for an A/B. See SetupTextureStages.
    bool decalUvScaleFromSet = true;
    // FIX B: take ALPHAARG1 from the texture, not TFACTOR, when the chosen albedo sampler names
    // a Decal map AND the draw is genuinely blended by its factors. See SetupTextureStages.
    bool decalAlphaFromTexture = true;
    // DIAGNOSTIC: building decals (signs, logos, posters - the family whose CHOSEN albedo
    // sampler name contains "Decal") render as a flat quad showing a fine repeating stripe
    // pattern instead of their image. FIX A and FIX B above did not change it, and depth,
    // blinking, albedo churn, the alpha path, the texcoord SET and the tiling lookup have all
    // been measured and ruled out. The two things left that decide what appears on the quad -
    // which texture is bound as albedo, and what coordinate range is fed to it - have never
    // been measured; this does that, for the first 8 qualifying draws, and changes no render
    // state. See ProbeDecalDraw.
    bool decalProbe = true;
    bool decalProbeDumpTexture = true;  // also write the bound albedo's top mip as a .dds
    // STRICT TARGET, 2026-09-18: without this, ProbeDecalDraw armed on the first 8 converted
    // draws whose albedo sampler merely NAMED a Decal map, and in practice that population is
    // dominated by particle billboards (verts=20 prims=10 at the origin, usesObjTM=0) whose
    // source buffer is DYNAMIC, so the raw-coordinate read then refused every one of the 8
    // reports. ON also requires stream 0 to be static, the draw to be indexed, and the draw to
    // carry an object transform that does not place it at the origin - the shape of a BUILDING
    // decal, not a particle effect. OFF restores the old, permissive test for an A/B; see the
    // rejection counters beside ProbeDecalDraw for which test is excluding candidates when this
    // is ON.
    bool decalProbeStrictTarget = true;
    bool deinstanceConverted = false; // reset stream frequency on single-instance converted draws; OFF by default as of 2026-09-17, synced to the deployed ini - measured not to fix the popping it targeted, see the ini
    bool clothProbe = true;      // one pass over the customisable-clothing material inputs
    bool diffuseColorProbe = true;  // distribution of the Diffuse_Color constant we discard
    bool rtAlbedoCopy = true;    // re-upload a render-target albedo so Remix can hash it
    bool atlasSnoop = true;      // snoop the game's own writes to the character atlas
    // Validate the source range (VirtualQuery, in Hook_SurfUnlockRect) before the atlas
    // snoop copies from it, and run the copy itself under a structured-exception guard.
    // Off reverts to the old unconditional memcpy. See the ini for why this exists.
    bool snoopValidateSource = true;
    // THREAD-SCOPED g_internal. Before this, g_internal was a plain global the render thread
    // held true across the whole of BeginFFP while the game's own atlas/vertex-buffer lock and
    // unlock pairs ran concurrently on its MAIN thread - a main-thread call in that window read
    // g_internal == true and wrongly skipped its own bookkeeping, which is what orphaned a
    // pending atlas entry and produced the September 14 crash (see g_internalThread and
    // InternalHere, above the hooks). ON scopes every guard read to "the thread that actually
    // set it"; OFF restores the bare global for an A/B.
    bool snoopThreadScopedInternal = true;
    // 2026-09-18 AUDIT FIX: Hook_VBUnlock consumed g_pendingLock whenever self matched, with no
    // check on WHICH thread was calling. UvBufferFor's RegisterSnoop(g_stream0) makes every
    // DYNAMIC stream-0 buffer a snooped instance stream, and roughly a dozen probes read-lock
    // g_stream0 without checking DYNAMIC first (BakeDecal, MeasureShortUVs, OcclMeshFor and
    // others) - so the render thread's own internal Unlock could land while a GAME thread's write
    // Lock on the same buffer was still pending, consume THAT entry, copy a half-written buffer
    // into the snoop cache, and clear the pending slot before the game's real Unlock ever ran -
    // whose finished write was then never snooped. ON restricts consumption to the thread that
    // set the pending entry (a main-thread unlock still consumes its own main-thread entry
    // regardless of the internal flag, which is what the September 14 change was actually for);
    // OFF restores that change's unconditional consume for an A/B.
    bool vbUnlockOwnerThread = true;
    bool cameraOnly = false;     // hand Remix a camera and touch no draw at all
    bool cameraOnlyFloatUV = false;  // also convert SHORT2 texcoords on pass-through draws
    bool cameraMainViewOnly = true;  // give Remix ONLY the main scene camera, not all ~11
    bool convertDynamicUV = true;    // convert SHORT2 UVs on DYNAMIC buffers via the snoop
    bool remixApi = true;            // initialize Remix's programmatic API and register the device
    bool remixApiCamera = false;     // AND hand it the camera through SetupCamera (step 1b)
    bool remixApiTestCube = false;   // step 2: one API-submitted mesh, to prove it appears; OFF by default as of 2026-09-17, synced to the deployed ini
    bool remixApiCharacter = false;  // step 3a: submit one character mesh through the API; OFF by default as of 2026-09-17, synced to the deployed ini
    bool remixApiSkinning = false;   // step 3c: let Remix skin. CRASHED 2026-09-02, see the ini
    bool remixApiClothAlbedo = false;  // replace a cloth slot texture with a generated one
    bool bakeShaderAlbedo = false;    // run the game's pixel shader into a UV-space target; CLOSED, do not reopen (exhausted memory, see the ini) - 2026-09-17: the LoadSettings fallback below used to default this to true, contradicting this false; both now agree
    bool clothUseDiffuse = true;      // multiply the customisation colour by the Diffuse_Map
    float clothBrightness = 1.0f;     // scale the baked cloth albedo (tuning, not a fix)
    float remixApiCharacterOffset = 3.0f;  // stand it beside the real one, so they cannot z-fight
    bool scanCommandBlocks = false;  // walk each command block to see what the engine will do; OFF by default as of 2026-09-17, synced to the deployed ini
    // OFF by default and on its own key. Reading the GAME's render targets with
    // GetRenderTargetData froze SR3 twice - once unbounded, and again at one read per 120
    // frames with a 60-read budget, which means the cost was never the problem: Remix's
    // D3D9 cannot service a readback of a surface it owns and is using. Sharing a gate with
    // atlasSnoop meant the harmless snoop could not be enabled without the dangerous probe.
    bool rtContentProbe = false;
    int rtAlbedoRetries = 3000;  // frames to keep retrying while the source reads back blank; default 3000 (~2 minutes) as of 2026-09-17, synced to the deployed ini
    bool generateCloth = true;   // build clothing albedo per outfit from the pattern + colours
    bool clothDump = false;      // write the first 3 outfits and their patterns out as raw RGB
    bool charTexDump = false;    // write every named stage of the probed character materials
    int captureKey = 0x78;       // VK_F9: re-arm the frame and shape dumps, 0 disables
    // Write ONE minidump (sr3-rtx-test.dmp) at device-hook time, with a null exception pointer,
    // so MiniDumpWriteDump and its flags can be verified on demand - a wrong flag or a bad file
    // handle would otherwise only be discovered the next time the game actually crashes. OFF by
    // default: this is a diagnostic, not something to leave writing a dump file every launch.
    bool crashTestDumpAtStart = false;
    // Refuse to read a bone palette for a draw whose vertex shader declares no BLENDINDICES
    // input. Such a shader places the mesh by objTM alone; posing it costs a bone matrix that
    // belongs to whatever was drawn before. 0 restores the old fallback-to-c52 behaviour, which
    // is what makes this a controlled experiment rather than an assertion.
    int skinRequireBoneDecl = 1;
    int skinRingMB = 24;         // size of the CPU-skinning ring; larger means fewer wraps; default 24 as of 2026-09-17, synced to the deployed ini
    bool forceOcclusionVisible = true;   // answer the engine's occlusion queries "fully visible"; ON by default as of 2026-09-17, synced to the deployed ini - must stay 1 with capture off, see the ini
    // PERFORMANCE. Two kinds of draw cost a bridge round trip each and produce nothing:
    //  - the OCCLUSION PROXIES: the draw the engine issues between Query::Issue(BEGIN) and (END)
    //    so the query can count its pixels. With forceOcclusionVisible the count is fabricated, so
    //    neither the proxy draw nor the Issue calls need to reach D3D9. A proxy is a box - the
    //    skip is gated on primitive count so a real object drawn inside a query is left alone,
    //    and the frame report histograms what was actually seen inside queries.
    //  - DECLINED PASS-THROUGH: a pass-through draw with a programmable vertex shader bound.
    //    With rtx.useVertexCapture = False Remix drops those outright (that is why the HUD was
    //    invisible until it was rebuilt), so forwarding them buys nothing. Turn this OFF if
    //    vertex capture is ever turned back on.
    bool skipOcclusionProxies = true;
    int occlusionProxyMaxPrims = 12;
    bool skipDeclinedPassThrough = true;
    // CPU SKINNING is the shim's dominant cost: PROFILE measured 12.27 ms of a 27 ms city frame
    // in the vertex loop, 117,984 skinned vertices over 24 draws, ~104 ns each. Every vertex was
    // paying for four per-influence policy checks and six global counters that the same report
    // shows inert. skinFast runs a straight blend when every one of those policies is off - the
    // output is identical by construction - and skinThreads splits draws of 2048+ vertices over
    // a persistent worker pool (0 = half the cores, capped at 4).
    bool skinFast = true;
    // Default 1 as of 2026-09-17, synced to the deployed ini (0 = auto: half the cores, capped at 4).
    int skinThreads = 1;
    // Fixed-function (hardware) skinning: the alternative to CPU skinning above, through
    // D3D9's indexed vertex blending - see the big comment beside SkinViaFixedFunction, below
    // UnbindSkinned, for why this was thought closed (MaxVertexBlendMatrixIndex=8 in dxvk-
    // remix's reported caps) and why it is not (that cap describes something else; the actual
    // path stages maxBone+1 matrices and both SetTransform and the bridge validate indices up
    // to WORLDMATRIX(255)). OFF by default: the CPU ring above remains the default path and is
    // completely untouched when this is off. ON, a draw that would go to SkinAndBind tries the
    // hardware path first and falls back to CPU skinning - not to pass-through - for anything
    // it refuses (cloth, morph, a declaration it cannot read, a bone it cannot stage safely).
    bool skinViaFixedFunction = false;
    // THE SHAPE BAKE (morphBake): apply a character's morph delta ONCE into a whole-buffer copy
    // of its source mesh, instead of refusing every morph-active draw from the GPU (fixed-
    // function) skin path outright. Only safe when the delta itself rarely changes frame to
    // frame - see morphProbe, which measures that before this is trusted. OFF by default.
    bool morphBake = false;
    // Cap on the total bytes held by every baked copy at once (sum of each source VB's own
    // desc.Size) - LRU-evicted by last-used frame when a new bake would exceed it.
    int morphBakeMaxMB = 24;
    // New (first-time) bakes AND rebuilds (the delta actually changed) are each capped at this
    // many per frame - a rebuild count exceeding it is the delta-churn guard: the shim would
    // otherwise pay a full re-bake every frame for a delta that is not, in fact, static.
    int morphBakeMaxPerFrame = 2;
    // Measures whether a character's morph delta changes frame to frame at all, and how often -
    // read this BEFORE trusting morphBake=1. OFF by default; the ini ships it ON for the next run.
    bool morphProbe = false;
    // FIX 1: WORLDMATRIX(0) IS D3DTS_WORLD - staging it for a mesh that uses bone 0 overwrites
    // it, UnbindSkinFF restores plain objTM after the draw, and the NEXT draw sharing this
    // palette (the sameStage branch in SkinViaFixedFunction, common - 8.9 draws/frame share one
    // mesh key) restages NOTHING, so WORLDMATRIX(0) stays at the WRONG objTM for every vertex
    // weighted to bone 0. Off restores the old, restage-nothing-on-reuse behaviour.
    bool skinFFRestageBone0 = true;
    // FIX 2: SkinFFBufferFor decodes its side buffer assuming stream 0 starts at byte 0, and
    // SkinViaFixedFunction always bound it at offset 0 - so a pooled source buffer bound at a
    // non-zero stream-0 offset (several accessories sharing one VB) fed every vertex ANOTHER
    // vertex's weights/indices, addressing bone slots this draw never staged. See
    // SkinFFBufferFor's own comment for the phase/base split. Off restores the old, offset-blind
    // addressing and the old 0-only bind.
    bool skinFFHonourStreamOffset = true;
    // FIX 4: SkinFFDeclarationFor only clones a declaration that carries BOTH BLENDWEIGHT and
    // BLENDINDICES, but a rigid single-bone mesh (skinRigidSingleBone) legitimately carries
    // BLENDINDICES and NO BLENDWEIGHT at all - SkinFFBufferFor already writes an implicit [1,0,0]
    // weight for these, but the clone had nothing to retarget, so every one of these declarations
    // failed and ~10-11 draws/frame fell back to CPU skinning ("declarations: 3 made, 7 failed").
    // On, with skinRigidSingleBone also on, appends the missing BLENDWEIGHT element instead of
    // requiring one to already exist. Off restores the old retarget-only cloning.
    bool skinFFRigidDecl = true;
    // FIX 5: SkinViaFixedFunction stages every draw's bones from mesh->usedBones - built by
    // GetBaseMesh from the VERTEX DECLARATION's weight/index bytes - never from what the VERTEX
    // SHADER actually reads. SkinAndBind (the proven CPU path) does the opposite: reads =
    // usesBlendIndices || !skinRequireBoneDecl, influences = usesBlendWeights ? 4 : 1 - see its
    // own "A shader that declares no blendweight input uses exactly ONE bone" comment. A shader
    // that reads no BLENDINDICES at all places the mesh by objTM alone; one that reads
    // BLENDINDICES but no BLENDWEIGHT (every vehicle body/glass variant, and any other rigid
    // attachment) uses exactly bone idx[0] at weight 1, regardless of what the declaration's
    // other three weight bytes hold. The GPU path ignored both distinctions and staged whatever
    // the declaration happened to carry - including palette slots the object never uploaded,
    // still holding the PREVIOUS character's bones, which is why car windows and other rigid
    // parts floated with player/NPC animation once FIX 4 (skinFFRigidDecl) started cloning these
    // declarations successfully and moved this population onto the GPU path. On: refuse a
    // no-BLENDINDICES shader outright (CPU path handles it), stage exactly idx[0] for a
    // no-BLENDWEIGHT shader instead of the declaration's other three weights, and refuse a
    // four-bone draw with any vertex the shader would otherwise leave at its bind pose. Off
    // restores today's declaration-driven staging.
    bool skinFFFollowShaderInfluences = true;
    // THE BRIDGE DIET. Averaged over 60 city hitches: 24 ms inside our hooks, 43 ms outside -
    // and "outside" is mostly the game's own D3D9 traffic crossing the 32->64-bit bridge for
    // draws that are then skipped or declined. With rtx.useVertexCapture = False no programmable
    // vertex shader ever runs on the bridge, so two whole classes of call are dead weight:
    //   dietVsConstants  SetVertexShaderConstantF - bone palettes (3 KB per skinned draw) and
    //                    per-draw matrices. Shadowed in g_vsConst, which is all the shim reads.
    //   dietVsBinds      SetVertexShader - shadowed in g_lastVS; the bridge's own binding is
    //                    synced lazily (SyncBridgeVS) only before a draw that is forwarded, so
    //                    a converted draw gets null and a raw pass-through gets the game's.
    // Both must be OFF if vertex capture is ever turned back on.
    bool dietVsConstants = true;
    bool dietVsBinds = true;
    // CPU OCCLUSION CULLING. forceOcclusionVisible answers every query "visible", so the engine
    // draws everything in the frustum and Remix path-traces the block behind the building. The
    // engine's own test cannot be handed back (it needs the depth prepass Remix never executes),
    // so the test is done here: the proxy drawn inside each query is the object's box (a unit
    // cube placed by Scale/Offset/World_xform/Render_offset, read from the proxy shader's own
    // constants), the rigid opaque converted draws are the occluders (geometry cached once per
    // mesh), and a worker thread rasterises the largest occluders into a small depth buffer and
    // tests each box against it. In doubt - near plane, off-screen, no data - the answer is
    // VISIBLE; only a box entirely behind rasterised opaque geometry is culled. One frame late.
    bool occlusionCull = true;
    bool occlusionDryRun = false;        // compute and report, but keep answering "visible"; OFF by default as of 2026-09-17, synced to the deployed ini - real culling is engaged
    int occlusionMaxOccluders = 2000;    // largest on screen, per frame; default 2000 as of 2026-09-17, synced to the deployed ini
    int occlusionMaxTris = 250000;       // rasterisation budget per frame; default 250000 as of 2026-09-17, synced to the deployed ini
    int occlusionMeshBudget = 16;        // new meshes read from the bridge per frame; default 16 as of 2026-09-17, synced to the deployed ini
    int occlusionWidth = 320;            // depth buffer; height follows the back buffer aspect
    // Nothing within this distance of the camera (view depth of the box's nearest corner, in
    // game units) is ever culled by us: inside it the engine's own behaviour stands. The user's
    // rule, 2026-09-10 - the near field is where a wrong answer is most visible and least worth it.
    int occlusionMinDistance = 5;
    // Hysteresis and answers keyed by the OBJECT (quantised box translation + size) rather than
    // by the query pointer, which the engine may hand to a different object next frame.
    bool occlusionKeyByObject = true;
    // An occluder within this many centimetres of a box's translation is the box's own object
    // and never occludes it; 0 = the exact quantised key only.
    int occlusionSelfToleranceCm = 50;
    int occlusionExplainFlips = 40;      // verdict flips explained in the log per report window
    // The uv and occluder caches are invalidated through a per-buffer index instead of a scan
    // of the whole cache per invalidated buffer (~60 a frame x ~2300 entries = the 1.2 ms drain).
    bool indexedInvalidation = true;
    // A game lock drops only the cached occluder meshes whose byte range it overlaps (DISCARD
    // or a size-0 lock drops the buffer's all). Meshes in a buffer the game appends to every
    // frame otherwise blink out of the pool, and everything behind them flips visible.
    bool occlusionRangeInvalidation = true;
    // An occluder used last job stays in the pool past occlusionMaxOccluders: the boundary at
    // that rank otherwise moves with every change in the offered set.
    bool occlusionStickyOccluders = true;
    // Scan each skinned draw's index range once (a read lock, 8 new ranges a frame) and count
    // indices outside the declared vertex window. Our ring copy holds only the window; an index
    // past it reads another mesh's vertices - a triangle stretched across the screen.
    bool probeIndexWindow = true;
    // A converted, non-skinned, indexed draw narrowed to the tight vertex range its own indices
    // reference, instead of the whole-mesh window SR3 declares for a building decal (2-4
    // triangles submitted with NumVertices set to the whole mesh, measured at 28336). Remix's
    // geometry hash covers positions and texcoords over that whole declared window, so the
    // decal's identity to Remix churns every time the game's streamer writes anywhere else in
    // the shared buffer, although the decal's own six vertices never move. OFF by default: this
    // only changes what the device is told to process, and is meant to be turned on after a
    // session has been read with it off - see the TIGHTEN WINDOW report line, which is printed
    // and measured regardless of this switch.
    bool tightenConvertedWindow = false;
    // 2026-09-18 AUDIT FIX: g_tightWin above used to cache a tight window keyed on a raw
    // IDirect3DIndexBuffer9* with nothing AddRef'd and nothing invalidated - SR3 streams
    // geometry, so a released index buffer's address handed straight back to a new one made the
    // cache answer with a different mesh's range. ON pins every index buffer this cache ever
    // learns the shape of (see GetTightIbShape) so its address cannot be recycled while known to
    // it; OFF restores today's unreferenced cache for an A/B.
    bool tightenWindowPinIndexBuffers = true;
    // The 2026-09-18 PROFILE audit: TightenConvertedWindow's cache-miss path evicted an
    // ARBITRARY entry (erase(g_tightWin.begin())) once full, rather than the least recently
    // used one, so a decal-dense area holding more than the cache's cap thrashed and spent the
    // 32-lock-a-frame budget every frame forever instead of once. ON replaces that with the
    // same age-based scheme OcclMeshFor/KickOcclusionJob already use - a lastUsed frame stamp
    // and a periodic sweep (SweepTightWin, called from Hook_Present); OFF restores today's
    // arbitrary eviction for an A/B.
    bool tightWinEvictByAge = true;
    // TightenConvertedWindow's safety net used to only ever refuse when the referenced range
    // fell outside what the game declared (see g_tightRefuseNotSubset, and the OUTSIDE WINDOW
    // report line printed alongside TIGHTEN WINDOW). A refusal falls back to the game's own
    // declared window, which is exactly the undefined case in D3D9: a vertex the indices
    // reference but the declared window does not cover has no obligation to exist there, and
    // Remix builds the draw's geometry from the range it is handed - a triangle stretched across
    // the scene. ON widens the window instead, to [min(minIndex, lo), max(minIndex+numVertices,
    // hi+1)), so the device is always given a window that actually contains every vertex the
    // indices touch; the widened window is clamped to the bound vertex buffer first (see
    // g_tightRefuseWidenBeyondBuffer), so this can never declare a vertex that does not exist.
    // OFF restores today's refuse-and-fall-back for an A/B.
    bool widenUnderDeclaredWindow = true;
    // INDEX WINDOW PROBE - THE 1e10 SLIVER, measured and dated: SR3 hides an unused world-decal
    // quad by parking its vertices at exactly 1e10 (float bytes F9 02 15 50, seen in the Y or Z
    // slot of an otherwise ordinary vertex) so the rasteriser clips it away; nothing clips a
    // path-traced triangle, so it appears as a sliver stretched across the screen for one frame -
    // confirmed real geometry in Remix's Geometry Hash debug view. Two independent samples found
    // the offending vertex INDEX equal to the draw's own declared NumVertices (v=24 -> bad vertex
    // gv=24; v=76 -> bad vertex gv=76) - exactly one past the end of [MinVertexIndex,
    // MinVertexIndex+NumVertices). Remix never looks at that declared window at all:
    // dxvk-remix/src/d3d9/d3d9_rtx.cpp:666 computes geoData.vertexCount purely from the bound
    // index buffer's own min/max (DrawContext DOES carry MinVertexIndex/NumVertices,
    // d3d9_rtx.h:53-54, but neither is referenced anywhere in that file) - so the sentinel vertex
    // is faithfully included and its triangle gets built. This is the MEASUREMENT half: for every
    // CONVERTED indexed draw, is the referenced index range actually inside the declared window.
    // Cheap by construction: a RIGID (non-skinned) draw reuses the g_tightWin entry
    // TightenConvertedWindow (above) already Locked and cached for this exact identity a few
    // lines earlier in Hook_DrawIndexedPrimitive - one hash-map lookup, no extra Lock. Only a
    // SKINNED draw, which TightenConvertedWindow deliberately never touches, pays for a fresh
    // min/max-only scan (no allocation, no hashing - see ScanIndexMinMaxCheap). ON by default:
    // see CheckIndexWindow's own comment, above Hook_DrawIndexedPrimitive, for the full picture.
    bool indexWindowProbe = true;
    // DIAGNOSTIC (measurement only): dynamic-VB last-unlock thread vs drawing thread. See LtNoteDraw.
    bool lockThreadProbe = true;
    // DIAGNOSTIC (measurement only): do a character head/body draws bind albedo with different
    // sRGB / size / format / render-target state? See BhNoteDraw.
    bool headBodyProbe = true;
    // INDEX WINDOW CLAMP - the fix. OFF (the default): measured only, every draw issued exactly
    // as today, byte for byte. ON: a converted, non-skinned, D3DPT_TRIANGLELIST draw the probe
    // above found referencing an out-of-window index is re-issued from a SHIM-OWNED index buffer
    // in which every triangle touching that index has all three of its own indices replaced with
    // MinVertexIndex - a zero-area triangle, never hit by a ray, harmless under rasterisation.
    // Narrowing or widening the WINDOW parameter (widenUnderDeclaredWindow, above) cannot fix
    // this bug: Remix ignores that parameter completely (see the comment above), so only
    // rewriting the actual index VALUES removes the sliver. Primitive count, vertex window and
    // every other draw parameter are left exactly as the game declared them - see
    // IssueIndexWindowClampedDraw's own comment for exactly what changes and what does not.
    bool indexWindowClamp = false;
    // INDEX WINDOW CLAMP, STRIPS AND FANS (indexWindowClampStrips) - default OFF, so today's
    // deployed indexWindowClamp=1 keeps behaving byte for byte the same until this is switched on
    // separately. MEASURED in game, first run with indexWindowProbe+indexWindowClamp both on:
    // 864.5/frame examined, 4.67/frame had an index OUTSIDE the declared window (worst overshoot
    // 1 - always the sentinel-at-NumVertices case, see CheckIndexWindow's own comment), and
    // 266342 flagged draws over that run were left uncorrected because EVERY one of them was a
    // D3DPT_TRIANGLESTRIP or D3DPT_TRIANGLEFAN - IssueIndexWindowClampedDraw's own
    // TRIANGLELIST-only restriction means the fix above never actually ran. ON: a flagged
    // strip/fan is first EXPANDED into an explicit triangle list in the same shim-owned ring the
    // TRIANGLELIST path already uses (see ExpandStripOrFanToTriangleList's own comment for the
    // winding fix a strip needs and a fan does not), then collapsed triangle-by-triangle exactly
    // the way a native TRIANGLELIST draw already is - no separate collapse logic, no separate
    // counters (g_iwcDrawsCorrected/g_iwcTrianglesCollapsed cover both). OFF leaves a flagged
    // strip/fan counted apart in g_iwcTopologyRefused and issued unchanged, same as today.
    bool indexWindowClampStrips = false;
    // ORPHAN SWEEP (2026-09-21): every pointer-keyed cache in this file AddRefs a game-owned
    // resource so a recycled address can never be handed an old buffer's cached data - correct,
    // but for STATIC resources the only release paths were "the game writes the buffer again"
    // (never happens for static geometry), a size-cap flush (rarely tripped), or nothing at all.
    // A district the game streams out, or the whole city at the menu, then stayed alive in
    // NvRemixBridge.exe forever: measured at exit, 11,960 resources / 148 declarations / 1,902 VS
    // / 886 PS still alive on the server with nothing left on screen that needed them.
    //
    // SweepOrphans (called from Hook_Present) periodically probes every reference this shim
    // holds - `const ULONG pub = p->AddRef() - 1; p->Release();` is a LOCAL atomic read, no IPC -
    // and releases whatever the shim turns out to be the ONLY holder of. Device bindings
    // (SetStreamSource/SetTexture/SetIndices/state blocks) do not add to the public count, so an
    // object still bound is never mistaken for an orphan; the shadow state this shim already
    // tracks is checked before anything is probed. Master switch; 0 behaves exactly as before.
    bool orphanSweep = true;
    // How often the sweep runs, in frames. It walks every cache this shim owns, so it is not
    // free - see the ORPHAN SWEEP report line for its own measured cost.
    int orphanSweepFrames = 120;
    // Tally and log what WOULD be released, release nothing. The A/B for "is this finding the
    // population it should" before it is trusted to touch a live cache.
    bool orphanSweepReportOnly = false;
    // Shaders and vertex declarations stay PINNED unless this is on. They were pinned for the
    // process lifetime before the sweep existed, so any memo keyed by a shader or declaration
    // ADDRESS could never go stale; releasing them makes address reuse possible, and not every
    // such memo has been audited. They are also bounded by the game's own shader set, so they
    // do not grow as the player drives - the unbounded growth is vertex/index buffers and
    // textures, which the sweep always handles.
    bool orphanSweepShadersDecls = false;
    // GROUND TRUTH probe (SkinAndBind, "GROUND TRUTH: is our shadow..."): reads back the
    // device's own vertex shader constants to check them against this shim's g_vsConst shadow,
    // two GetVertexShaderConstantF calls - reads, therefore synchronous bridge round trips - per
    // single-bone skinned draw while armed. It already produced its answer: the SHADOW vs DEVICE
    // report line reads 12 checked, 0 matched, 12 mismatched, every run of this session. OFF by
    // default because of that - there is nothing left this probe would tell us that the log does
    // not already say.
    bool shadowCheckProbe = false;
    // ProbeHudDraw: two uncached GetDesc bridge round trips (g_curRenderTarget, then g_stream0)
    // per draw for every draw in the frame until it has produced 8 reports - and every F9
    // (RingRecordDraw's capture key) re-arms it by resetting g_hudProbeReports to 0. Purely a
    // Log()-only diagnostic: nothing else in this file reads anything it sets. OFF by default so
    // a capture taken with F9 does not pay for it at exactly the moment someone is diagnosing
    // something else.
    bool hudProbe = false;
    // A converted draw whose STATIC stream-0 range was written within the last N frames is
    // skipped for those frames. The one-frame stretched polygons appear "when meshes load";
    // whether the fault is bridge ordering or Remix's geometry cache, the first draw after
    // the write is the one that shows it, and one frame of pop-in is invisible next to that.
    bool deferFreshStaticDraws = true;
    int deferFreshFrames = 1;
    int ringFrames = 60;                 // frames kept in the rolling draw record written on F9
    // ASSET HASH PROBE: reproduces Remix's own geometry-hash inputs (indices, geometry
    // descriptor, texcoords) for CONVERTED decal-named/skinned draws across a short burst
    // armed by the capture key (F9), to tell whether the hash genuinely changes frame to
    // frame (the flicker in Remix's Geometry Hash debug view) or this shim resubmits
    // identical geometry under a churning identity. Diagnostic only: every buffer it touches
    // is locked read-only (or read from the existing snoop copy for a dynamic source), and it
    // binds nothing and draws nothing - see the ASSET HASH PROBE summary this logs at the end
    // of each burst. 0/false disables entirely: the burst never arms and the per-draw hook
    // returns on its first line.
    bool assetHashProbe = true;
    int assetHashProbeFrames = 60;       // frames captured per F9-armed burst
    // ASSET HASH PROBE WORLD: the probe above only ever sampled decal-named or skinned draws, on
    // the unverified assumption that a plain building surface (a window, a wall - never decal-
    // named, never skinned) would not need watching. OFF by default so ProbeAssetHash's filter is
    // BIT-FOR-BIT identical to before; ON, it additionally admits any other non-skinned converted
    // draw that binds an albedo texture, so the hypothesis that building geometry churns Remix's
    // Geometry Hash can actually be measured rather than assumed away. See ProbeAssetHash's own
    // comment for the exact three-way OR.
    bool assetHashProbeWorld = false;
    // ASSET HASH PROBE ALBEDO: the probe above reproduced two of Remix's three geometry-hash
    // inputs (indices, texcoords/descriptor) but never the MATERIAL half - materialData.getHash()
    // in Remix's own drawCall.getHash() XOR is simply the albedo texture's image hash, and this
    // shim was never recording which texture it bound as albedo for a probed draw, let alone
    // whether that identity changed frame to frame. ON by default: reuses EffectiveAlbedo()'s own
    // selection (g_lastEffectiveAlbedo) rather than re-deriving it, costs one pointer compare and
    // a couple of short strncpy_s calls per probed draw, and is the whole point of this pass - see
    // ProbeAssetHash's cur.albedoOk/albedoHash and LogAssetHashProbeSummary's TOP CHURNER block.
    bool assetHashProbeAlbedo = true;
    // Frame stepping: frameStepPauseKey (F7) freezes the game at Present with the frame on
    // screen; frameStepKey (F8) releases exactly one frame; every stepped frame gets a full
    // frame dump when frameStepDumpEachStep=1; the capture key still writes the rolling
    // record while frozen. 0 disables.
    int frameStepPauseKey = 0x76;
    int frameStepKey = 0x77;
    bool frameStepDumpEachStep = true;
    // Decal offset: a converted draw that does not write depth (or tests ZFUNC=EQUAL) is a
    // layer on top of other geometry - posters, signs, dirt, road paint. In a path tracer two
    // coplanar surfaces shimmer; the rasteriser resolved them by draw order and depth bias.
    // Such a draw is pulled toward the camera by this many parts per thousand of its distance
    // (1 = 1 mm per metre). 0 disables.
    // Default 0 as of 2026-09-17, synced to the deployed ini - the pull is currently DISABLED,
    // which makes decalPrimaryOffset/biasOffset/blendedOffset/decalByLowestSampler inert,
    // since they only affect the offset computed under decalOffsetPermille > 0.
    int decalOffsetPermille = 0;
    // Layer offset: the same triangles (vertex window AND index range) at the same placement,
    // converted again in the same frame with another texture or state, are a second material
    // pass over the same surface - coplanar by construction, and the exact-duplicate filter
    // keeps both because their textures differ. Pulled toward the eye by pass number times this.
    bool layerOffset = true;
    // Default 0 as of 2026-09-17, synced to the deployed ini - the per-pass pull is disabled.
    int layerOffsetPermille = 0;
    bool biasOffset = true;              // a draw the game gives a depth bias is a decal: pull it too
    // THE GAME'S OWN WORD for a decal: a pixel shader whose FIRST (lowest-register) sampler is
    // a Decal map is a dedicated decal pass - the sign, the logo, the poster on a wall. A
    // building's material shader carries Decal_Map too, but behind Diffuse_Map, so testing the
    // FIRST sampler names the decal pass and never the wall. 32 such draws a frame, measured.
    bool decalPrimaryOffset = true;
    // Which sampler decalPrimaryOffset above tests. 1 = lowestSampler, the sampler bound to the
    // LOWEST register - what the shader actually samples first when it runs. 0 = the old test,
    // firstSampler, which is only whichever sampler name sorts ALPHABETICALLY first in D3DX's
    // CTAB and is a different sampler entirely: measured, 400 pixel shaders report a Decal map
    // alphabetically first while only 126 of them actually have one at the lowest register - the
    // other 274 are ordinary wall/building materials. Kept togglable so the two can be compared
    // in a capture without a rebuild.
    bool decalByLowestSampler = true;
    // A draw that really blends (by the FACTORS - SR3 leaves ALPHABLENDENABLE on with ONE/ZERO
    // for opaque draws) is a layer over whatever is behind it.
    bool blendedOffset = true;
    bool rejectStaleBones = false; // drop bone influences belonging to a different object; OFF by default as of 2026-09-17, synced to the deployed ini
    bool clampBonesToUpload = false; // ignore bones outside this object's own palette upload; OFF by default as of 2026-09-17, synced to the deployed ini
    bool vehicleBonesOff = false;    // single-bone draws: ignore the palette, use objTM alone
    bool paletteSetupScope = false;  // only pose by a palette published in this draw's own setup; OFF by default as of 2026-09-17, synced to the deployed ini
    float clothTintScale = 2.0f; // clothAlbedoPercent/100; see the ini - default 2.0 (200%) as of 2026-09-17, synced to the deployed ini
    // OFF. Three attempts, three regressions, and no run in which it demonstrably reduced the
    // double-draw it was written for. See the ini for the full history. Kept as a switch because
    // the analysis is sound and the key is now complete; it simply has never paid for itself.
    // Drop a skinned draw whose identical triangles, in the identical POSE, were already
    // converted this frame.
    //
    // Twice wrong before this. First the key omitted the index range, so every material
    // sub-range of one mesh looked like a duplicate. Then it omitted the pose, so two NPCs in the
    // same garment looked like one object and the second lost its clothing. The key now folds in
    // eight bone matrices, which is what makes "the same object" mean the same object.
    // 2026-09-17: the deployed ini nonetheless has this ON; the default here is synced to match
    // it rather than assume off. The "never paid for itself" history above still stands - this
    // sync doesn't endorse re-enabling it, only ends the code/ini disagreement.
    bool dedupSkinned = true;
    // Extend the same duplicate test to NON-skinned draws. Measured 2026-08-27: 94 redundant
    // converted copies a frame, and only 4 of them were skinned.
    // Default ON as of 2026-09-17, synced to the deployed ini (see dedupSkinned above - same caveat).
    bool dedupAll = true;
    // Apply the per-character morph delta from stream 2. 0 renders the base mesh, which is what
    // every build before 2026-08-27 did.
    bool applyMorph = true;
    bool tintFallbackAlbedo = false;  // OFF: measured to darken clothing, see SetupTextureStages
    // RESET TEXTUREFACTOR ON A NORMAL TEXTURED DRAW. SetupTextureStages' own contract, stated in
    // the comment above BeginFFP's D3DRS_LIGHTING block, is that TEXTUREFACTOR "holds either the
    // opaque alpha for a normal draw or the base colour of a constant-coloured material, and it is
    // written per converted draw. One register, one owner." The constant-colour and tint-fallback
    // branches below DO write it. The ordinary textured branch never did - so a normal draw
    // inherited whatever the LAST constant-colour draw left in the register.
    //
    // That matters because Remix reads the register for EVERY draw
    // (d3d9_rtx_utils.cpp:175, materialData.tFactor = renderStates[D3DRS_TEXTUREFACTOR]) and turns
    // on texture-factor blending whenever any live texture stage's args reference D3DTA_TFACTOR
    // (d3d9_rtx.cpp:963). A stale colour left in that register therefore tints a textured surface
    // whose own stage setup never asked for it, and WHICH draw leaks onto which depends on draw
    // ORDER - so the tint changes as the camera moves and turns, which is exactly the reported
    // symptom: same texture (confirmed with Remix's object picker), detail intact, only the tint
    // changing, distance and angle dependent.
    //
    // Opaque white is the identity for both uses: as a colour it is a no-op modulate, and its
    // alpha is the opaque alpha the contract asks for. 0 restores the leak for an A/B.
    bool resetTFactorOnTexturedDraws = true;
    bool skinRigidSingleBone = true;  // ON by default as of 2026-09-17, synced to the deployed ini - 0 means cars do not render; the "correlated with clothing vanishing" concern that shipped this OFF is unresolved, see GetBaseMesh
    // Any frame at or above this many ms writes one line partitioning where the time went.
    // 40 ms is ~2.3 frames at the measured 58 fps: long enough that the player sees a hitch,
    // short enough to catch the 100 ms ones with room to spare.
    int hitchMs = 40;
    int probeVerts[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    int probeVertCount = 0;
    // THE RASTERIZED-FRAME INVARIANT: a draw that SAMPLES a render target on any bound stage
    // AND WRITES a screen-sized target (render target 0 exactly the back buffer's own size)
    // must never reach the device unless Remix is already guaranteed to decline it. This is the
    // permanent, path-independent fix for the user's stated top priority - "at some camera
    // angles, the rasterized render of the game replaces the scene" - a bug this project has
    // fixed before on one path while it returned through another. See
    // CompositesEngineOutputToScreen (defined with g_rtTextures, well above Classify) for the
    // predicate itself and why both halves are needed to leave the character atlas composite
    // untouched. Default ON: this is a correctness invariant, not a tuning knob. 0 restores
    // today's per-path behaviour for an A/B.
    bool blockEngineOutputToScreen = true;
    // injectProbe: MEASURE ONLY where the shim's own fixed-function draws satisfy Remix's OWN
    // UI-classification trigger for RtxContext::injectRTX (read out of remix-1.5.2's
    // d3d9_rtx.cpp, not guessed - see EvaluateInjectTrigger, defined beside the marker texture
    // section below). blockEngineOutputToScreen above is this project's EARLIER attempt at the
    // same user-reported bug, from a different angle (catch the game's rasterised composite
    // before it reaches the device); injectProbe and injectControl are the diagnosis Remix's
    // own source gives instead, and the two are independent - this one changes no render state
    // and no pixel, so there is no reason to ship it off. Default ON.
    bool injectProbe = true;
    // injectControl: THE FIX. (a) suppresses an accidental early trigger from one of the
    // shim's own PRETRANSFORMED (RHW) draws by forcing D3DTS_PROJECTION off-identity for just
    // that one draw - invisible, since RHW vertices ignore the transform pipeline for
    // rendering and Remix's classifier reads the transform, not the vertex; (b) issues the
    // GTA4-RTX-mod's own deliberate, invisible trigger quad once per frame, immediately after
    // the game's final composite and before the first HUD draw, so injectRTX fires at the
    // RIGHT moment on every frame instead of by camera-angle accident. Default ON: this is the
    // permanent fix, not a tuning knob.
    bool injectControl = true;
    // STRETCH DETECT: hunts the one-frame "world decals stretch across the screen" bug by
    // finding the CONVERTED draw that does it and dumping everything about it - see
    // ProbeStretchDetect's own comment, above Hook_DrawIndexedPrimitive, for the full story.
    // OFF by default: this is a diagnostic aimed at one bug, not a permanent behaviour.
    bool stretchDetect = false;
    // World-space AABB diagonal (game units) above which a converted draw is "stretched".
    // Steelport's own geometry runs to the low thousands (MeasureWorldExtent's own comment,
    // above); 5000 is comfortably past any real object while still well short of "the whole
    // map", which is what this bug actually produces.
    int stretchDetectUnits = 5000;
    // Hard cap on dump BLOCKS written to the log, not on detection itself - flagged draws are
    // still counted past this cap (see g_stretchDetectFlagged), only the Log() calls stop, so a
    // systemic false positive cannot flood the log the way an uncapped version would.
    int stretchDetectDumps = 20;

    // hideShaderContains - DIAGNOSTIC bisection switch, not a fix. This project is hunting a
    // TRANSIENT artifact: a large flat polygon that appears for a frame or two, stretching
    // across the screen and intersecting buildings. Aggregate measurements have repeatedly
    // pointed at populations that turned out not to be the thing on screen, so instead of
    // measuring again this ELIMINATES: name a shader family by its ALBEDO SAMPLER here (a
    // comma-separated list of substrings) and every draw that carries it is hidden -
    // Disp::Hide, the same disposition HiddenDisp's case 1 already uses, so the draw still
    // executes for the engine but never reaches the path tracer - instead of converted. Run
    // with and without a name in this list and see whether the artifact stops. EMPTY by
    // default: the feature is then completely inert (see the check inside Classify, which
    // costs one length test when this list is empty). See that check for why the match is
    // case-sensitive.
    static constexpr int kMaxHideShaderTerms = 8;
    char hideShaderTerms[kMaxHideShaderTerms][32] = {};  // trimmed substrings, parsed in LoadSettings
    int hideShaderTermCount = 0;
} g_settings;

void LoadSettings() {
    char path[MAX_PATH]{};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    if (char* slash = strrchr(path, '\\')) strcpy_s(slash + 1, 32, "sr3-rtx.ini");

    // Detect a key ABSENT from the ini rather than silently falling back - a missing or
    // misspelt key was previously indistinguishable from an explicit one, and the startup log
    // said nothing about it. The sentinel is never a legal value for any key here (every key in
    // this file is numeric or boolean), so an unchanged buffer means GetPrivateProfileStringA
    // used ITS default, i.e. the key was not found.
    int iniPresentCount = 0, iniAbsentCount = 0;
    auto checkPresent = [&](const char* key) {
        char sentinel[8];
        GetPrivateProfileStringA("sr3-rtx", key, "\x01", sentinel, sizeof(sentinel), path);
        const bool present = !(sentinel[0] == '\x01' && sentinel[1] == '\0');
        if (present) ++iniPresentCount; else ++iniAbsentCount;
        return present;
    };

    auto flag = [&](const char* key, bool def) {
        if (!checkPresent(key))
            Log("ini: key '%s' ABSENT - using built-in default %d", key, def ? 1 : 0);
        return GetPrivateProfileIntA("sr3-rtx", key, def ? 1 : 0, path) != 0;
    };
    auto num = [&](const char* key, int def) {
        if (!checkPresent(key))
            Log("ini: key '%s' ABSENT - using built-in default %d", key, def);
        return GetPrivateProfileIntA("sr3-rtx", key, def, path);
    };

    g_settings.ffp = flag("ffp", true);
    g_settings.convertSkinned = flag("convertSkinned", true);
    g_settings.mainCameraScreenSizedOnly = flag("mainCameraScreenSizedOnly", true);
    g_settings.skipMirrored = flag("skipMirrored", true);
    g_settings.cacheMeshAlbedo = flag("cacheMeshAlbedo", false);
    g_settings.skipUntextured = flag("skipUntextured", true);
    g_settings.skipNonColourTargets = flag("skipNonColourTargets", true);
    g_settings.skipDeferredGBuffer = flag("skipDeferredGBuffer", true);
    g_settings.skipMaskOnlyVfx = flag("skipMaskOnlyVfx", true);
    g_settings.screenSpaceMode = num("screenSpaceMode", 0);
    g_settings.uiDemoteUP = flag("uiDemoteUP", true);
    g_settings.uiConvertUP = flag("uiConvertUP", true);
    g_settings.uiRawAfterHud = flag("uiRawAfterHud", false);
    g_settings.uiKeepPixelShader = flag("uiKeepPixelShader", true);
    g_settings.compositeFfp = flag("compositeFfp", true);
    g_settings.clothUniformFromDiffuse = flag("clothUniformFromDiffuse", true);
    g_settings.hairStrandsFromDob = flag("hairStrandsFromDob", true);
    g_settings.hairHsvAlbedo = flag("hairHsvAlbedo", true);
    g_settings.skinMatcapAlbedo = flag("skinMatcapAlbedo", true);
    g_settings.skinGenPerFrame = num("skinGenPerFrame", 2);
    g_settings.skinSingleMatcap = flag("skinSingleMatcap", true);
    g_settings.clothDominantPattern = flag("clothDominantPattern", true);
    g_settings.clothPadUnused = flag("clothPadUnused", true);
    g_settings.clothCutout = flag("clothCutout", true);
    g_settings.clothMeshDecal = flag("clothMeshDecal", true);
    g_settings.clothDecalTiles = flag("clothDecalTiles", true);
    g_settings.clothColourCurve = flag("clothColourCurve", true);
    g_settings.uvScaleFromShader = flag("uvScaleFromShader", false);
    g_settings.passCensus = flag("passCensus", true);
    g_settings.atlasCompositeProbe = flag("atlasCompositeProbe", true);
    g_settings.compositeToTexturePass = flag("compositeToTexturePass", false);
    g_settings.markAuxCamera = flag("markAuxCamera", true);
    g_settings.clearBackBuffer = flag("clearBackBuffer", true);
    g_settings.hideLightVolumes = flag("hideLightVolumes", true);
    g_settings.injectLights = flag("injectLights", true);
    g_settings.rankAlbedo = flag("rankAlbedo", true);
    g_settings.excludeRTAlbedo = flag("excludeRTAlbedo", true);
    g_settings.albedoRejectColourlessTexture = flag("albedoRejectColourlessTexture", true);
    g_settings.albedoChurnProbe = flag("albedoChurnProbe", true);
    g_settings.logLayouts = flag("logLayouts", true);
    g_settings.lightScale = num("lightScalePercent", 100) / 100.0f;
    g_settings.lightRangeScale = num("lightRangePercent", 100) / 100.0f;
    g_settings.dumpFrame = num("dumpFrame", 60);
    g_settings.shapeProbe = flag("shapeProbe", true);
    g_settings.hitchMs = num("hitchMs", 40);
    g_settings.skinProbe = flag("skinProbe", true);
    g_settings.rigidSkinProbe = flag("rigidSkinProbe", true);
    g_settings.remixShortUV = flag("remixShortUV", true);
    g_settings.uvHonourStreamOffset = flag("uvHonourStreamOffset", true);
    g_settings.decalUvFromDeclaredSet = flag("decalUvFromDeclaredSet", true);
    g_settings.decalUvScaleFromSet = flag("decalUvScaleFromSet", true);
    g_settings.decalAlphaFromTexture = flag("decalAlphaFromTexture", true);
    g_settings.decalProbe = flag("decalProbe", true);
    g_settings.decalProbeDumpTexture = flag("decalProbeDumpTexture", true);
    g_settings.decalProbeStrictTarget = flag("decalProbeStrictTarget", true);
    g_settings.deinstanceConverted = flag("deinstanceConverted", false);
    g_settings.clothProbe = flag("clothProbe", true);
    g_settings.diffuseColorProbe = flag("diffuseColorProbe", true);
    g_settings.rtAlbedoCopy = flag("rtAlbedoCopy", true);
    g_settings.atlasSnoop = flag("atlasSnoop", true);
    g_settings.snoopValidateSource = flag("snoopValidateSource", true);
    g_settings.snoopThreadScopedInternal = flag("snoopThreadScopedInternal", true);
    g_settings.vbUnlockOwnerThread = flag("vbUnlockOwnerThread", true);
    g_settings.cameraOnly = flag("cameraOnly", false);
    g_settings.cameraOnlyFloatUV = flag("cameraOnlyFloatUV", false);
    g_settings.cameraMainViewOnly = flag("cameraMainViewOnly", true);
    g_settings.convertDynamicUV = flag("convertDynamicUV", true);
    g_settings.remixApi = flag("remixApi", true);
    g_settings.remixApiCamera = flag("remixApiCamera", false);
    g_settings.remixApiTestCube = flag("remixApiTestCube", false);
    g_settings.remixApiCharacter = flag("remixApiCharacter", false);
    g_settings.remixApiSkinning = flag("remixApiSkinning", false);
    g_settings.remixApiClothAlbedo = flag("remixApiClothAlbedo", false);
    g_settings.bakeShaderAlbedo = flag("bakeShaderAlbedo", false);
    g_settings.clothUseDiffuse = flag("clothUseDiffuse", true);
    g_settings.clothBrightness =
        static_cast<float>(num("clothBrightness", 100)) / 100.0f;
    g_settings.remixApiCharacterOffset =
        static_cast<float>(num("remixApiCharacterOffset", 3));
    g_settings.scanCommandBlocks = flag("scanCommandBlocks", false);
    g_settings.rtContentProbe = flag("rtContentProbe", false);
    g_settings.rtAlbedoRetries = num("rtAlbedoRetries", 3000);
    g_settings.generateCloth = flag("generateCloth", true);
    g_settings.clothDump = flag("clothDump", false);
    g_settings.charTexDump = flag("charTexDump", false);
    g_settings.captureKey = num("captureKey", 0x78);
    g_settings.crashTestDumpAtStart = flag("crashTestDumpAtStart", false);
    g_settings.skinRequireBoneDecl = num("skinRequireBoneDecl", 1);
    g_settings.skinRingMB = num("skinRingMB", 24);
    g_settings.forceOcclusionVisible = flag("forceOcclusionVisible", true);
    g_settings.skipOcclusionProxies = flag("skipOcclusionProxies", true);
    g_settings.occlusionProxyMaxPrims = static_cast<int>(num("occlusionProxyMaxPrims", 12));
    g_settings.skipDeclinedPassThrough = flag("skipDeclinedPassThrough", true);
    g_settings.skinFast = flag("skinFast", true);
    g_settings.skinThreads = static_cast<int>(num("skinThreads", 1));
    g_settings.skinViaFixedFunction = flag("skinViaFixedFunction", false);
    g_settings.skinFFRestageBone0 = flag("skinFFRestageBone0", true);
    g_settings.skinFFHonourStreamOffset = flag("skinFFHonourStreamOffset", true);
    g_settings.skinFFRigidDecl = flag("skinFFRigidDecl", true);
    g_settings.skinFFFollowShaderInfluences = flag("skinFFFollowShaderInfluences", true);
    g_settings.dietVsConstants = flag("dietVsConstants", true);
    g_settings.dietVsBinds = flag("dietVsBinds", true);
    g_settings.occlusionCull = flag("occlusionCull", true);
    g_settings.occlusionDryRun = flag("occlusionDryRun", false);
    g_settings.occlusionMaxOccluders = static_cast<int>(num("occlusionMaxOccluders", 2000));
    g_settings.occlusionMaxTris = static_cast<int>(num("occlusionMaxTris", 250000));
    g_settings.occlusionMeshBudget = static_cast<int>(num("occlusionMeshBudget", 16));
    g_settings.occlusionWidth = static_cast<int>(num("occlusionWidth", 320));
    g_settings.occlusionMinDistance = static_cast<int>(num("occlusionMinDistance", 5));
    g_settings.occlusionKeyByObject = flag("occlusionKeyByObject", true);
    g_settings.occlusionSelfToleranceCm = static_cast<int>(num("occlusionSelfToleranceCm", 50));
    g_settings.occlusionExplainFlips = static_cast<int>(num("occlusionExplainFlips", 40));
    g_settings.indexedInvalidation = flag("indexedInvalidation", true);
    g_settings.occlusionRangeInvalidation = flag("occlusionRangeInvalidation", true);
    g_settings.occlusionStickyOccluders = flag("occlusionStickyOccluders", true);
    g_settings.probeIndexWindow = flag("probeIndexWindow", true);
    g_settings.tightenConvertedWindow = flag("tightenConvertedWindow", false);
    g_settings.tightenWindowPinIndexBuffers = flag("tightenWindowPinIndexBuffers", true);
    g_settings.tightWinEvictByAge = flag("tightWinEvictByAge", true);
    g_settings.widenUnderDeclaredWindow = flag("widenUnderDeclaredWindow", true);
    g_settings.indexWindowProbe = flag("indexWindowProbe", true);
    g_settings.lockThreadProbe = flag("lockThreadProbe", true);
    g_settings.headBodyProbe = flag("headBodyProbe", true);
    g_settings.indexWindowClamp = flag("indexWindowClamp", false);
    g_settings.indexWindowClampStrips = flag("indexWindowClampStrips", false);
    g_settings.orphanSweep = flag("orphanSweep", true);
    g_settings.orphanSweepFrames = static_cast<int>(num("orphanSweepFrames", 120));
    g_settings.orphanSweepReportOnly = flag("orphanSweepReportOnly", false);
    g_settings.orphanSweepShadersDecls = flag("orphanSweepShadersDecls", false);
    g_settings.shadowCheckProbe = flag("shadowCheckProbe", false);
    g_settings.hudProbe = flag("hudProbe", false);
    g_settings.deferFreshStaticDraws = flag("deferFreshStaticDraws", true);
    g_settings.deferFreshFrames = static_cast<int>(num("deferFreshFrames", 1));
    g_settings.ringFrames = static_cast<int>(num("ringFrames", 60));
    g_settings.assetHashProbe = flag("assetHashProbe", true);
    g_settings.assetHashProbeFrames = static_cast<int>(num("assetHashProbeFrames", 60));
    g_settings.assetHashProbeWorld = flag("assetHashProbeWorld", false);
    g_settings.assetHashProbeAlbedo = flag("assetHashProbeAlbedo", true);
    g_settings.frameStepPauseKey = static_cast<int>(num("frameStepPauseKey", 0x76));
    g_settings.frameStepKey = static_cast<int>(num("frameStepKey", 0x77));
    g_settings.frameStepDumpEachStep = flag("frameStepDumpEachStep", true);
    g_settings.decalOffsetPermille = static_cast<int>(num("decalOffsetPermille", 0));
    g_settings.layerOffset = flag("layerOffset", true);
    g_settings.layerOffsetPermille = static_cast<int>(num("layerOffsetPermille", 0));
    g_settings.biasOffset = flag("biasOffset", true);
    g_settings.decalPrimaryOffset = flag("decalPrimaryOffset", true);
    g_settings.decalByLowestSampler = flag("decalByLowestSampler", true);
    g_settings.blendedOffset = flag("blendedOffset", true);
    g_settings.rejectStaleBones = flag("rejectStaleBones", false);
    g_settings.clampBonesToUpload = flag("clampBonesToUpload", false);
    g_settings.vehicleBonesOff = flag("vehicleBonesOff", false);
    g_settings.paletteSetupScope = flag("paletteSetupScope", false);
    g_settings.clothTintScale = num("clothAlbedoPercent", 200) / 100.0f;
    g_settings.dedupSkinned = flag("dedupSkinned", true);
    g_settings.dedupAll = flag("dedupAll", true);
    g_settings.applyMorph = flag("applyMorph", true);
    g_settings.morphBake = flag("morphBake", false);
    g_settings.morphBakeMaxMB = num("morphBakeMaxMB", 24);
    g_settings.morphBakeMaxPerFrame = num("morphBakeMaxPerFrame", 2);
    g_settings.morphProbe = flag("morphProbe", false);
    g_settings.tintFallbackAlbedo = flag("tintFallbackAlbedo", false);
    g_settings.resetTFactorOnTexturedDraws = flag("resetTFactorOnTexturedDraws", true);
    g_settings.skinRigidSingleBone = flag("skinRigidSingleBone", true);
    g_settings.hiddenPassMode = num("hiddenPassMode", 2);
    g_settings.blockEngineOutputToScreen = flag("blockEngineOutputToScreen", true);
    g_settings.injectProbe = flag("injectProbe", true);
    g_settings.injectControl = flag("injectControl", true);
    g_settings.stretchDetect = flag("stretchDetect", false);
    g_settings.stretchDetectUnits = static_cast<int>(num("stretchDetectUnits", 5000));
    g_settings.stretchDetectDumps = static_cast<int>(num("stretchDetectDumps", 20));
    {
        char list[128]{};
        if (!checkPresent("probeVerts"))
            Log("ini: key 'probeVerts' ABSENT - using built-in default (empty list)");
        GetPrivateProfileStringA("sr3-rtx", "probeVerts", "", list, sizeof(list), path);
        char* ctx = nullptr;
        for (char* tok = strtok_s(list, ", \t", &ctx);
             tok && g_settings.probeVertCount < 8; tok = strtok_s(nullptr, ", \t", &ctx)) {
            const int v = atoi(tok);
            if (v > 0) g_settings.probeVerts[g_settings.probeVertCount++] = v;
        }
    }
    {
        // Reuses the exact mechanism probeVerts (immediately above) reads its list with -
        // GetPrivateProfileStringA into a local buffer, then split by hand - because that is
        // the only string-list reader this file already has; inventing a second one for the
        // same job would be the thing worth avoiding. Unlike probeVerts this is SUBSTRINGS,
        // not numbers, and split on ',' ONLY (not space/tab too, unlike probeVerts), so
        // trimming below is unambiguous about what is delimiter and what is whitespace to
        // strip from an entry's ends.
        char list[256]{};
        if (!checkPresent("hideShaderContains"))
            Log("ini: key 'hideShaderContains' ABSENT - using built-in default (empty - feature inert)");
        GetPrivateProfileStringA("sr3-rtx", "hideShaderContains", "", list, sizeof(list), path);
        char* ctx = nullptr;
        for (char* tok = strtok_s(list, ",", &ctx);
             tok && g_settings.hideShaderTermCount < Settings::kMaxHideShaderTerms;
             tok = strtok_s(nullptr, ",", &ctx)) {
            while (*tok == ' ' || *tok == '\t') ++tok;              // trim leading whitespace
            size_t len = strlen(tok);
            while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t' ||
                                tok[len - 1] == '\r' || tok[len - 1] == '\n'))
                tok[--len] = '\0';                                  // trim trailing whitespace
            if (len > 0)   // ignore empty entries (",," or a trailing comma)
                strncpy_s(g_settings.hideShaderTerms[g_settings.hideShaderTermCount++], tok,
                          sizeof(g_settings.hideShaderTerms[0]) - 1);
        }
    }

    // This exists to end a silent failure: a missing or misspelt ini key used to look exactly
    // like an explicit one, with nothing in the log to tell them apart. A clean run is worth
    // stating for the same reason - it says the ini was actually read, not merely present.
    if (iniAbsentCount == 0)
        Log("ini: all %d keys present in sr3-rtx.ini", iniPresentCount);
    else
        Log("ini: %d of %d keys ABSENT from sr3-rtx.ini (see lines above; built-in defaults used)",
            iniAbsentCount, iniPresentCount + iniAbsentCount);
}

