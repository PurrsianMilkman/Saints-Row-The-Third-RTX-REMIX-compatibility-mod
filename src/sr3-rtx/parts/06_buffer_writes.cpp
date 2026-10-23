// parts/06_buffer_writes.cpp - recent vertex-buffer writes and the VB lock/unlock hooks.
// NOT a standalone translation unit: #included, in order, by ../sr3rtx.cpp inside its anonymous
// namespace. Originally sr3rtx.cpp lines 3058-3521 (old file:line citations map here).
// Render thread only. Called at the top of every draw, before anything takes a pointer into the
// caches those invalidations would erase from.
// ---------------------------------------------------------------- recent buffer writes
// Which byte ranges of which vertex buffers the game wrote, and in which frame. Fed from the
// drained lock queue on the render thread; asked by the draw hook to defer a converted draw
// whose static buffer range is fresher than deferFreshFrames, and by the rolling draw record.
struct RecentWrite { UINT offset, size; bool whole; unsigned frame; };
std::unordered_map<IDirect3DVertexBuffer9*, std::vector<RecentWrite>> g_recentWrites;
unsigned long long g_deferredFresh = 0, g_freshSeen = 0;
void NoteRecentWrite(IDirect3DVertexBuffer9* vb, UINT offset, UINT size, bool whole) {
    try {
        auto& v = g_recentWrites[vb];
        for (auto it = v.begin(); it != v.end();)
            if (g_frames > it->frame + 8) it = v.erase(it); else ++it;
        if (v.size() >= 8) v.erase(v.begin());
        v.push_back(RecentWrite{offset, size, whole, g_frames});
    } catch (...) {}
}
// Frames since the newest write overlapping [begin, end), or -1 when none within `within`.
int FramesSinceWrite(IDirect3DVertexBuffer9* vb, UINT begin, UINT end, unsigned within) {
    const auto it = g_recentWrites.find(vb);
    if (it == g_recentWrites.end()) return -1;
    int best = -1;
    for (const RecentWrite& w : it->second) {
        if (g_frames > w.frame + within) continue;
        if (!w.whole && !(w.offset < end && begin < w.offset + w.size)) continue;
        const int age = static_cast<int>(g_frames - w.frame);
        if (best < 0 || age < best) best = age;
    }
    return best;
}
bool IsDynamicVB(IDirect3DVertexBuffer9* vb) {
    if (!vb) return false;
    const auto it = g_vbDescCache.find(vb);
    if (it != g_vbDescCache.end()) return (it->second.Usage & D3DUSAGE_DYNAMIC) != 0;
    D3DVERTEXBUFFER_DESC d{};
    if (FAILED(vb->GetDesc(&d))) return false;
    try { g_vbDescCache[vb] = d; } catch (...) {}
    return (d.Usage & D3DUSAGE_DYNAMIC) != 0;
}
void PruneRecentWrites() {
    for (auto it = g_recentWrites.begin(); it != g_recentWrites.end();) {
        bool live = false;
        for (const RecentWrite& w : it->second) if (g_frames <= w.frame + 8) { live = true; break; }
        if (!live) it = g_recentWrites.erase(it); else ++it;
    }
}

void DrainPendingInvalidations() {
    if (!g_snoopCsReady) return;
    std::vector<PendingInvalidation> todo;
    {
        SnoopGuard g;
        if (g_pendingInvalidations.empty()) return;
        todo.swap(g_pendingInvalidations);
    }
    for (const PendingInvalidation& p : todo) {
        InvalidateUvBuffers(p.vb);
        InvalidateBaseMeshes(p.vb);
        InvalidateSkinFFBuffers(p.vb);
        InvalidateMorphBakesBySrc(p.vb);   // THE SHAPE BAKE
        InvalidateOccluders(p.vb, p.whole ? 0 : p.offset, p.whole ? 0 : p.size);
        NoteRecentWrite(p.vb, p.offset, p.size, p.whole);
        ++g_deferredInvalidations;
    }
}
// Defined with the skinning, further down - every read-lock of a game vertex buffer uses it.
bool VertexRangeFits(IDirect3DVertexBuffer9* vb, UINT streamOffset, UINT firstVertex,
                     UINT count, UINT stride);

