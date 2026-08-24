# Credits

This project is a thin layer on top of a lot of other people's work. Almost nothing here is
original: the rendering approach is borrowed, the runtime is NVIDIA's, the reverse-engineering
tooling is someone else's, and the engine knowledge came out of reading other projects' notes.

**If you are listed here and want the wording changed, or a link corrected — or if you should be
listed and are not — please open an issue. Credit that is missing is a bug, and it will be fixed.**

---

## RTX Remix, and NVIDIA

Without RTX Remix none of this exists. It is the entire reason the project is possible.

- **[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix)** — the runtime and toolkit.
  Everything this shim does is in service of handing Remix geometry it can path-trace.
- **[dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix)** — the renderer inside Remix.
  Its binary also turned out to be the best documentation available: every option is registered
  by name with its description as an adjacent string, and reading those strings settled more open
  questions in this project than any other single source.
- **[DXVK](https://github.com/doitsujin/dxvk)** — Philip Rebohle (doitsujin) and contributors.
  Remix's D3D9 implementation is built on it.
- **[d3d8to9](https://github.com/crosire/d3d8to9)** — crosire, shipped with the Remix bridge.
- The **RTX Remix Showcase Discord** community, whose game compatibility table and collective
  troubleshooting knowledge saved a great deal of wasted effort.

**Thank you to RTX Remix and NVIDIA.**

## The lineage this shim is ported from

The fixed-function conversion approach — null both shaders, hand D3D9 real WORLD/VIEW/PROJECTION
matrices through `SetTransform`, bind the real albedo to stage 0, draw, restore — is **not this
project's idea**. It is inherited, and it is the single most important thing here.

### Saints Row 2 — RTX Remix FFP proxy

**[BRAGme/sr2-rtx-remix-proxy](https://github.com/BRAGme/sr2-rtx-remix-proxy)** (MIT) — the direct
ancestor. It solves the same problem on Saints Row 2: same studio, same engine lineage, and as it
turned out the same bone palette layout (c52, three registers per bone). This project was
restarted around that design after months of heuristics that did not work, and the design is
theirs. SR3 is the easier of the two targets — SR2 has to *analytically decompose* a fused W·V·P
matrix, while SR3 hands us the view matrix outright.

The people behind it:

- **[BRAGme](https://github.com/BRAGme)** — the SR2 proxy itself, which is the specific thing this
  project is a port of.
- **[xoxor4d](https://github.com/xoxor4d)** — `remix-comp-base`, the original that the SR2 proxy
  derives from, plus [`p2-rtx`](https://github.com/xoxor4d/p2-rtx),
  [`gta4-rtx`](https://github.com/xoxor4d/gta4-rtx) and
  [`remix-comp-projects`](https://github.com/xoxor4d/remix-comp-projects). A large part of the
  practical knowledge about making a shader-driven DX9 game legible to Remix comes from this body
  of work.
- **[Kim2091](https://github.com/Kim2091)** — adapted `remix-comp-base` into the form the SR2 proxy
  took, and also maintains a `dxvk-remix` fork in the same lineage.

The SR2 proxy's MIT notice carries two copyright lines, `Copyright (c) 2026 github.com/xoxor4d`
and `Copyright (c) 2026 Kim2091`. Both are reproduced in full in [LICENSE](LICENSE).

The SR2 proxy in turn builds on work this project does not vendor but which made that
implementation possible, and which deserves naming here:

- **[Dear ImGui](https://github.com/ocornut/imgui)** — Omar Cornut and contributors.
- **[MinHook](https://github.com/TsudaKageyu/minhook)** — Tsuda Kageyu.
- **[RemixProjGroup/dxvk-remix](https://github.com/RemixProjGroup/dxvk-remix)** — the `numos3`
  branch, providing procedural sky and weather, forked from
  [Kim2091/dxvk-remix](https://github.com/Kim2091/dxvk-remix) and ultimately from
  NVIDIA's [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix).

## Reverse-engineering tooling

### Vibe-Reverse-Engineering

**[Ekozmaster/Vibe-Reverse-Engineering](https://github.com/Ekozmaster/Vibe-Reverse-Engineering)**
(MIT, `Copyright 2026 Emanuel Kozerski`) — static analysis (`retools`), Frida-based dynamic
analysis (`livetools`), a full D3D9 call tracer, and `rtx_remix_tools`. It also ships a
`dx9-ffp-port` skill that describes *exactly* this task, and two of its documented pitfalls named
bugs this project had open at the time it was integrated.

Its `retools.search strings` is what made Remix's own binary readable — which is where the
texcoord-format bug, the real semantics of `rtx.ignoreTextures`, the terrain-baker requirement and
the vertex-capture behaviour were all found. Four of the biggest unblocks in this project came out
of that one command.

Everyone who has contributed to it, per the repository's contributor list:

- **[Ekozmaster](https://github.com/Ekozmaster)** (Emanuel Kozerski) — author.
- **[Kim2091](https://github.com/Kim2091)** — the largest contributor by commit count, and the
  same person who adapted the SR2 lineage above. Both halves of this project's foundation run
  through their work.
- **[Night1099](https://github.com/Night1099)**
- **[Hemry81](https://github.com/Hemry81)**

### The stack underneath it

- **[radare2](https://github.com/radareorg/radare2)** and
  [r2ghidra](https://github.com/radareorg/r2ghidra) — bundled with Vibe-RE, and the disassembly
  engine underneath it.
- **[Ghidra](https://github.com/NationalSecurityAgency/ghidra)** — NSA. SLEIGH processor
  definitions and the decompiler.
- **[Frida](https://frida.re/)**, **[Capstone](https://github.com/capstone-engine/capstone)**,
  **[pefile](https://github.com/erocarrera/pefile)** — the dynamic and static analysis stack.

## ASI loading

- **[ThirteenAG](https://github.com/ThirteenAG)** —
  [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader), which is how
  `sr3-rtx.asi` gets into the process at all.

## The Saints Row community

- **[saintsrowmods.com](https://www.saintsrowmods.com/)** — the community that documented the
  Volition file formats. `tools/vpp_extract.py` reads VPP_PC v6 packfiles because that format was
  already public knowledge.
- **SRTT.MixFix** — the Saints Row: The Third stability and QoL patch, part of the modding
  groundwork on this game. (Disabled while running this mod, since two ASIs hooking the same
  device conflict — but it is what made the game a viable modding target.)
- **ZMenu SR3** and the wider SR3 ASI mod scene, which established that this game can be hooked
  and how.
- **[PCGamingWiki — Saints Row: The Third](https://www.pcgamingwiki.com/wiki/Saints_Row:_The_Third)**
  — the display-settings and launcher behaviour documented there.

## The game

- **Volition** — Saints Row: The Third (2011) and the Core engine. The inferred-lighting renderer
  this project spent weeks reading is genuinely elegant, and the skybox family is inherited
  wholesale from Red Faction: Guerrilla, which is its own small piece of studio history.
- **THQ**, and **Deep Silver / Koch Media / PLAION** — publishers.

Saints Row is a trademark of its respective owners. This is an unofficial fan project, not
affiliated with or endorsed by any of the above. **No game assets are distributed in this
repository** — `tools/vpp_extract.py` extracts them from your own copy of the game.

## How this was built

This project was built with AI assistance: **Claude Opus**, with some **Fable 5**, running as
[Claude Code](https://claude.com/claude-code) inside Visual Studio Code. Anthropic makes those
models.

The reverse-engineering conclusions in [docs/](docs/) are all backed by measurements recorded in
[docs/evidence/](docs/evidence/) — logs, frame dumps and screenshots — precisely so that they can
be checked rather than taken on trust.

---

**Thanks to everyone who made this project possible with their work on previous projects.**
