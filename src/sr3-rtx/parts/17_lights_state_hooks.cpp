// parts/17_lights_state_hooks.cpp - light injection and the device state-setter hooks.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 11402-12704 (old file:line citations map here).
// ---------------------------------------------------------------- light injection

// The light's parameters are in view space (its shader reconstructs surface positions along a
// view ray), so they are moved to world space with the inverse of the captured camera view.
// Every light reuses D3D9 slot 0: Remix accumulates game lights per draw call, so one slot
// carries an unbounded number and the 8-simultaneous-light cap never applies.
void EmitLight(IDirect3DDevice9* dev) {
    if (!g_settings.injectLights || g_curPS.light.kind == LightShader::NotLight) return;
    if (!g_haveCamera) return;

    D3DMATRIX invView;
    if (!Invert(g_cameraView, invView)) return;

    const LightShader& ls = g_curPS.light;
    if (ls.regPos < 0 || ls.regPos >= static_cast<int>(kMaxPsConst)) return;
    if (ls.regColor < 0 || ls.regColor >= static_cast<int>(kMaxPsConst)) return;

    const float* pos = g_psConst[ls.regPos];
    const float* col = g_psConst[ls.regColor];

    D3DLIGHT9 light{};
    light.Diffuse.r = col[0] * g_settings.lightScale;
    light.Diffuse.g = col[1] * g_settings.lightScale;
    light.Diffuse.b = col[2] * g_settings.lightScale;
    light.Specular = light.Diffuse;

    if (ls.kind == LightShader::Directional) {
        light.Type = D3DLIGHT_DIRECTIONAL;
        const D3DVECTOR dir = TransformDir(pos, invView);   // IR_Light_Pos holds a direction
        light.Direction = {-dir.x, -dir.y, -dir.z};
    } else {
        const float* info = (ls.regInfo >= 0) ? g_psConst[ls.regInfo] : nullptr;
        const float outer = info ? info[2] : 10.0f;         // (falloff, inner, outer, -)
        light.Position = TransformPoint(pos, invView);
        light.Range = (outer > 0.01f ? outer : 10.0f) * g_settings.lightRangeScale;
        light.Attenuation2 = 1.0f / (light.Range * light.Range * 0.25f + 1e-4f);

        if (ls.kind == LightShader::Spot && ls.regDir >= 0 && ls.regSpot >= 0) {
            light.Type = D3DLIGHT_SPOT;
            light.Direction = TransformDir(g_psConst[ls.regDir], invView);
            const float* spot = g_psConst[ls.regSpot];
            float cosOuter = spot[1];   // .y = cos(outer half-angle), .z = 1/(cosIn - cosOut)
            float cosInner = (std::fabs(spot[2]) > 1e-6f) ? cosOuter + 1.0f / spot[2] : cosOuter;
            cosOuter = max(-1.0f, min(1.0f, cosOuter));
            cosInner = max(cosOuter, min(1.0f, cosInner));
            light.Phi = 2.0f * std::acos(cosOuter);
            light.Theta = 2.0f * std::acos(cosInner);
            light.Falloff = 1.0f;
        } else {
            light.Type = D3DLIGHT_POINT;
        }
    }

    // ONE SLOT PER LIGHT, not one slot for every light.
    //
    // This used to be `SetLight(dev, 0, ...)` for all of them: every light in the frame - ~46 of
    // them - written through index 0, one after another. A D3D9 light index is the only handle
    // Remix has for correlating a light with its previous-frame self, so rewriting slot 0 forty
    // times a frame tells Remix that one light teleported forty times, and next frame it does it
    // again with different values. Nothing can be matched across frames.
    //
    // That single line explains all three reported symptoms:
    //   - "most lights, especially further from the camera, change hashes a lot" - a light's hash
    //     is derived from properties that, at slot 0, belong to a different light every call;
    //   - "the flashlight leaves a trail of lights mid air that go away after 1 or 2 seconds" -
    //     each frame's flashlight is a NEW light to Remix, and the previous one is kept alive for
    //     a few frames rather than being recognised as the same light having moved;
    //   - the flashing, at least in part, since unstable lights destabilise everything they lit.
    //
    // Emission order is the identity: the engine submits its light volumes in a consistent order
    // within a frame, so light N is the same light as light N was last frame. That is imperfect -
    // a light appearing mid-list shifts everything after it by one - but it is enormously better
    // than every light sharing one slot, and it costs nothing.
    // MEASURED 2026-08-19: checking only SetLight was not enough. D3D9 limits how many lights may
    // be simultaneously ACTIVE (D3DCAPS9::MaxActiveLights). Past that limit SetLight still
    // succeeds and LightEnable quietly fails, so the light is stored and never lit - and the
    // shotgun flashlight, which is emitted late in the frame's light list, stopped working
    // entirely while the counter still read a healthy 28 lights a frame.
    //
    // So the enable is checked too, and a light that cannot have its own slot falls back to slot
    // 0 rather than being dropped. That is the old shared-slot behaviour, but now it applies ONLY
    // to the overflow: the first N lights keep stable identities and everything beyond the
    // device limit is at least visible, which is strictly better than either extreme.
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    bool lit = false;
    if (g_lightSlot < g_maxLightSlots &&
        SUCCEEDED(g_origSetLight(dev, g_lightSlot, &light)) &&
        SUCCEEDED(g_origLightEnable(dev, g_lightSlot, TRUE))) {
        ++g_lightSlot;
        lit = true;
    } else if (SUCCEEDED(g_origSetLight(dev, 0, &light))) {
        g_origLightEnable(dev, 0, TRUE);
        ++g_lightOverflow;
        lit = true;
    }
    if (lit) ++g_lightsEmitted;
    g_internal = false;
}

// Turn off the slots this frame did not use. Without this a light that goes away stays enabled at
// its old position forever, which is the same trail by a different route.
void DisableUnusedLightSlots(IDirect3DDevice9* dev) {
    if (!g_settings.injectLights) return;
    g_internal = true;
    g_internalThread = GetCurrentThreadId();
    for (unsigned i = g_lightSlot; i < g_lightSlotsUsedLastFrame; ++i)
        g_origLightEnable(dev, i, FALSE);
    g_internal = false;
    g_lightSlotsUsedLastFrame = g_lightSlot;
    g_lightSlot = 0;
}

// ---------------------------------------------------------------- hooks