HRESULT WINAPI Hook_VBLock(IDirect3DVertexBuffer9* self, UINT offset, UINT size, void** data,
                           DWORD flags) {
    const HRESULT hr = g_origVBLock(self, offset, size, data, flags);
    if (InternalHere() || FAILED(hr) || !data || !*data) return hr;
    if (flags & D3DLOCK_READONLY) return hr;   // the game is reading; nothing new to capture

    SnoopGuard guard;   // everything below touches state the render thread also reads

    // A discard invalidates everything we hold for this buffer, even the bytes it does not go on
    // to rewrite. Recorded rather than assumed away.
    if (flags & D3DLOCK_DISCARD) {
        const auto di = g_instCache.find(self);
        if (di != g_instCache.end()) {
            std::fill(di->second.fresh.begin(), di->second.fresh.end(), 0);
            ++g_instDiscards;
            // THE SHAPE BAKE
            ++di->second.writeSeq;
            ++di->second.discardSeq;
            if (di->second.isMorph) ++g_morphVbDiscards;
        }
    }

    // The converted texture coordinates for this buffer describe what it held a moment ago. The
    // buffer being refilled is exactly the case a reference on it cannot protect against, and
    // this is the only place that sees it happen.
    // The bind-pose cache has the same problem. Its key is {vb, offset, stride, minIndex, count},
    // which names a SLOT in a buffer, not a mesh - so once the game refills that slot with a
    // different character's vertices, every later draw from it is served the previous occupant's
    // geometry. That is "each character had the wrong head".
    //
    // QUEUED, not done here: both invalidations erase from containers the render thread reads,
    // and this hook does not run on the render thread. Doing it inline frees a BaseMesh while a
    // skinning loop is walking it.
    //
    // DEDUPED. Measured over 26,400 frames: 7,185,052 queued - 272 a frame - and
    // `bind-pose cache invalidated by a game write: 0 times`, so every one of them found nothing.
    // The game relocks a handful of dynamic buffers constantly; queuing the same pointer twice
    // between drains asks for the same lookup twice. The scan is over a list that is a few
    // entries long precisely because of this.
    {
        // whole-buffer for DISCARD and for size 0 (to the end); otherwise the locked range.
        // Two ranges on one buffer coalesce to their hull - conservative.
        const bool whole = (flags & D3DLOCK_DISCARD) != 0 || size == 0;
        bool queued = false;
        for (PendingInvalidation& q : g_pendingInvalidations)
            if (q.vb == self) {
                queued = true;
                if (whole) q.whole = true;
                else if (!q.whole) {
                    const UINT end = max(q.offset + q.size, offset + size);
                    q.offset = min(q.offset, offset);
                    q.size = end - q.offset;
                }
                break;
            }
        if (!queued) g_pendingInvalidations.push_back(PendingInvalidation{self, offset, size, whole});
        else ++g_invalidationsCoalesced;
    }

    // Only buffers we have already identified as instance streams are worth snooping.
    const auto it = g_instCache.find(self);
    if (it == g_instCache.end()) return hr;

    UINT span = size;
    if (span == 0) {                    // size 0 means "to the end of the buffer"
        D3DVERTEXBUFFER_DESC d{};
        if (FAILED(self->GetDesc(&d))) return hr;
        span = (d.Size > offset) ? (d.Size - offset) : 0;
    }
    g_pendingLock = {self, *data, offset, span, GetCurrentThreadId()};
    return hr;
}

