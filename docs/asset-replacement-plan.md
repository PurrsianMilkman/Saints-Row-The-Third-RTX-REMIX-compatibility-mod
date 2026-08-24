# Asset replacement with the Remastered assets

Goal, stated 2026-08-20: replace world and character assets with Saints Row: The Third Remastered
versions, **while keeping character customisation working**. Functionality first; performance is a
later pass.

## The one structural fact that shapes everything

**There are two substitution points, and characters need a different one from the world.**

| asset | where the substitution happens | why |
|---|---|---|
| world, props, vehicles, architecture | **inside Remix**, keyed on texture / geometry hash | the game's texture IS the final albedo, so replacing it replaces the result |
| character clothing | **inside our shim**, on the *input* | the final albedo is COMPUTED AT RUNTIME from the player's chosen colours, so there is no fixed texture for Remix to key on |

Clothing colour is chosen by the player. A generated texture's hash is stable *per colour
combination*, so a Remix replacement keyed to it would apply to one colour and no other. Replacing
the **Pattern_Map input** instead sidesteps that completely: the remastered detail comes through,
the colours stay live, and nothing downstream needs to key on a hash that varies.

This is the answer to "how do I have both customisation and remastered assets". It is not a
compromise between the two - the substitution simply happens one step earlier.

## Where we already are

| foundation | state |
|---|---|
| world texture coordinates readable by Remix | done - float2 conversion, run 45 |
| tiling correct | done - matched to the albedo map, run 46 |
| texture hashes stable | done - they were never the problem |
| geometry ASSET hashes stable across animation | done - `geometryAssetHashRuleString = indices,geometrydescriptor`, run 47 |
| replacements enabled | done - `rtx.enableReplacementAssets` was False and is now True |
| Remastered packfiles readable | **verified** - magic `0x51890ACE`, version 6, the same VPP_PC v6 `tools/vpp_extract.py` already reads |

Captures taken before 2026-08-20 are **stale for geometry** (the vertex layout and the asset hash
rule both changed). Texture hashes in them still hold. Recapture before authoring.

## Phase 1 - get the assets out