HRESULT WINAPI Hook_SetVertexShaderConstantF(IDirect3DDevice9* dev, UINT start,
                                             const float* data, UINT count) {
    ++g_callVsConst;
    g_callVsConstRegs += count;
    // No programmable vertex shader runs on the bridge with capture off, so these never need to
    // cross it; the shadow below is the only reader.
    HRESULT hr = D3D_OK;
    if (g_settings.dietVsConstants && !InternalHere()) ++g_dietVsConstDropped;
    else hr = g_origSetVSConstF(dev, start, data, count);
    if (!data || !count || start + count > kMaxVsConst) return hr;

    memcpy(&g_vsConst[start][0], data, count * 4 * sizeof(float));

    const UINT end = start + count;
    if (start <= kRegProjTM && end >= kRegProjTM + 4) {
        Trace('P');
        g_viewProjDirty = true;
        // A projTM upload starts a new draw setup. Clear the world assumption so geometry that
        // does not write objTM gets identity rather than the previous object's matrix - unless
        // objTM was written earlier in this same setup, which some paths do.
        if (g_worldWritten && !g_worldWrittenSinceDraw) {
            g_worldWritten = false;
            g_worldDirty = true;
        }
        // The bone palette needs the same scoping objTM already has, and never had it.
        //
        // Measured 2026-08-25: a mesh whose objTM was IDENTICAL between two frames moved 0.75
        // units, because bone[0]'s translation went from (0.132, -0.939, -0.360) to (0, 0, 0).
        // The palette shadow persists across setups, so a draw whose own setup uploaded no
        // palette silently inherits the previous object's bones - and which object that is
        // depends on draw order, which depends on what is on screen.
        //
        // This is the identical failure the comment above describes for objTM, and the same
        // remedy applies: an object that did not publish a palette gets IDENTITY bones, so objTM
        // alone places it, rather than being posed by somebody else's skeleton.
        ++g_setupId;
        // Reject non-finite uploads: some passes reuse these registers for parameter blocks,
        // and a NaN projection poisons Remix's camera tracking for the whole frame.
        g_viewProjValid = true;
        for (int i = 0; i < 16 && g_viewProjValid; ++i)
            if (!std::isfinite(g_vsConst[kRegProjTM][i])) g_viewProjValid = false;
    }
    if (start <= kRegWorld2View && end >= kRegWorld2View + 3) {
        Trace('W');
        g_viewProjDirty = true;
    }
    if (start < kRegBonePalette + kBonesMax * kRegsPerBone && end > kRegBonePalette) {
        g_lastBoneUploadDraw = g_drawIndexThisFrame;
        g_lastBoneUploadFrame = g_frames;
        // How far the upload actually REACHED. The palette shadow persists across draws, so
        // registers past this point still hold whatever the previous object left there. A mesh
        // whose bone indices run past the upload is posed by another object's bones - which is
        // what "a car door flew to where that NPC's arm was" looks like. Recorded as a bone
        // count so it can be compared with a mesh's highest index directly.
        const unsigned reach = (end > kRegBonePalette) ? (end - kRegBonePalette) : 0u;
        g_lastBoneUploadBones = reach / kRegsPerBone;
        // Mark every bone this call touched. Partial calls accumulate instead of overwriting,
        // which is the flaw in g_lastBoneUploadBones above.
        g_paletteHistory[g_paletteHistoryPos % 24] =
            {g_drawIndexThisFrame, g_frames, start, end};
        ++g_paletteHistoryPos;
        const unsigned firstBone = (start > kRegBonePalette)
            ? (start - kRegBonePalette) / kRegsPerBone : 0u;
        const unsigned lastBone = (reach + kRegsPerBone - 1) / kRegsPerBone;
        for (unsigned b = firstBone; b < lastBone && b < static_cast<unsigned>(kBonesMax); ++b) {
            g_boneWrittenDraw[b] = g_drawIndexThisFrame;
            g_boneWrittenFrame[b] = g_frames;
            // How many bones the upload that wrote this one covered. A skeleton's size is a
            // fingerprint: a mesh that uses 1 bone but reads it from a 48-bone upload is reading
            // a CHARACTER's root bone, and every palette upload in this game starts at c52 so
            // bone 0 belongs to whoever wrote last.
            g_boneWrittenUploadBones[b] = g_lastBoneUploadBones;
            g_boneWrittenObjGen[b] = g_objGeneration;
        }
        // 2026-09-18 AUDIT FIX: moved out of the loop above, where the misindentation put it -
        // it ran once per BONE instead of once per UPLOAD, and was skipped entirely on an
        // upload whose range left the loop body unentered.
        g_paletteSetupId = g_setupId;
    }
    if (start <= kRegObjTM && end >= kRegObjTM + 3) {
        // Compared by VALUE, not merely by "a write happened": the game re-uploads an identical
        // objTM for consecutive draws of one object, and counting those as new objects would make
        // every part look foreign.
        const float t0 = g_vsConst[kRegObjTM][3], t1 = g_vsConst[kRegObjTM + 1][3],
                    t2 = g_vsConst[kRegObjTM + 2][3];
        if (t0 != g_lastObjTM[0] || t1 != g_lastObjTM[1] || t2 != g_lastObjTM[2]) {
            g_lastObjTM[0] = t0; g_lastObjTM[1] = t1; g_lastObjTM[2] = t2;
            ++g_objGeneration;
        }
        Trace('O');
        g_worldDirty = true;
        g_worldWritten = true;
        g_worldWrittenSinceDraw = true;
    }
    return hr;
}

HRESULT WINAPI Hook_SetPixelShaderConstantF(IDirect3DDevice9* dev, UINT start,
                                            const float* data, UINT count) {
    ++g_callPsConst;
    g_callPsConstRegs += count;
    if (data)
        for (UINT i = 0; i < count && start + i < kMaxPsConst; ++i)
            memcpy(g_psConst[start + i], data + i * 4, sizeof(float) * 4);
    return g_origSetPSConstF(dev, start, data, count);
}

HRESULT WINAPI Hook_CreateVertexShader(IDirect3DDevice9* dev, const DWORD* function,
                                       IDirect3DVertexShader9** shader) {
    const HRESULT hr = g_origCreateVertexShader(dev, function, shader);
    if (SUCCEEDED(hr) && shader && *shader && function) {
        const LONGLONG t = Now();
        if (g_shaders.emplace(*shader, ReflectShader(function)).second) (*shader)->AddRef();
        g_otherMsThisFrame += MsSince(t);
        ++g_shadersCreatedThisFrame;
    }
    return hr;
}

HRESULT WINAPI Hook_SetVertexShader(IDirect3DDevice9* dev, IDirect3DVertexShader9* shader) {
    ++g_callSetVS;
    if (!InternalHere()) {
        const auto it = g_shaders.find(shader);
        g_curVS = (it != g_shaders.end()) ? it->second : ShaderInfo{};
        g_lastVS = shader;
        Trace(g_curVS.usesObjTM ? 'V' : 'v');
        // NOTE: g_worldWritten is deliberately NOT cleared here. See ComputeTransforms.
        // Shadowed only. The bridge is synced right before any draw that is forwarded.
        if (g_settings.dietVsBinds) { ++g_dietVsBindDropped; return D3D_OK; }
    }
    SyncBridgeVS(dev, shader);
    return D3D_OK;
}

HRESULT WINAPI Hook_CreatePixelShader(IDirect3DDevice9* dev, const DWORD* function,
                                      IDirect3DPixelShader9** shader) {
    const HRESULT hr = g_origCreatePixelShader(dev, function, shader);
    if (SUCCEEDED(hr) && shader && *shader && function) {
        const LONGLONG t = Now();
        ShaderInfo info = ReflectShader(function);
        info.isPixelShader = true;
        if (g_shaders.emplace(*shader, info).second) (*shader)->AddRef();
        g_otherMsThisFrame += MsSince(t);
        ++g_shadersCreatedThisFrame;
        if (info.light.kind != LightShader::NotLight && ++g_lightShadersSeen <= 6) {
            static const char* kNames[] = {"none", "point", "spot", "directional"};
            Log("light shader: %s (pos=c%d dir=c%d color=c%d info=c%d spot=c%d)",
                kNames[info.light.kind], info.light.regPos, info.light.regDir,
                info.light.regColor, info.light.regInfo, info.light.regSpot);
        }
    }
    return hr;
}

HRESULT WINAPI Hook_SetPixelShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* shader) {
    ++g_callSetPS;
    if (!InternalHere()) {
        const auto it = g_shaders.find(shader);
        g_curPS = (it != g_shaders.end()) ? it->second : ShaderInfo{};
        g_lastPS = shader;
    }
    return g_origSetPixelShader(dev, shader);
}

HRESULT WINAPI Hook_SetTexture(IDirect3DDevice9* dev, DWORD stage,
                               IDirect3DBaseTexture9* texture) {
    ++g_callSetTex;
    if (!InternalHere() && stage < 8) g_curTexture[stage] = texture;
    return g_origSetTexture(dev, stage, texture);
}

HRESULT WINAPI Hook_CreateTexture(IDirect3DDevice9* dev, UINT w, UINT h, UINT levels,
                                  DWORD usage, D3DFORMAT format, D3DPOOL pool,
                                  IDirect3DTexture9** texture, HANDLE* shared) {
    const HRESULT hr = g_origCreateTexture(dev, w, h, levels, usage, format, pool, texture,
                                           shared);
    // Remember which textures are generated rather than authored, so they are never mistaken
    // for surface colour.
    if (SUCCEEDED(hr) && texture && *texture) {
        ++g_texturesCreatedThisFrame;
        // Every LARGE uncompressed texture, whatever its usage - so that if the character atlas
        // is created with parameters the filter below rejects, it still shows up here instead of
        // vanishing silently. This is the check the previous two builds did not have.
        if ((format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) &&
            w >= 1024 && h >= 512 && g_bigTexReports < 16) {
            ++g_bigTexReports;
            Log("BIG TEXTURE #%u created: %ux%u levels=%u fmt=%d pool=%d usage=0x%lX",
                g_bigTexReports, w, h, levels, static_cast<int>(format),
                static_cast<int>(pool), static_cast<unsigned long>(usage));
        }
        // Large uncompressed RENDER TARGETS are the candidate composite destinations - the step
        // before the dynamic texture the game fills with zeroes. Read once each, later.
        if ((format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8) &&
            (usage & D3DUSAGE_RENDERTARGET) != 0 && w >= 1024 && h >= 512)
            NoteRenderTargetCandidate(*texture, w, h);
        // The texture vtable is patched from the FIRST texture this game creates, and candidate
        // atlases are recorded at CREATION.
        //
        // The previous version did both at the first character DRAW, and measured LockRect 0 -
        // which proved nothing, because SR3 composites a character's texture when it SPAWNS,
        // long before that character is ever drawn. The hook was being installed after the
        // event it was meant to observe. Creation is the earliest point where a texture exists
        // at all, so nothing can now be written to one before the hook is in place.
        InstallTextureHooks(*texture);
        // TIGHTENED 2026-08-29. The first version accepted DYNAMIC *or* RENDERTARGET at any
        // size >= 256, and its own overflow counter reported 955 candidates against a 32-entry
        // array - so it was tracking the first 32 render targets the game happened to create and
        // the character atlas was never among them. Every count taken through it described other
        // textures.
        //
        // The signature to match is the one the albedo report prints for the texture actually
        // bound on character draws:
        //
        //     2048x1024 fmt=22 mips=9 pool=0 usage=0x200
        //
        // fmt 22 is D3DFMT_X8R8G8B8 and 0x200 is D3DUSAGE_DYNAMIC. The render targets that
        // flooded the old test are fmt=21 with usage=0x1, so excluding RENDERTARGET and
        // requiring a large surface separates them cleanly.
        const bool uncompressed = (format == D3DFMT_X8R8G8B8 || format == D3DFMT_A8R8G8B8);
        const bool dynamicNotRT = (usage & D3DUSAGE_DYNAMIC) != 0 &&
                                  (usage & D3DUSAGE_RENDERTARGET) == 0;
        if (uncompressed && dynamicNotRT && pool == D3DPOOL_DEFAULT && w >= 1024 && h >= 512) {
            NoteAtlasPtr(*texture);
            if (g_atlasCreateReports < 8) {
                ++g_atlasCreateReports;
                Log("ATLAS CANDIDATE #%u created: %ux%u levels=%u fmt=%d pool=%d usage=0x%lX "
                    "(want 2048x1024 fmt=22 usage=0x200)",
                    g_atlasCreateReports, w, h, levels, static_cast<int>(format),
                    static_cast<int>(pool), static_cast<unsigned long>(usage));
            }
        }
    }
    if (SUCCEEDED(hr) && texture && *texture && (usage & D3DUSAGE_RENDERTARGET)) {
        // AddRef for the same reason as the mesh cache, though this set is only ever compared
        // against and never bound: a destroyed texture's address can be recycled by a newly
        // created one, which would then be permanently misclassified as generated data and
        // refused as albedo. The set holds ~55 entries, so the cost of pinning them is nil.
        if (g_rtTextures.insert(*texture).second) (*texture)->AddRef();
    }
    return hr;
}