HRESULT WINAPI Hook_VBUnlock(IDirect3DVertexBuffer9* self) {
    // SNAPSHOT the pending lock and clear it BEFORE touching anything else.
    //
    // The previous version tested g_pendingLock.ptr, then called vector::resize, then passed
    // g_pendingLock.ptr to memcpy. resize is opaque to the optimiser, so the pointer is RE-READ
    // from the global after the test - and this hook is not called from one thread. Crash dump
    // 2026-08-28 00:59: ACCESS_VIOLATION at CopyUpLargeMov+0xa with esi=0, i.e.
    // memcpy(dst, NULL, 4176). 4176 is 348 vertices x the 12-byte morph stride.
    //
    // The window had always been there and never mattered: only instance streams were snooped,
    // and the game fills those on the render thread. Registering the morph buffer put a stream
    // the game writes elsewhere through the same single global.
    //
    // Locals close it completely - nothing is read from the global after the null test.
    //
    // OWNER-THREAD GATED (2026-09-18 audit fix), replacing the September 14 unconditional
    // consume. That change's stated invariant - "our own internal locks are never on a buffer a
    // real game lock has pending at the same moment" - is not actually enforced anywhere:
    // UvBufferFor's RegisterSnoop(g_stream0) turns every DYNAMIC stream-0 buffer into a snooped
    // instance stream, and roughly a dozen probes (BakeDecal, MeasureShortUVs, OcclMeshFor and
    // others) read-lock g_stream0 without checking DYNAMIC first. So this DOES mis-fire: the
    // render thread's own internal read-Unlock can land while a GAME thread's write Lock on the
    // very same buffer is still open, silently consume THAT pending entry, copy a half-written
    // buffer into the snoop cache, and clear the slot before the game's own real Unlock ever
    // runs - whose finished write is then never snooped. Comparing the owner thread fixes that
    // while keeping the September 14 case working: a main-thread unlock still consumes its own
    // main-thread entry regardless of the internal flag, because pl.owner names that SAME thread
    // here - only a DIFFERENT thread's Unlock is now refused the entry.
    SnoopGuard guard;
    PendingLock pl = g_pendingLock;
    const bool sameOwnerThread =
        !g_settings.vbUnlockOwnerThread || pl.owner == GetCurrentThreadId();
    if (pl.vb == self && pl.ptr && sameOwnerThread) g_pendingLock = {};
    if (pl.vb == self && pl.ptr && pl.size && sameOwnerThread) {
        InstanceCache& c = g_instCache[self];
        // Grows to what the game actually writes, which is a fraction of most buffers'
        // declared size. Safe to resize here because nothing outside this lock holds a pointer
        // into it any more - readers copy out (SnoopCopy) rather than borrowing.
        //
        // Capped in total, and allocation failure is CAUGHT. The 2026-08-28 build that reserved
        // every buffer's declared size up front exhausted a 32-bit address space, and the
        // bad_alloc that came out of it was unhandled: "This application has requested the
        // Runtime to terminate it in an unusual way" is std::terminate, not a segfault. A shim
        // must degrade rather than abort the process it is a guest in.
        const UINT end = pl.offset + pl.size;
        bool grew = true;
        if (c.data.size() < end) {
            const size_t add = end - c.data.size();
            if (g_snoopBytes + add * 2 > kMaxSnoopBytes) {
                ++g_snoopBudgetRefusals;
                grew = false;
            } else {
                try {
                    c.data.resize(end);
                    c.fresh.resize(end, 0);
                    g_snoopBytes += add * 2;
                } catch (const std::bad_alloc&) {
                    ++g_snoopAllocFailures;
                    grew = false;
                }
            }
        }
        if (grew && c.data.size() >= end) {
            // pl.ptr is the same kind of pointer as the atlas snoop's p.bits: captured at Lock,
            // held across the gap to this Unlock, and dead the instant a foreign thread's
            // DISCARD or release runs in between. This buffer never had either of the atlas
            // copy's two defences until now - the 2026-08-28 fix only removed a NULL re-read,
            // and the STALE NON-NULL case (a pointer that still looks valid but is no longer
            // this lock's memory) was never addressed. Guarded exactly the same way: validate
            // the whole range with VirtualQuery (gated on snoopValidateSource, the same switch
            // the atlas copy uses), then run the copy itself under structured-exception
            // protection regardless, since a range that just passed validation can still be
            // unmapped a microsecond later.
            bool copied = false;
            const void* failAddr = nullptr;
            MEMORY_BASIC_INFORMATION failInfo{};
            const bool rangeOk = !g_settings.snoopValidateSource ||
                MemoryRangeReadable(pl.ptr, pl.size, &failAddr, &failInfo);
            if (!rangeOk) {
                ++g_vbCopyRefused;
                if (g_vbCopyRefused <= 4)
                    Log("VB LOCK HOOK: SKIPPED a stale/unmapped source range before copying - "
                        "vb=%p src=%p size=%u, failing page at %p state=%#lx protect=%#lx "
                        "allocBase=%p", static_cast<void*>(self), pl.ptr, pl.size, failAddr,
                        failInfo.State, failInfo.Protect, failInfo.AllocationBase);
            } else {
                copied = CopyRangeGuarded(c.data.data() + pl.offset, pl.ptr, pl.size);
                if (!copied) ++g_vbCopyRefused;
            }
            if (copied) std::fill(c.fresh.begin() + pl.offset, c.fresh.begin() + end, 1);
        }
        // THE SHAPE BAKE: counted here so it fires once per real Unlock-with-pending-write,
        // whether or not the byte range grew/copied cleanly above.
        ++c.writeSeq;
        if (c.isMorph) { ++g_morphVbWrites; g_morphVbBytes += pl.size; }
        c.valid = true;
        c.frame = g_frames;
        ++g_instanceLocks;
    }
    return g_origVBUnlock(self);
}