`F:\SteamLibrary\steamapps\common\Saints Row The Third Remastered\cache\` holds 44 packfiles. The
ones that matter here:

    characters.vpp_pc  characters_high.vpp_pc
    customize_item.vpp_pc  customize_player.vpp_pc
    high_mips_0.vpp_pc  high_mips_1.vpp_pc

Textures inside are Volition bitmap pairs - a `.cvbm_pc` header chunk and a `.gvbm_pc` GPU data
chunk. Extracting the packfile is solved; **converting the bitmap pair to DDS is not, and is the
first piece of new tooling.** The header carries format, dimensions and mip layout; the payload is
already block-compressed, so this is a container rewrite rather than a transcode.

## Phase 2 - the real bottleneck: which hash is which asset

Remix matches replacements by hash. To replace assets in bulk we must know **which hash corresponds
to which named asset**, and neither side offers that:

- Remix's menu shows hashes and lets you tag them by eye. Fine for tens of textures. There are
  thousands.
- The packfiles give names and content, but not the hash Remix will compute at runtime.

The bridge between them has to be built, and there are two candidate routes:

1. **Hook the game's texture loader.** The name is present at load time inside the engine; capture
   it alongside the D3D9 texture pointer, and the shim already knows pointer to bound-texture. This
   is what the vibe-RE toolkit's static analysis is genuinely good for - find the packfile loader,
   find where it passes a name, hook it. Note the standing caveat: SR3 is data-driven and the
   PE scanners found nothing for skinning, so expect to work from strings and xrefs rather than
   from the D3D9 scanners.
2. **Reproduce Remix's hash offline.** Compute the legacy texture hash over the extracted original
   asset and match it against what Remix reports. Removes the need for a runtime hook entirely, but
   depends on reproducing the hash exactly, including which mip levels participate.

Route 1 is more certain; route 2 is more useful if it works, because it turns replacement into a
purely offline batch job. **Try 2 first on a handful of textures - it is cheap to test and the
payoff is large.**

Until this exists, replacement is limited to whatever can be hand-tagged.

## Phase 3 - the world

Standard Remix replacement, and unblocked today:

1. recapture (old captures are stale)
2. author a USD per asset with the remastered albedo, normal, roughness and **height**
3. POM windows: the height map drives `rtx.displacement` - this build has both `RaymarchPOM` and
   `QuadtreePOM`, with `displaceIn`/`displaceOut` factors

Modelled window interiors are explicitly deferred in favour of POM.

## Phase 4 - characters

**Albedo** - runtime texture generation, with the remastered Pattern_Map as the input rather than
the game's. The recipe is read from `ir_sr3npcclothfull_c.fxo_pc` shader [8] and recorded in
`configs/sr3-rtx.ini` under `clothProbe`. Design settled: CPU generation, 256-entry gamma lookup
tables (exact - every `pow` input is an 8-bit channel), cached per (pattern, a, b, c, tint), one
generation per frame, capped with a flush.

**Normal, roughness, specular** - these cannot travel over fixed function, which carries exactly
one texture. Two routes:

- a Remix replacement material keyed on the albedo hash. **Does not work for clothing**, because
  that hash varies with the player's colours - the same reason the substitution moved upstream;
- **the Remix API.** `RemixApi_CreateMaterial` + `CreateMesh` + `DrawInstance`, reachable from our
  32-bit process through the bridge client's exported `remixapi_InitializeLibrary`.

The API was set aside on 2026-08-20 on performance grounds: it has no mesh update entry point, so
animated characters mean destroy-and-create per mesh per frame across the 32-to-64-bit bridge.
**That objection was about cost, not capability, and the stated priority is now functionality
first.** It is therefore back on the table as the only route to full PBR characters with live
customisation, and it should be validated early rather than late - it is experimental, two of its
ten commands are stubs, and its interface leaves ten of twenty-two slots null.

Gate: `exposeRemixApi = True` in a `bridge.conf` that does not yet exist in the game directory.

**Character MESH replacement is a separate question.** Remix cannot skin a replacement mesh, so a
static replacement would freeze a character in bind pose. The only route that could work is
submitting remastered geometry *skinned by us* through the API - we already skin on the CPU, and
the rigs are shared between the two versions. Promising, unvalidated, and dependent on the API
question above.

## Recommended order

1. **Validate the Remix API** - `bridge.conf`, initialise, create one material, draw one instance.
   Everything in phase 4 hangs off whether this works, and it is a day's work to find out rather
   than discovering it after the asset pipeline is built.
2. **Try reproducing Remix's texture hash offline** on a few extracted originals. If it works,
   phase 2 collapses from a reverse-engineering job into a script.
3. **Bitmap pair to DDS converter** - needed by every later phase.
4. Cloth generator, using the probe's answers about read access and combination count.
5. World replacement and POM, which need no new capability - only assets and captures.

Steps 1 and 2 are both cheap and both decide the shape of the work that follows. Neither should be
deferred behind the asset pipeline.

---

## Update, 2026-08-21 - what the intervening sessions changed

**Two prerequisites are now met and one new one appeared.**

Met:

- **World texturing works.** SHORT2 texcoords are converted to a float2 stream, so Remix reads them;
  tiling is matched to the albedo map rather than the normal map. Texture hashes are stable.
- **Geometry asset hashes are stable across animation** (`geometryAssetHashRuleString =
  indices,geometrydescriptor`), confirmed by the user on NPCs, the player and building windows.
  Captures taken before 2026-08-20 are stale for geometry; texture hashes in them still hold.

New, and it gates authoring:

- **`rtx.useVertexCapture` must be turned OFF**, because it is the only mechanism that stops Remix
  drawing a second, shader-derived copy of every unconverted pass. Until the three blockers in
  YOUR-INSTRUCTIONS are cleared - stability, UI, sky - the scene contains duplicate geometry, and
  authoring replacements against a scene with duplicates is authoring against the wrong thing.

**The Remix API question is settled and the answer is no**, for characters. It is reachable from
our own 32-bit process and needs only `exposeRemixApi = True` in a `bridge.conf`, but every resource
is a Create/Destroy pair with **no update entry point anywhere**, so animated meshes mean
destroy-and-create per mesh per frame across the bridge. It remains right for static replacement
geometry, for lights, and for `SetConfigVariable`. It also disposes of the fork option: a fork was
only ever worth considering for per-texel material maths, and runtime texture generation reaches
that from outside with no upstream drift.

**Runtime texture generation is proven on the NPC clothing family** - byte-exact against the
shader, 21 texel classes checked, zero mismatches - which is the evidence that the approach works
for the player family too once its second UV set is reconciled.

**POM is confirmed available**: `rtx.displacement` with `RaymarchPOM` and `QuadtreePOM`, and
`rtx.enableReplacementAssets` has been switched on (it was False, which would have silently
prevented every replacement from loading).

### Revised order

1. Clear the three capture-off blockers. Nothing downstream is trustworthy until the scene contains
   one copy of each object.
2. Bitmap-pair (`.cvbm_pc` / `.gvbm_pc`) to DDS converter - still the first piece of new tooling,
   and still needed by every later phase.
3. The hash-to-asset-name bridge, still the real bottleneck for bulk replacement. Try reproducing
   Remix's texture hash offline first; it turns the job into a script if it works.
4. World replacement and POM, which need no new capability - only assets and fresh captures.