HRESULT WINAPI Hook_SetRenderState(IDirect3DDevice9* dev, D3DRENDERSTATETYPE state, DWORD value) {
    ++g_callRS;
    if (!InternalHere() && state < kRsMax) { g_rsShadow[state] = value; g_rsKnown[state] = true; }
    return g_origSetRenderState(dev, state, value);
}

HRESULT WINAPI Hook_SetTextureStageState(IDirect3DDevice9* dev, DWORD stage,
                                         D3DTEXTURESTAGESTATETYPE type, DWORD value) {
    ++g_callTSS;
    if (!InternalHere() && stage < 8 && type < kTssMax) {
        g_tssShadow[stage][type] = value;
        g_tssKnown[stage][type] = true;
    }
    return g_origSetTextureStageState(dev, stage, type, value);
}

HRESULT WINAPI Hook_SetSamplerState(IDirect3DDevice9* dev, DWORD sampler,
                                    D3DSAMPLERSTATETYPE type, DWORD value) {
    ++g_callSS;
    if (!InternalHere() && sampler < 16 && type < kSsMax) {
        g_ssShadow[sampler][type] = value;
        g_ssKnown[sampler][type] = true;
    }
    return g_origSetSamplerState(dev, sampler, type, value);
}

// The game drives everything through shader constants and never sets fixed-function
// transforms, so anything arriving here that is not ours would invalidate our cache.
HRESULT WINAPI Hook_SetTransform(IDirect3DDevice9* dev, D3DTRANSFORMSTATETYPE state,
                                 const D3DMATRIX* matrix) {
    ++g_callTransform;
    if (!InternalHere()) {
        if (state == D3DTS_WORLD) g_haveAppliedWorld = false;
        else if (state == D3DTS_VIEW) g_haveAppliedView = false;
        else if (state == D3DTS_PROJECTION) g_haveAppliedProj = false;
        // 2026-09-18 AUDIT FIX: g_appliedU/g_appliedV (SetupTextureStages' texture-matrix elision
        // cache) were never invalidated by anything - not here, not the per-frame reset below.
        // 0.0f is the same sentinel their own initialisers use, and su/sv are always positive in
        // every case this shim builds them from, so it can never collide with a real scale.
        else if (state == D3DTS_TEXTURE0) { g_appliedU = 0.0f; g_appliedV = 0.0f; }
    }
    return g_origSetTransform(dev, state, matrix);
}

VertexLayout ParseDeclaration(IDirect3DVertexDeclaration9* decl) {
    VertexLayout out;
    if (!decl) return out;
    D3DVERTEXELEMENT9 elems[64]{};   // MAXD3DDECLLENGTH; not exposed by every SDK version
    UINT count = 0;
    if (FAILED(decl->GetDeclaration(elems, &count))) return out;
    for (UINT i = 0; i < count; ++i) {
        const D3DVERTEXELEMENT9& e = elems[i];
        if (e.Stream == 0xFF) break;   // D3DDECL_END
        ++out.elements;
        // POSITION is recorded whatever stream it is in: knowing it lives outside stream 0 is
        // itself a reason not to convert, and silently skipping it would hide that.
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 0) {
            out.posType = e.Type;
            out.posOffset = e.Offset;
            out.posStream = e.Stream;
        }
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex == 1) {
            out.morphPosType = e.Type;
            out.morphPosOffset = e.Offset;
            out.morphStream = e.Stream;
        }
        if (e.Usage == D3DDECLUSAGE_NORMAL && e.UsageIndex == 1) {
            out.morphNrmType = e.Type;
            out.morphNrmOffset = e.Offset;
        }
        // The instance transform: POSITION 2/3/4 as float4s, all in one stream.
        if (e.Usage == D3DDECLUSAGE_POSITION && e.UsageIndex >= 2 && e.UsageIndex <= 4 &&
            e.Type == D3DDECLTYPE_FLOAT4) {
            out.instStream = e.Stream;
            out.instRowOffset[e.UsageIndex - 2] = e.Offset;
        }
        // 2026-09-18 AUDIT FIX: recorded BEFORE the stream-0 filter below, unlike texcoord1Type/
        // texcoord1Offset just below it, so a genuinely stream-1 TEXCOORD1 (normal for the
        // INSTANCED decal draws FIX A targets) is actually SEEN instead of leaving
        // texcoord1Stream indistinguishable from "no texcoord1 at all" - both used to read -1.
        // InstallFloatUV's `texcoord1Stream != 0` refusal test was dead for want of this.
        if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 1) out.texcoord1Stream = e.Stream;
        // CHANGE 3, 2026-09-18: same fix, extended to usage-index 0. Recorded here, before
        // the stream-0 filter below, so a TEXCOORD0 declared on any stream other than 0 is
        // still seen instead of leaving texcoordStream indistinguishable from "no TEXCOORD0
        // at all" - both used to fall through to the sentinel -1.
        if (e.Usage == D3DDECLUSAGE_TEXCOORD && e.UsageIndex == 0) out.texcoordStream = e.Stream;
        if (e.Stream != 0) continue;
        switch (e.Usage) {
            case D3DDECLUSAGE_POSITIONT: out.hasPositionT = true; break;
            case D3DDECLUSAGE_NORMAL:
                out.hasNormal = true;
                if (e.UsageIndex == 0) { out.normalType = e.Type; out.normalOffset = e.Offset; }
                break;
            case D3DDECLUSAGE_COLOR:     out.hasColor = true; break;
            case D3DDECLUSAGE_BLENDWEIGHT:
                out.skinned = true;
                out.blendWeightType = e.Type;
                out.blendWeightOffset = e.Offset;
                break;
            case D3DDECLUSAGE_BLENDINDICES:
                out.skinned = true;
                out.blendIndexType = e.Type;
                out.blendIndexOffset = e.Offset;
                break;
            case D3DDECLUSAGE_TEXCOORD:
                if (e.UsageIndex == 0) {
                    out.hasTexcoord = true;
                    out.texcoordType = e.Type;
                    out.texcoordOffset = e.Offset;
                } else if (e.UsageIndex == 1) {
                    out.texcoord1Type = e.Type;
                    out.texcoord1Offset = e.Offset;
                    // texcoord1Stream is set above, before the stream-0 filter - always 0 here
                    // (that filter guarantees it), so re-assigning it would be a no-op.
                }
                break;
            default: break;
        }
    }
    out.hasInstanceTransform = out.instStream >= 0 && out.instStream < kMaxStreams &&
                               out.instRowOffset[0] >= 0 && out.instRowOffset[1] >= 0 &&
                               out.instRowOffset[2] >= 0;
    out.parsed = true;
    return out;
}

const char* DeclTypeName(int t) {
    switch (t) {
        case D3DDECLTYPE_FLOAT1: return "float1";
        case D3DDECLTYPE_FLOAT2: return "float2";
        case D3DDECLTYPE_FLOAT3: return "float3";
        case D3DDECLTYPE_FLOAT4: return "float4";
        case D3DDECLTYPE_D3DCOLOR: return "d3dcolor";
        case D3DDECLTYPE_UBYTE4: return "ubyte4";
        case D3DDECLTYPE_SHORT2: return "short2";
        case D3DDECLTYPE_SHORT4: return "short4";
        case D3DDECLTYPE_UBYTE4N: return "ubyte4n";
        case D3DDECLTYPE_SHORT2N: return "short2n";
        case D3DDECLTYPE_SHORT4N: return "short4n";
        case D3DDECLTYPE_FLOAT16_2: return "half2";
        case D3DDECLTYPE_FLOAT16_4: return "half4";
        default: return "?";
    }
}