// Whole-buffer snapshot of an instance stream, refreshed once per frame.
// Pure cache lookup - this NEVER locks. Creating the map entry is what registers the buffer as
// an instance stream so Hook_VBLock/Hook_VBUnlock start snooping the game's own writes to it;
// the first draw that touches a new buffer therefore finds nothing and is not converted, and
// every draw after the game next fills it is served from the snooped copy.
// Register a buffer for snooping. Creating the map entry is what makes Hook_VBLock/Hook_VBUnlock
// start copying the game's own writes to it; the first draw that touches a new buffer therefore
// finds nothing, and every draw after the game next fills it is served from the snooped copy.
//
// This used to RETURN a pointer into the cache. It no longer does, and must not again: the
// locking thread resizes those vectors, so any pointer handed out here dangles the moment it
// does. Readers use SnoopCopy.
void RegisterSnoop(IDirect3DVertexBuffer9* vb) {
    if (!vb) return;
    SnoopGuard guard;
    const size_t before = g_instCache.size();
    g_instCache[vb];
    if (g_instCache.size() != before) vb->AddRef();
}

// Copy a range out of a snooped buffer, under the lock, reporting whether every byte of it had
// actually been written by the game since the last discard.
//
// This replaces handing callers a raw pointer into the cache. A caller keeps what it reads for
// the length of a draw, and the locking thread can resize that vector at any moment - so the
// pointer was a use-after-free waiting for the timing to line up. Copying costs the bytes a draw
// actually consumes (a head is 1,426 vertices x 12) and nothing else.
bool SnoopCopy(IDirect3DVertexBuffer9* vb, UINT offset, unsigned char* dst, UINT len,
               bool* allFresh) {
    if (!vb || !dst || !len) return false;
    SnoopGuard guard;
    const auto it = g_instCache.find(vb);
    if (it == g_instCache.end()) return false;
    const InstanceCache& c = it->second;
    if (!c.valid || offset + static_cast<size_t>(len) > c.data.size()) return false;
    memcpy(dst, c.data.data() + offset, len);
    if (allFresh) {
        // Was a manual byte-by-byte scan (up to 1.25 MB/frame measured, ~0.42 ms at one cycle a
        // byte, held the whole time under g_snoopCs - the same critical section the game's own
        // streaming thread takes to lock/unlock a buffer). The guard below reproduces the loop's
        // "b >= c.fresh.size()" arm exactly: any byte in [offset, offset+len) is out of range iff
        // offset+len > c.fresh.size(), since the range is contiguous and fresh.size() is a single
        // cutoff. Once the range is known in-bounds, memchr for a zero byte is exactly the loop's
        // "!c.fresh[b]" test - fresh is std::vector<unsigned char>, one byte per flag, and memchr
        // finding no zero means every byte in range is non-zero, i.e. every byte is fresh.
        //
        // data and fresh are grown together (Hook_VBUnlock) and are normally the same size, but a
        // bad_alloc from c.fresh.resize can leave fresh smaller than data after c.data.resize has
        // already succeeded - data.size() alone does not catch that, which is exactly why the
        // original loop re-checked c.fresh.size() on every byte rather than once before it. The
        // explicit guard here checks the same thing, once.
        if (offset + static_cast<size_t>(len) > c.fresh.size()) *allFresh = false;
        else *allFresh = (memchr(c.fresh.data() + offset, 0, len) == nullptr);
    }
    return true;
}

// World matrix of an instanced draw, read from its instance stream. Instance count is always 1
// in SR3, so the record sits at the stream's base offset.
bool InstanceWorld(D3DMATRIX& world) {
    if (!g_curLayout.hasInstanceTransform) return false;
    const int s = g_curLayout.instStream;
    // Registering is what starts the snoop. This used to be a side effect of the accessor that
    // handed out a pointer; removing that accessor without this line would have silently stopped
    // ~1,400 instanced draws a frame from converting, with nothing failing loudly.
    RegisterSnoop(g_streamVB[s]);
    const UINT rec = g_streamOffset[s];
    float rows[12];
    bool allFresh = true;
    for (int r = 0; r < 3; ++r) {
        const UINT off = rec + static_cast<UINT>(g_curLayout.instRowOffset[r]);
        bool rowFresh = false;
        if (!SnoopCopy(g_streamVB[s], off, reinterpret_cast<unsigned char*>(&rows[r * 4]), 16,
                       &rowFresh))
            return false;
        if (!rowFresh) allFresh = false;
    }
    if (allFresh) ++g_instFreshTransform;
    else {
        ++g_instStaleTransform;
        if (g_instStaleReports < 6) {
            ++g_instStaleReports;
            Log("INSTANCE TRANSFORM STALE #%u: stream %d offset %u - these bytes were not written "
                "since the last discard. translation (%.1f %.1f %.1f)",
                g_instStaleReports, s, rec, rows[3], rows[7], rows[11]);
        }
    }
    world = FromRegisters(rows, 3);   // same row-major 3x4 convention as objTM
    return IsFinite(world);
}

// Build this draw's transforms. Returns false if the draw cannot be represented.
bool ComputeTransforms(D3DMATRIX& world, D3DMATRIX& view, D3DMATRIX& proj) {
    view = FromRegisters(&g_vsConst[kRegWorld2View][0], 3);
    const D3DMATRIX viewProj = FromRegisters(&g_vsConst[kRegProjTM][0], 4);
    if (!IsFinite(view) || !IsFinite(viewProj)) return false;

    // REVERTED 2026-08-18, and the measurement that killed it is worth keeping.
    //
    // The theory was that the UI pass never refreshes IR_World2View, so decomposing its projTM
    // against a stale c48 recovered the world's projection and planted the HUD in the world.
    // The first half is true. The conclusion was not: substituting view = identity and
    // projection = projTM did NOT put the UI on screen, and made it disappear at some camera
    // angles instead.
    //
    // The reason is in the log. The UI target's raw projTM is
    //     row0 (-0.5863 0.4811 0.9159 0.9159)  row3 (100.7555 -456.2618 -46.0588 -45.9083)
    // - a rotated basis with a large translation. That is neither a perspective projection nor
    // an orthographic one, so `rtx.orthographicIsUI` has nothing to latch onto and the draw is
    // just world geometry with an unusual transform. `perspective=0` in that log line means
    // "failed the perspective test", NOT "is orthographic", and reading it as the latter is what
    // made this look like a fix.
    //
    // So the 8-bit full-size target is not simply "the HUD". Whatever it is, it carries a full
    // world transform, and the UI problem needs a different model than "one pass, one
    // projection". Left as it was.
    if (!DeriveProjection(view, viewProj, proj)) return false;

    // Does this draw have an object matrix? The shader's own constant table is the authority:
    // if it declares objTM then the engine MUST have uploaded a valid one before the draw, or
    // the game's own rendering would be wrong too. If it does not declare objTM the geometry is
    // already in world space and identity is correct.
    //
    // This deliberately does NOT also require that we witnessed the c32 upload. That extra
    // condition was ported from SR2, whose comment explains it as "new VS may have different
    // constant layout" - true there, false here: SR3's register layout is fixed and documented
    // (docs/shader-map.md). Carrying it over meant that whenever the engine uploaded objTM
    // *before* binding the shader, the matrix was discarded and the object fell back to
    // identity. Measured effect: 1,828 converted draws sharing ~50 transform writes, i.e.
    // nearly all of the world drawn at one place.
    // ...but "the engine must have uploaded one" is only true for the draw it uploaded it FOR.
    // Dropping the freshness test entirely (previous build) meant a shader that declares objTM
    // without uploading inherited whatever was left in c32 - very often the player character's
    // matrix, which is why objects appeared at the character's origin and popped as that matrix
    // changed. c32 stays valid across the several draws of one multi-material object and is
    // invalidated at the next projTM upload following a draw, which is where a new setup starts.
    //
    // When a shader wants an object matrix and no fresh one exists we now REFUSE the draw
    // rather than substitute identity or a stale matrix. Drawing it in the wrong place is worse
    // than leaving it to the game: a skipped draw is merely not path-traced.
    // An instanced draw carries its transform in the instance stream instead of objTM, so that
    // takes precedence: for these the c32 registers are stale or irrelevant.
    if (g_instancedDraw) {
        if (!InstanceWorld(world)) return false;
        ++g_instanceConverted;
        return true;
    }

    if (g_curVS.usesObjTM) {
        if (!g_worldWritten) { ++g_objTMWithoutFreshUpload; return false; }
        world = FromRegisters(&g_vsConst[kRegObjTM][0], 3);
    } else {
        world = kIdentity;
    }
    return IsFinite(world);
}