unsigned g_layoutReports = 0;
// SR3 stores every world texture coordinate as D3DDECLTYPE_SHORT2, and Remix cannot read it.
// From its own log, with its own format table used to decode the number:
//
//     warn: [rtx-interleaver] Unsupported texcoord buffer format (80), skipping texcoord
//
// VkFormat 80 is VK_FORMAT_R16G16_SSCALED, which is what DXVK maps SHORT2 to. "Skipping texcoord"
// is literal: the geometry reaches the path tracer with NO UVs, so every surface samples one
// texel and renders as flat material colour. That is the whole of "the world is not textured".
//
// Re-declaring the same bytes as SHORT2N was tried first because it costs nothing, and Remix
// answered in the same words with a different number - "Unsupported texcoord buffer format (78)",
// VK_FORMAT_R16G16_SNORM. The interleaver wants real floats, and no reinterpretation of these
// four bytes can produce them.
//
// So the coordinates are CONVERTED, into a float2 buffer of our own bound as a second stream.
// float2 is not a third guess: it is the only texcoord format known to work on this exact path,
// and it is known from the two populations that are textured today - characters, whose vertices
// this shim rebuilds into a float2 FVF for skinning, and foliage, drawn from a game instance
// stream whose stream 0 is a single float2 TEXCOORD.
//
// WHOLE BUFFER AT A TIME, keyed on the source buffer rather than on the draw. Converting per draw
// would rewrite the same city block's coordinates on each of the ~450 draws a frame that need
// them. Converting the whole buffer once also makes the stream offset trivial: our entry i is
// source vertex i, so any draw from that buffer binds it unchanged at any minIndex. A per-draw
// ring would instead have to satisfy streamOffset + minIndex*stride with a UINT offset, which
// forces it to waste minIndex*8 bytes on every allocation.
DWORD g_uvStream = kUvStreamDefault;

struct UvBuffer {
    IDirect3DVertexBuffer9* src = nullptr;   // referenced: the key is its address
    IDirect3DVertexBuffer9* uv = nullptr;
    UINT vertexCount = 0;
    UINT bytes = 0;
    bool ring = false;       // uv points into the shared dynamic ring: never Release it
    UINT offset = 0;         // stream offset to bind the ring at, for this entry
};
std::unordered_map<unsigned long long, UvBuffer> g_uvBuffers;
// Keys per source buffer, so an invalidation touches only that buffer's entries. The scan of
// the whole map per invalidated buffer, ~60 times a frame, was most of the "drain" phase.
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<unsigned long long>> g_uvByVB;
inline void UvIndexAdd(IDirect3DVertexBuffer9* vb, unsigned long long key) {
    try { g_uvByVB[vb].push_back(key); } catch (...) {}
}
void UvIndexRebuild() {
    g_uvByVB.clear();
    for (const auto& kv : g_uvBuffers) UvIndexAdd(kv.second.src, kv.first);
}
// THE UV RING. A DYNAMIC source buffer is rewritten by the game every frame, which invalidated
// its converted copy every frame - measured 103,687 conversions with 100,980 invalidations in
// one run, each a CreateVertexBuffer + Lock + Unlock + Release across the bridge: the whole
// 14 ms "uv" phase. Converted ranges from dynamic sources now go into one persistent ring
// with NOOVERWRITE appends (DISCARD on wrap, which also empties every ring entry from the
// cache), bound at an offset - the same scheme the skinning ring uses. Static sources keep
// their own buffers as before.
IDirect3DVertexBuffer9* g_uvRing = nullptr;
bool g_uvRingTried = false;
constexpr UINT kUvRingBytes = 8u * 1024u * 1024u;
UINT g_uvRingPos = 0;
unsigned g_uvRingWraps = 0, g_uvRingAppends = 0, g_uvRingFail = 0;
size_t g_uvBytesHeld = 0;
unsigned g_uvBuffersMade = 0;
unsigned g_uvConvertFailures = 0;
unsigned g_uvFailLayout = 0;     // no stream 0, or the texcoord is outside the stride
unsigned g_uvFailDynamic = 0;    // DYNAMIC source: must not be read back, and would be stale
unsigned g_uvFailDesc = 0;       // GetDesc failed, or the buffer holds no whole vertices
unsigned g_uvFailCreate = 0;     // could not allocate our own buffer
unsigned g_uvFailLock = 0;       // could not lock one of the two buffers
unsigned g_uvDynConverted = 0;   // DYNAMIC source converted from the snooped copy
unsigned g_uvDynWaiting = 0;     // DYNAMIC source registered, snoop not filled yet
unsigned g_uvBufferFlushes = 0;
unsigned g_uvInvalidations = 0;
unsigned g_uvStreamDraws = 0;
// FIX A: draws whose vertex shader declares texcoord1 but not texcoord0 (see DecalUvSourceSet),
// served from that set instead of the always-sampled set 0; and draws that wanted it but the
// bound vertex declaration had no usable (SHORT2) texcoord1 to convert.
unsigned g_uvSet1Draws = 0;
unsigned g_uvSet1Unavailable = 0;
// Crash fix 2026-09-14: set-1 draws refused not for TYPE (g_uvSet1Unavailable, above) but
// because texcoord1 lives outside stream 0 - a non-zero stream (normal for these INSTANCED
// decal draws) or an unresolved offset. Reading stream-1-shaped offsets out of stream 0's bytes
// is the access violation that crashed world load (0xC0000005 reading near 0x59ADF000). Counted
// separately so a wrong-stream refusal is distinguishable from a wrong-type one in the report.
unsigned g_uvSet1WrongStream = 0;
// FIX 3 diagnostics (uvHonourStreamOffset): how often a UV conversion is even asked to run with
// stream 0 bound at a non-zero offset - measured whether or not the switch is on, split static
// (the whole-buffer decode) from dynamic (the snooped range/ring), and by phase (offset % stride
// != 0, i.e. not simply a whole-vertex shift the old base-blind math could not fix either way).
unsigned g_uvNonZeroOffsetStatic = 0, g_uvNonZeroOffsetDynamic = 0, g_uvNonZeroOffsetPhase = 0;
unsigned g_uvNonZeroOffsetDecalAlbedo = 0;

// The range is part of the key for DYNAMIC sources only. A dynamic buffer is a RING: the game
// refills one slice of it per draw, so two draws in the same frame legitimately read different
// vertices from one buffer. Keyed on the pointer alone, the second draw is served the first
// draw's coordinates - the identical defect the bind-pose cache had ("each character had the
// wrong head"), whose key had to become {vb, offset, stride, minIndex, count} for the same
// reason. Static buffers pass 0/0 and keep their old whole-buffer key.
// FIX A: `set` is folded in as its own hash term, not merely implied by texOffset differing -
// a buffer converted for the decal family's texcoord1 must never be handed back to a draw that
// wanted texcoord0, even in the coincidence that the two offsets happened to match.
// FIX 3: `phase` is a further hash term - for a static source it is offset % stride (so a pooled
// buffer bound at two different phases never shares one decoded entry, see UvBufferFor's own
// comment); for a dynamic source it is the full g_stream0Offset (a dynamic entry is already keyed
// per draw range, so there is no base/phase split to make - see the dynamic branch's own comment).
unsigned long long UvKey(void* vb, UINT stride, int texOffset, UINT first, UINT count, int set,
                         unsigned long long phase) {
    unsigned long long h = 1469598103934665603ull;
    const unsigned long long parts[7] = {
        reinterpret_cast<unsigned long long>(vb), stride,
        static_cast<unsigned long long>(texOffset), first, count,
        static_cast<unsigned long long>(set), phase};
    for (int i = 0; i < 7; ++i) { h ^= parts[i]; h *= 1099511628211ull; }
    return h;
}

void ReleaseUvBuffers() {
    for (auto& kv : g_uvBuffers) {
        if (kv.second.src) kv.second.src->Release();
        if (kv.second.uv && !kv.second.ring) kv.second.uv->Release();
    }
    g_uvBuffers.clear();
    g_uvByVB.clear();
    g_uvBytesHeld = 0;
}

// The converted buffer for the currently bound stream 0, or null if it cannot be built.
IDirect3DVertexBuffer9* UvBufferFor(IDirect3DDevice9* dev) {
    const UINT stride = g_stream0Stride;
    // FIX A: InstallFloatUV has already picked which texcoord set this draw needs and confirmed
    // it is SHORT2; read the matching offset rather than always set 0's.
    const int uvSet = DecalUvSourceSet();
    const int texOffset = (uvSet == 1) ? g_curLayout.texcoord1Offset : g_curLayout.texcoordOffset;
    if (!g_stream0 || stride == 0 || texOffset < 0 || texOffset + 4 > static_cast<int>(stride)) {
        ++g_uvFailLayout;
        ++g_uvConvertFailures;
        return nullptr;
    }

    // FIX 3 (uvHonourStreamOffset): D3D9 addresses stream 0 at g_stream0Offset + n*stride for
    // vertex n. The STATIC branch below decodes ONE buffer for the WHOLE source range, entry i
    // from byte i*stride - which only equals vertex i's bytes when g_stream0Offset is 0. Split it
    // the same way SkinFFBufferFor splits its own side stream: uvPhase = offset % stride folds
    // into the static decode and the cache key; uvBase = offset / stride only shifts where the
    // draw BINDS the result (added to g_uvBindOffset below), since it does not change which bytes
    // get decoded. The DYNAMIC (snooped/ring) branches read a per-draw-range slice instead of a
    // whole buffer, so they use the full offset directly rather than this split - see their own
    // comments, further down. Off: uvPhase = uvBase = 0, byte-identical to the pre-fix addressing.
    const UINT uvPhase = g_settings.uvHonourStreamOffset ? (g_stream0Offset % stride) : 0;
    const UINT uvBase = g_settings.uvHonourStreamOffset ? (g_stream0Offset / stride) : 0;
    const UINT uvOffsetForRead = g_settings.uvHonourStreamOffset ? g_stream0Offset : 0;   // dynamic branches only

    D3DVERTEXBUFFER_DESC desc{};
    {
        const auto dit = g_vbDescCache.find(g_stream0);
        if (dit != g_vbDescCache.end()) desc = dit->second;
        else {
            if (FAILED(g_stream0->GetDesc(&desc))) { ++g_uvFailDesc; ++g_uvConvertFailures; return nullptr; }
            try { g_vbDescCache[g_stream0] = desc; } catch (...) {}
        }
    }
    // DYNAMIC source. This used to refuse outright, on two objections that are both real and
    // both already answered elsewhere in this file:
    //
    //   "a DYNAMIC buffer must not be read back"  - correct, and we do not. The VB lock hook has
    //      snooped the game's OWN writes since the morph work; RegisterSnoop/SnoopCopy read that
    //      copy, never the buffer. Same mechanism the morph stream uses.
    //   "a conversion of it would be stale immediately" - InvalidateUvBuffers(vb) is already
    //      wired into Hook_VBUnlock, so the moment the game refills the buffer our converted
    //      copy is dropped and rebuilt. The staleness guard exists; this path was written before
    //      it did.
    //
    // WHY IT MATTERS, measured 2026-08-31: this single branch accounted for EVERY ONE of the
    // 83,737 UV conversion failures in the run - 5.8 draws a frame, and 0.0/frame for all four
    // other reasons. Those draws keep raw SHORT2 texcoords, which are NOT normalised: a stored
    // 16384 is a UV of 16384, so the texture tiles into noise. That is visible with RAY TRACING
    // OFF as well, because the bad coordinates are in the rasterised draw itself - which is
    // exactly how the user described it ("the top is textured right but not shoes and other
    // stuff... also reflected with ray tracing off"). Dynamic vertex buffers are what the engine
    // uses for per-frame-written character parts.
    //
    // The first draw on a newly registered buffer finds nothing and waits - counted separately as
    // g_uvDynWaiting, NOT as a failure of the conversion itself, so the two cannot be confused
    // the way a single lumped counter hid this branch for so long.
    const bool dynamicSrc = (desc.Usage & D3DUSAGE_DYNAMIC) != 0;
    if (dynamicSrc) {
        if (!g_settings.convertDynamicUV) {
            ++g_uvFailDynamic;
            ++g_uvConvertFailures;
            return nullptr;
        }
        RegisterSnoop(g_stream0);
    }

    // FIX 3 diagnostics: measured whether or not the switch is on - see the counters' own comment,
    // above g_uvSet1WrongStream.
    if (g_stream0Offset != 0) {
        if (dynamicSrc) ++g_uvNonZeroOffsetDynamic; else ++g_uvNonZeroOffsetStatic;
        if ((g_stream0Offset % stride) != 0) ++g_uvNonZeroOffsetPhase;
        if (g_curPS.isPixelShader && g_curPS.albedoSampler[0] &&
            StrStrIA(g_curPS.albedoSampler, "Decal") != nullptr)
            ++g_uvNonZeroOffsetDecalAlbedo;
    }

    // FIX 3: the whole-buffer STATIC decode covers [uvPhase, uvPhase + vertexCount*stride) rather
    // than [0, desc.Size) - see uvPhase's own comment, above. The DYNAMIC count is unaffected: its
    // branches read a per-draw-range slice, not this whole-buffer decode.
    const UINT vertexCount = dynamicSrc ? (desc.Size / stride)
                                        : ((desc.Size > uvPhase) ? (desc.Size - uvPhase) / stride : 0);
    if (vertexCount == 0) { ++g_uvFailDesc; ++g_uvConvertFailures; return nullptr; }
    const UINT bytes = vertexCount * kUvBytesPerVertex;

    // The slice this draw actually reads. Entries stay indexed per vertex exactly as stream 0 is
    // - the uv stream is bound at offset 0 with its own stride - so entry i always corresponds to
    // stream-0 vertex i, whether or not we filled it.
    UINT rangeFirst = 0, rangeCount = vertexCount;
    if (dynamicSrc) {
        rangeFirst = g_curDrawFirstVertex;
        rangeCount = g_curDrawVertexCount;
        // FIX 3: honouring the offset means the true bound bytes are checked, not just a vertex
        // count that silently assumed g_stream0Offset was 0 - VertexRangeFits is the same helper
        // GetBaseMesh uses for the identical arithmetic. Off keeps the exact old, offset-blind
        // check for a byte-identical A/B.
        const bool rangeOk = g_settings.uvHonourStreamOffset
            ? (rangeCount != 0 && VertexRangeFits(g_stream0, g_stream0Offset, rangeFirst, rangeCount, stride))
            : (rangeCount != 0 && rangeFirst + rangeCount <= vertexCount);
        if (!rangeOk) {
            ++g_uvFailLayout;
            ++g_uvConvertFailures;
            return nullptr;
        }
    }

    const unsigned long long key = UvKey(g_stream0, stride, texOffset,
                                         dynamicSrc ? rangeFirst : 0,
                                         dynamicSrc ? rangeCount : 0, uvSet,
                                         dynamicSrc ? static_cast<unsigned long long>(uvOffsetForRead)
                                                    : static_cast<unsigned long long>(uvPhase));
    const auto it = g_uvBuffers.find(key);
    if (it != g_uvBuffers.end()) {
        // FIX 3: a static entry's own offset field is always 0 (there is no ring position for a
        // whole-buffer decode) - uvBase is what shifts the bind for THIS call, recomputed from
        // THIS draw's live g_stream0Offset rather than stored in the entry, because the cache key
        // only folds in phase (offset % stride): two draws at different offsets but the same
        // phase correctly share one decoded entry and differ only in where they bind it.
        g_uvBindOffset = it->second.offset + (dynamicSrc ? 0u : uvBase * kUvBytesPerVertex);
        return it->second.uv;
    }
    g_uvBindOffset = 0;

    // ---- dynamic source: append the converted range to the ring
    if (dynamicSrc) {
        if (!g_uvRing && !g_uvRingTried) {
            g_uvRingTried = true;
            const bool wasInt = g_internal;
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            if (FAILED(dev->CreateVertexBuffer(kUvRingBytes, D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0,
                                               D3DPOOL_DEFAULT, &g_uvRing, nullptr)))
                g_uvRing = nullptr;
            g_internal = wasInt;
            Log("uv ring: %s (%u MB)", g_uvRing ? "created" : "CREATE FAILED - dynamic sources fall back",
                kUvRingBytes >> 20);
        }
        if (g_uvRing) {
            // Crash fix 2026-09-14: belt-and-suspenders for the dynamic/snooped (ring) read
            // below, which indexes stream 0's bytes at texOffset using stream 0's stride.
            // InstallFloatUV already refuses a stream-1 or unresolved texcoord1 before this
            // function is ever called (see the crash-fix guard there), but this is the actual
            // read site: refuse here too, before touching the ring, rather than walking past
            // the mapped vertex range - the access violation that crashed world load
            // (0xC0000005 reading near 0x59ADF000).
            //
            // 2026-09-18 audit: this specific re-check is provably dead code, not merely belt-
            // and-suspenders. texOffset and stride are both const, set once at function entry
            // (above) from the same g_curLayout/g_stream0Stride this uvSet==1 branch already
            // uses, and the entry-point guard a few lines up (texOffset < 0 || texOffset + 4 >
            // stride) already refused anything that would make this condition true - nothing
            // between there and here can change either value. Removed rather than left to
            // increment a counter (g_uvSet1WrongStream) that can never actually be reached by
            // this path, which is its own kind of misleading zero.
            const UINT need = rangeCount * kUvBytesPerVertex;
            const UINT base = rangeFirst * kUvBytesPerVertex;
            // the draw addresses vertex i at offset + i*8, so the block must sit at or past base
            if (g_uvRingPos < base) g_uvRingPos = base;
            bool discard = false;
            if (g_uvRingPos + need > kUvRingBytes) {
                if (base + need > kUvRingBytes) { ++g_uvRingFail; ++g_uvConvertFailures; return nullptr; }
                g_uvRingPos = base;
                discard = true;
                ++g_uvRingWraps;
                // every ring entry's data is gone with the discard
                for (auto e = g_uvBuffers.begin(); e != g_uvBuffers.end();) {
                    if (e->second.ring) { if (e->second.src) e->second.src->Release(); e = g_uvBuffers.erase(e); }
                    else ++e;
                }
                UvIndexRebuild();
            }
            std::vector<unsigned char> staging;
            bool fresh = false;
            // FIX 3: the snoop slice must start at the vertex's TRUE byte address
            // (g_stream0Offset + rangeFirst*stride), not rangeFirst*stride alone - see
            // uvOffsetForRead's own comment, above. The ring's own bind-offset math needs no
            // change for this: entry.offset (= g_uvRingPos - base, below) already places vertex
            // (rangeFirst+n) at ring byte g_uvRingPos+n*8 regardless of where its SOURCE bytes
            // came from, so fixing only the read (this line) is sufficient - confirmed by
            // substitution: D3D reads this stream at bindOffset + v*8 for vertex v = rangeFirst+n,
            // i.e. (g_uvRingPos - rangeFirst*8) + (rangeFirst+n)*8 = g_uvRingPos + n*8, exactly
            // where entry n's data was written below, independent of g_stream0Offset.
            const UINT sliceOff = uvOffsetForRead + rangeFirst * stride, sliceLen = rangeCount * stride;
            try { staging.resize(sliceLen); } catch (...) { ++g_uvRingFail; return nullptr; }
            if (!SnoopCopy(g_stream0, sliceOff, staging.data(), sliceLen, &fresh) || !fresh) {
                ++g_uvDynWaiting;
                return nullptr;
            }
            void* dstData = nullptr;
            const bool wasInt = g_internal;
            g_internal = true;
            g_internalThread = GetCurrentThreadId();
            const HRESULT lh = discard ? g_uvRing->Lock(0, 0, &dstData, D3DLOCK_DISCARD)
                                       : g_uvRing->Lock(g_uvRingPos, need, &dstData, D3DLOCK_NOOVERWRITE);
            if (FAILED(lh) || !dstData) { g_internal = wasInt; ++g_uvRingFail; ++g_uvConvertFailures; return nullptr; }
            float* dst = static_cast<float*>(dstData);
            if (discard) dst = reinterpret_cast<float*>(static_cast<unsigned char*>(dstData) + g_uvRingPos);
            for (UINT n = 0; n < rangeCount; ++n) {
                const short* t = reinterpret_cast<const short*>(staging.data() + n * stride + texOffset);
                dst[n * 2 + 0] = static_cast<float>(t[0]);
                dst[n * 2 + 1] = static_cast<float>(t[1]);
            }
            g_uvRing->Unlock();
            g_internal = wasInt;
            UvBuffer entry;
            entry.src = g_stream0;
            entry.src->AddRef();
            entry.uv = g_uvRing;
            entry.ring = true;
            entry.offset = g_uvRingPos - base;
            entry.vertexCount = vertexCount;
            entry.bytes = 0;
            g_uvRingPos += need;
            ++g_uvRingAppends;
            ++g_uvDynConverted;
            g_uvBuffers[key] = entry;
            UvIndexAdd(entry.src, key);
            g_uvBindOffset = entry.offset;
            return g_uvRing;
        }
    }

    // A cap WITH A FLUSH, not a cap that stops accepting. Two caches here have already had the
    // "stop growing" cliff, where the world progressively loses whatever the cache protects as
    // new content streams in. Flushing rebuilds within a frame or two.
    if (g_uvBytesHeld + bytes > kMaxUvBufferBytes) {
        ReleaseUvBuffers();
        ++g_uvBufferFlushes;
        Log("uv buffer arena flushed at %u MB (flush #%u)",
            static_cast<unsigned>(kMaxUvBufferBytes >> 20), g_uvBufferFlushes);
    }

    // Crash fix 2026-09-14: belt-and-suspenders for the read loop below (the static path, and
    // the non-ring dynamic/snooped fallback), which both index stream 0's bytes at texOffset
    // using stream 0's stride. InstallFloatUV already refuses a stream-1 or unresolved
    // texcoord1 before this function is ever called, but this is the actual read site: refuse
    // here too, before allocating or locking anything, rather than walking past the mapped
    // vertex range - the access violation that crashed world load (0xC0000005 reading near
    // 0x59ADF000).
    //
    // 2026-09-18 audit: provably dead for the same reason as the ring path's identical check
    // above - texOffset/stride are const from function entry and already validated there, so
    // this can never be true. Removed rather than left to increment a counter that can never
    // fire.

    UvBuffer entry;
    entry.vertexCount = vertexCount;
    entry.bytes = bytes;
    if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
                                       &entry.uv, nullptr)) || !entry.uv) {
        ++g_uvFailCreate;
        ++g_uvConvertFailures;
        return nullptr;
    }

    void* srcData = nullptr;
    void* dstData = nullptr;
    std::vector<unsigned char> staging;
    const unsigned char* src8 = nullptr;
    if (dynamicSrc) {
        // Never Lock a DYNAMIC buffer for reading. Take the copy the lock hook already made -
        // and take ONLY THE SLICE THIS DRAW READS.
        //
        // The first attempt asked for the whole declared buffer and required every byte fresh.
        // Both are wrong, and the snoop says so in its own comments: it "grows to what the game
        // actually writes, which is a fraction of most buffers' declared size", so a whole-buffer
        // request fails the `offset + len > c.data.size()` bound outright; and D3DLOCK_DISCARD
        // clears every fresh bit, so after a partial refill most of the buffer is legitimately
        // stale. Result: 0 converted, 24,772 waiting, with the report's own warning firing.
        // FIX 3: this is the SAME ring-unavailable fallback and needs the SAME fix as the ring's
        // own sliceOff, above - the true byte address is g_stream0Offset + rangeFirst*stride, not
        // rangeFirst*stride alone.
        bool fresh = false;
        const UINT sliceOff = uvOffsetForRead + rangeFirst * stride;
        const UINT sliceLen = rangeCount * stride;
        staging.resize(sliceLen);
        if (!SnoopCopy(g_stream0, sliceOff, staging.data(), sliceLen, &fresh) || !fresh) {
            entry.uv->Release();
            ++g_uvDynWaiting;
            return nullptr;      // not a conversion failure - the snoop has no data yet
        }
        src8 = staging.data();
    } else {
        if (FAILED(g_stream0->Lock(0, 0, &srcData, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) ||
            !srcData) {
            entry.uv->Release();
            ++g_uvFailLock;
            ++g_uvConvertFailures;
            return nullptr;
        }
        src8 = static_cast<const unsigned char*>(srcData);
    }
    if (FAILED(entry.uv->Lock(0, 0, &dstData, D3DLOCK_DISCARD)) || !dstData) {
        if (!dynamicSrc) g_stream0->Unlock();
        entry.uv->Release();
        ++g_uvFailLock;
        ++g_uvConvertFailures;
        return nullptr;
    }

    // RAW short values widened to float, NOT scaled. The uv texture matrix already carries the
    // 1/1024 and the per-material tiling, and it is the same convention the skinned path uses -
    // the one converted population that renders with correct textures today. Two owners for one
    // scale is how the two come to disagree.
    float* dst = static_cast<float*>(dstData);
    // Vertices outside the drawn slice are never read by this draw, but they must not be left as
    // whatever the allocator handed us: a later draw keyed to a different range would index them.
    if (dynamicSrc && rangeCount != vertexCount) memset(dst, 0, bytes);
    for (UINT n = 0; n < rangeCount; ++n) {
        const UINT i = rangeFirst + n;
        // src8 is the slice for a dynamic source and the whole buffer for a static one. FIX 3:
        // a dynamic slice already starts at the true byte address (sliceOff, above), so its own
        // vertices need no further shift; a static source is read from byte 0 of the WHOLE
        // buffer, so its decode still needs uvPhase added, exactly like SkinFFBufferFor's own.
        const UINT srcVertex = dynamicSrc ? n : i;
        const UINT srcPhase = dynamicSrc ? 0u : uvPhase;
        const short* t = reinterpret_cast<const short*>(src8 + srcPhase + srcVertex * stride + texOffset);
        dst[i * 2 + 0] = static_cast<float>(t[0]);
        dst[i * 2 + 1] = static_cast<float>(t[1]);
    }
    entry.uv->Unlock();
    if (!dynamicSrc) g_stream0->Unlock();
    if (dynamicSrc) ++g_uvDynConverted;
    // FIX 3: only the static (whole-buffer) path needs uvBase folded into the bind offset - see
    // the cache-hit branch's identical comment, above. A dynamic entry's offset (always 0 here,
    // the ring-unavailable fallback) is unaffected.
    if (!dynamicSrc) g_uvBindOffset = entry.offset + uvBase * kUvBytesPerVertex;

    // Referenced, because the key is its address. A freed buffer whose address is recycled would
    // otherwise hand a later mesh this one's coordinates - the defect already found in three
    // other pointer-keyed caches here.
    entry.src = g_stream0;
    entry.src->AddRef();
    g_uvBytesHeld += bytes;
    ++g_uvBuffersMade;
    g_uvBuffers[key] = entry;
    UvIndexAdd(entry.src, key);
    return entry.uv;
}