void ApplyTransforms(IDirect3DDevice9* dev, const D3DMATRIX& world, const D3DMATRIX& view,
                     const D3DMATRIX& proj) {
    if (!g_haveAppliedWorld || !Same(world, g_appliedWorld)) {
        g_origSetTransform(dev, D3DTS_WORLD, &world);
        g_appliedWorld = world;
        g_haveAppliedWorld = true;
        ++g_transformWrites;
        ++g_worldMatrixChanges;
    }
    if (!g_haveAppliedView || !Same(view, g_appliedView)) {
        g_origSetTransform(dev, D3DTS_VIEW, &view);
        g_appliedView = view;
        g_haveAppliedView = true;
        ++g_transformWrites;
    }
    if (!g_haveAppliedProj || !Same(proj, g_appliedProj)) {
        g_origSetTransform(dev, D3DTS_PROJECTION, &proj);
        g_appliedProj = proj;
        g_haveAppliedProj = true;
        ++g_transformWrites;
    }
    // Kept exact whether or not the write above actually ran: if it did not, the device
    // already held this value (the invariant this shadow depends on), so either way the
    // device now holds `proj`.
    g_curDeviceProj = proj;

    // How many DIFFERENT cameras does one frame carry? Every converted draw supplies its own
    // exact view, which is correct per draw but means Remix sees each of them. If the engine
    // renders reflections or auxiliary perspective passes, Remix has to pick between cameras
    // and may pick differently from frame to frame - which looks like flickering. Counting
    // them says whether that is happening before any fix is attempted.
    if (IsPerspective(proj)) {
        bool seen = false;
        for (unsigned i = 0; i < g_frameCameraCount; ++i)
            if (Same(view, g_frameCameras[i])) { seen = true; break; }
        if (!seen && g_frameCameraCount < kMaxFrameCameras)
            g_frameCameras[g_frameCameraCount++] = view;
    }

    if (!g_cameraCaptured && IsPerspective(proj)) {
        D3DMATRIX invView;
        if (Invert(view, invView)) {
            g_cameraView = view;
            g_camX = invView._41; g_camY = invView._42; g_camZ = invView._43;
            g_cameraCaptured = true;
            g_haveCamera = true;   // persists across frames, so lights are never orphaned
            // Log only when the projection materially changes. It was logging every frame,
            // which buried the rest of the file under 2,000 identical lines.
            if (std::fabs(proj._11 - g_loggedProj11) > 0.01f ||
                std::fabs(proj._33 - g_loggedProj33) > 1e-4f) {
                g_loggedProj11 = proj._11;
                g_loggedProj33 = proj._33;
                Log("camera: pos %.1f %.1f %.1f | proj _11=%.4f _22=%.4f _33=%.5f _43=%.2f "
                    "(aspect %.3f)",
                    g_camX, g_camY, g_camZ, proj._11, proj._22, proj._33, proj._43,
                    (std::fabs(proj._11) > 1e-6f) ? proj._22 / proj._11 : 0.0f);
            }
        }
    }
}