// The game refilled a buffer we have converted: our coordinates describe its old contents.
void InvalidateUvBuffers(IDirect3DVertexBuffer9* vb) {
    g_vbDescCache.erase(vb);
    if (g_settings.indexedInvalidation) {
        const auto list = g_uvByVB.find(vb);
        if (list == g_uvByVB.end()) return;
        for (const unsigned long long key : list->second) {
            const auto it = g_uvBuffers.find(key);
            if (it == g_uvBuffers.end() || it->second.src != vb) continue;
            g_uvBytesHeld -= it->second.bytes;
            it->second.src->Release();
            if (it->second.uv && !it->second.ring) it->second.uv->Release();
            g_uvBuffers.erase(it);
            ++g_uvInvalidations;
        }
        g_uvByVB.erase(list);
        return;
    }
    for (auto it = g_uvBuffers.begin(); it != g_uvBuffers.end();) {
        if (it->second.src == vb) {
            g_uvBytesHeld -= it->second.bytes;
            it->second.src->Release();
            if (it->second.uv && !it->second.ring) it->second.uv->Release();
            it = g_uvBuffers.erase(it);
            ++g_uvInvalidations;
        } else {
            ++it;
        }
    }
}

// A clone of the game's declaration with TEXCOORD0 moved onto our float2 stream. Position, normal
// and the other texcoord sets still come from the game's own buffers, so the geometry itself is
// untouched and only the coordinates change.
// FIX A: split by SET (index 0 or 1, from DecalUvSourceSet), not just by source declaration.
// The same vertex declaration can serve both an ordinary shader (set 0) and one of FIX A's
// decal-only shaders (set 1) over the same shared mesh format, and each needs a DIFFERENT
// element replaced - caching on `src` alone would hand one shader's clone to the other.
std::unordered_map<IDirect3DVertexDeclaration9*, IDirect3DVertexDeclaration9*> g_uvDecls[2];
unsigned g_uvDeclsMade = 0;
unsigned g_uvDeclFailures = 0;
// Set 1 only: a declaration that ALSO carries a genuine TEXCOORD0 element alongside the
// TEXCOORD1 element FIX A retargets. That TEXCOORD0 element has to be moved off usage-index 0
// before the retarget happens, or CreateVertexDeclaration sees two elements both claiming
// TEXCOORD usage-index 0 and rejects the whole declaration. See FloatUVDeclaration below.
unsigned g_uvDeclTexcoord0Displaced = 0;

IDirect3DVertexDeclaration9* FloatUVDeclaration(IDirect3DDevice9* dev,
                                                IDirect3DVertexDeclaration9* src) {
    if (!src) return nullptr;
    const int uvSet = DecalUvSourceSet();
    auto& cache = g_uvDecls[uvSet];
    const auto it = cache.find(src);
    if (it != cache.end()) return it->second;

    D3DVERTEXELEMENT9 elems[64]{};
    UINT count = 0;
    IDirect3DVertexDeclaration9* clone = nullptr;
    if (SUCCEEDED(src->GetDeclaration(elems, &count)) && count > 0) {
        bool changed = false;
        bool abandon = false;
        if (uvSet == 1) {
            // Which draw takes the set-1 path is decided by the VERTEX SHADER, independent of
            // what this declaration carries. If the declaration happens to have both a real
            // TEXCOORD0 element and the TEXCOORD1 element the loop below retargets, the retarget
            // would leave two elements both claiming TEXCOORD usage-index 0 - not a valid D3D9
            // declaration, and CreateVertexDeclaration would (correctly) refuse it, which would
            // silently fall back to the game's own broken declaration. Move the TEXCOORD0
            // element onto whatever TEXCOORD usage index is actually free instead. This is safe:
            // nothing in the fixed-function path reads any texcoord set other than index 0,
            // because D3DTSS_TEXCOORDINDEX is 0 for stage 0 and stages 1-7 are disabled, so the
            // displaced element's data is still there for anything that might look, just under
            // an index nothing samples - it is relabelled, not lost.
            bool usedIndex[8] = {};
            int zeroIdx = -1;
            for (UINT i = 0; i < count; ++i) {
                if (elems[i].Stream == 0xFF) break;
                if (elems[i].Usage == D3DDECLUSAGE_TEXCOORD && elems[i].UsageIndex < 8) {
                    usedIndex[elems[i].UsageIndex] = true;
                    if (elems[i].UsageIndex == 0) zeroIdx = static_cast<int>(i);
                }
            }
            if (zeroIdx >= 0) {
                int freeIndex = -1;
                for (int j = 1; j < 8; ++j) {
                    if (!usedIndex[j]) { freeIndex = j; break; }
                }
                if (freeIndex < 0) {
                    abandon = true;   // no room to displace it - fall back, like any other failure
                } else {
                    elems[zeroIdx].UsageIndex = static_cast<BYTE>(freeIndex);
                    ++g_uvDeclTexcoord0Displaced;
                }
            }
        }
        for (UINT i = 0; !abandon && i < count; ++i) {
            if (elems[i].Stream == 0xFF) break;
            // Set 0: only TEXCOORD0 - the set fixed function samples, since
            // D3DTSS_TEXCOORDINDEX is 0. Set 1 (FIX A): the decalonly/fauxinterior families
            // declare NO usage-index-0 input at all, so the element to replace is TEXCOORD1 -
            // and it must come out the other side relabelled AS usage-index 0, because the
            // fixed-function stage still only ever samples index 0, whatever the source data's
            // own index was.
            const bool isTarget = elems[i].Usage == D3DDECLUSAGE_TEXCOORD &&
                                  elems[i].Type == D3DDECLTYPE_SHORT2 &&
                                  elems[i].UsageIndex == (uvSet == 1 ? 1 : 0);
            if (isTarget) {
                elems[i].Stream = static_cast<WORD>(g_uvStream);
                elems[i].Offset = 0;
                elems[i].Type = D3DDECLTYPE_FLOAT2;
                elems[i].UsageIndex = 0;   // set 0: already 0. set 1: the whole point of FIX A.
                changed = true;
            }
        }
        if (!abandon && changed && FAILED(dev->CreateVertexDeclaration(elems, &clone))) clone = nullptr;
    }
    if (!clone) ++g_uvDeclFailures; else ++g_uvDeclsMade;

    src->AddRef();
    cache[src] = clone;   // null is cached too, so a failure is not retried every draw
    return clone;
}

HRESULT WINAPI Hook_SetVertexDeclaration(IDirect3DDevice9* dev,
                                         IDirect3DVertexDeclaration9* decl) {
    ++g_callDecl;
    if (!InternalHere()) g_curDecl = decl;
    const auto it = g_layouts.find(decl);
    if (it != g_layouts.end()) {
        g_curLayout = it->second;
    } else {
        g_curLayout = ParseDeclaration(decl);
        // Referenced for the same reason as g_shaders: a recycled declaration address would
        // return another mesh's field offsets, and skinning reads blend weights and indices
        // through exactly those offsets.
        if (decl && g_layouts.emplace(decl, g_curLayout).second) decl->AddRef();
        // One line per distinct declaration. The texcoord type is the number that matters:
        // fixed function reads short2 as a raw integer, so anything other than float2 needs
        // the uvScale texture matrix before its textures can land correctly.
        if (g_settings.logLayouts && decl && g_layoutReports < 24) {
            ++g_layoutReports;
            Log("vertex layout #%u: elems=%d pos=%s@%d(stream %d) uv=%s normal=%d color=%d "
                "skinned=%d positionT=%d",
                g_layoutReports, g_curLayout.elements, DeclTypeName(g_curLayout.posType),
                g_curLayout.posOffset, g_curLayout.posStream,
                DeclTypeName(g_curLayout.texcoordType), g_curLayout.hasNormal,
                g_curLayout.hasColor, g_curLayout.skinned, g_curLayout.hasPositionT);
            // For a skinned declaration the summary is not enough to write a decoder against,
            // so dump every element. Only the skinned ones, and only the first few: this is the
            // input to the CPU skinning port and nothing else needs it.
            if (g_curLayout.skinned && g_skinnedDeclDumps < 4) {
                ++g_skinnedDeclDumps;
                D3DVERTEXELEMENT9 all[64]{};
                UINT n = 0;
                if (SUCCEEDED(decl->GetDeclaration(all, &n))) {
                    for (UINT i = 0; i < n; ++i) {
                        if (all[i].Stream == 0xFF) break;
                        Log("    stream=%u offset=%2u type=%-8s(%u) usage=%-12s index=%u",
                            all[i].Stream, all[i].Offset, DeclTypeName(all[i].Type),
                            all[i].Type, UsageName(all[i].Usage), all[i].UsageIndex);
                    }
                }
            }
        }
    }
    return g_origSetVertexDeclaration(dev, decl);
}

HRESULT WINAPI Hook_SetStreamSource(IDirect3DDevice9* dev, UINT stream,
                                    IDirect3DVertexBuffer9* buffer, UINT offset, UINT stride) {
    ++g_callStream;
    if (!InternalHere()) {
        if (stream == 0) {
            g_stream0 = buffer;
            g_stream0Offset = offset;
            g_stream0Stride = stride;
        }
        if (stream < kMaxStreams) {
            g_streamVB[stream] = buffer;
            g_streamOffset[stream] = offset;
            g_streamStride[stream] = stride;
        }
        // Install the shared vertex-buffer Lock/Unlock hooks the first time we see any buffer.
        // All IDirect3DVertexBuffer9 instances share one vtable, so one patch covers them all.
        if (buffer && !g_origVBLock) {
            PatchVTable(buffer, kSlotVBLock, &Hook_VBLock,
                        reinterpret_cast<void**>(&g_origVBLock));
            PatchVTable(buffer, kSlotVBUnlock, &Hook_VBUnlock,
                        reinterpret_cast<void**>(&g_origVBUnlock));
        }
    }
    return g_origSetStreamSource(dev, stream, buffer, offset, stride);
}

// Hardware instancing draws many copies of one mesh, each positioned from a per-instance
// stream. Fixed function has no equivalent, so a converted instanced draw collapses every copy
// onto a single transform - a plausible reading of "the city is bunched up". Whether SR3 uses
// it at all is unknown, so this both measures and guards.
unsigned g_freqReports = 0;
HRESULT WINAPI Hook_SetStreamSourceFreq(IDirect3DDevice9* dev, UINT stream, UINT setting) {
    if (!InternalHere()) {
        if (stream == 0) {
            // D3DSTREAMSOURCE_INDEXEDDATA (0x40000000) | instanceCount marks the geometry
            // stream of an instanced draw; a plain 1 resets it.
            g_instancedDraw = (setting & D3DSTREAMSOURCE_INDEXEDDATA) != 0;
            g_instanceCount = g_instancedDraw ? (setting & 0x3FFFFFFF) : 1;
        }
        if (stream < 4) {
            g_streamFreq[stream] = setting;
        }
        // Record the shapes actually used, so per-instance conversion can be written against
        // measurements rather than an assumed layout.
        if (g_freqReports < 12) {
            ++g_freqReports;
            Log("stream freq #%u: stream=%u setting=%#010x -> %s count=%u", g_freqReports,
                stream, setting,
                (setting & D3DSTREAMSOURCE_INSTANCEDATA) ? "INSTANCEDATA" :
                (setting & D3DSTREAMSOURCE_INDEXEDDATA) ? "INDEXEDDATA" : "plain",
                setting & 0x3FFFFFFF);
        }
    }
    return g_origSetStreamSourceFreq(dev, stream, setting);
}

// SR3 stores texture coordinates as short2 on essentially all world geometry. Fixed function
// reads those as raw integers, so the UVs arrive in the thousands and every texture tiles into
// noise. The divisor is not documented anywhere we have found, so measure it: read the actual
// range a few meshes occupy. A mesh whose UVs span roughly 0..N for one texture repeat gives N
// directly, and the value is almost always a power of two.
unsigned g_uvReports = 0;
void MeasureShortUVs(UINT minIndex, UINT numVertices) {
    if (g_uvReports >= 12 || !g_stream0) return;
    if (g_curLayout.texcoordType != D3DDECLTYPE_SHORT2) return;
    if (g_curLayout.texcoordOffset < 0 || g_stream0Stride < 4) return;
    if (numVertices < 3 || numVertices > 4096) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    const UINT bytes = numVertices * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, bytes, &mapped, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) ||
        !mapped)
        return;

    int minU = 32767, maxU = -32768, minV = 32767, maxV = -32768;
    const unsigned char* p = static_cast<const unsigned char*>(mapped);
    for (UINT i = 0; i < numVertices; ++i) {
        const short* uv = reinterpret_cast<const short*>(
            p + i * g_stream0Stride + g_curLayout.texcoordOffset);
        if (uv[0] < minU) minU = uv[0];
        if (uv[0] > maxU) maxU = uv[0];
        if (uv[1] < minV) minV = uv[1];
        if (uv[1] > maxV) maxV = uv[1];
    }
    g_stream0->Unlock();

    ++g_uvReports;
    Log("short2 UV range #%u: u %d..%d  v %d..%d  (verts=%u stride=%u off=%d)",
        g_uvReports, minU, maxU, minV, maxV, numVertices, g_stream0Stride,
        g_curLayout.texcoordOffset);
}

// Where does a converted draw actually land? Steelport's coordinates run to the low thousands,
// so anything an order of magnitude beyond that is geometry we have transformed wrongly - the
// "polygons stretching across the whole world". Reporting the offenders with their layout says
// WHICH class of draw is broken, instead of inferring it from shader names.
unsigned g_extentReports = 0;
void MeasureWorldExtent(UINT minIndex, UINT numVertices) {
    // Gate the LOCK, not just the logging. The report was capped at 20 lines but the buffer lock
    // was not, so this locked stream 0 on every converted draw - roughly 1,400 vertex-buffer
    // locks per frame, each one a bridge round trip and a potential GPU sync, purely to feed a
    // diagnostic that had already stopped printing. Capping the log while leaving the work in
    // place is a mistake worth remembering.
    if (g_extentReports >= 20) return;
    if (!g_stream0 || g_stream0Stride < 12) return;
    if (g_curLayout.posOffset < 0) return;
    if (numVertices < 3 || numVertices > 4096) return;

    const UINT first = g_stream0Offset + minIndex * g_stream0Stride;
    void* mapped = nullptr;
    if (FAILED(g_stream0->Lock(first, numVertices * g_stream0Stride, &mapped,
                               D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)) || !mapped)
        return;

    float worst = 0.0f;
    const unsigned char* p = static_cast<const unsigned char*>(mapped);
    const UINT step = (numVertices > 32) ? (numVertices / 32) : 1;   // sample, do not scan all
    for (UINT i = 0; i < numVertices; i += step) {
        const float* v = reinterpret_cast<const float*>(
            p + i * g_stream0Stride + g_curLayout.posOffset);
        if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) {
            worst = 1e30f;
            break;
        }
        const D3DVECTOR w = TransformPoint(v, g_appliedWorld);
        const float m = max(max(std::fabs(w.x), std::fabs(w.y)), std::fabs(w.z));
        if (m > worst) worst = m;
    }
    g_stream0->Unlock();

    if (worst > 20000.0f) {
        ++g_outlierDraws;
        if (g_extentReports < 20) {
            ++g_extentReports;
            Log("OUTLIER draw #%u: max |world| = %.0f | verts=%u stride=%u elems=%d pos=%s@%d "
                "uv=%s objTM=%d worldWritten=%d",
                g_extentReports, worst, numVertices, g_stream0Stride, g_curLayout.elements,
                DeclTypeName(g_curLayout.posType), g_curLayout.posOffset,
                DeclTypeName(g_curLayout.texcoordType), g_curVS.usesObjTM, g_worldWritten);
        }
    }
}

void RecordFrameDraw(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d);
void ProbeAtlasComposite(IDirect3DDevice9* dev, UINT numVertices, UINT primitiveCount, Disp d);
// Defined below, with the HUD rebuild they share their vertex type with. Hook_DrawPrimitive is
// above that point in this file, which is the ordering trap this project has hit four times.
void NotePassedThrough(UINT primitiveCount);
bool IsAtlasComposite(UINT primitiveCount, Disp d);
bool DrawCompositeFixedFunction(IDirect3DDevice9* dev);

void ClearBackBufferOnce(IDirect3DDevice9* dev);   // defined below, with its rationale

// Inside an occlusion query (between Issue(BEGIN) and Issue(END) on a D3DQUERYTYPE_OCCLUSION).
// Cleared at frame end as well, so a query the engine never closes cannot pin it.
bool g_inOcclusionQuery = false;
unsigned long long g_occlDrawsSeen = 0, g_occlDrawsSmall = 0, g_occlDrawsLarge = 0,
                   g_occlDrawsSkipped = 0, g_occlIssueSkipped = 0, g_declinedPassSkipped = 0,
                   g_passForwarded = 0;
unsigned g_occlDrawReports = 0;
// THE OCCLUSION PLAN - what the culler will need, measured before it is written.
// The proxy drawn inside each query is that object's bounding box; whether it arrives as a unit
// cube placed by objTM or as world-space corners decides how the culler reads it. The query
// pointer ties the box to the GetData the engine will read for it.

