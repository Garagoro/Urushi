#include "carfx.h"
#include "carfx_internal.h"
#include "logger.h"
#include "native_carx.h"

#include <algorithm>
#include <cstring>

namespace CarFX {
using namespace CarFX::Internal;

namespace Internal {

void ReleaseSceneCopy() {
    if (g_sceneSrv) { g_sceneSrv->Release(); g_sceneSrv = nullptr; }
    if (g_sceneTex) { g_sceneTex->Release(); g_sceneTex = nullptr; }
    memset(&g_sceneDesc, 0, sizeof(g_sceneDesc));
}

void ReleaseDepthCopy() {
    if (g_depthSrv) { g_depthSrv->Release(); g_depthSrv = nullptr; }
    if (g_depthTex) { g_depthTex->Release(); g_depthTex = nullptr; }
    memset(&g_depthDesc, 0, sizeof(g_depthDesc));
}

} // namespace Internal

namespace {

DXGI_FORMAT UntypelessColorFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return fmt;
    }
}

bool IsDepthFormatMapping(DXGI_FORMAT dsvFmt, DXGI_FORMAT* texFmt, DXGI_FORMAT* srvFmt) {
    switch (dsvFmt) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
        *texFmt = DXGI_FORMAT_R32_TYPELESS;
        *srvFmt = DXGI_FORMAT_R32_FLOAT;
        return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        *texFmt = DXGI_FORMAT_R32G8X24_TYPELESS;
        *srvFmt = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        return true;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS:
        *texFmt = DXGI_FORMAT_R24G8_TYPELESS;
        *srvFmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        return true;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS:
        *texFmt = DXGI_FORMAT_R16_TYPELESS;
        *srvFmt = DXGI_FORMAT_R16_UNORM;
        return true;
    default:
        return false;
    }
}

bool EnsureSceneCopy(ID3D11DeviceContext* c, ID3D11Resource* rtRes) {
    if (!rtRes || !g_device) return false;
    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(rtRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex))) || !srcTex) {
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    DXGI_FORMAT fmt = UntypelessColorFormat(td.Format);

    if (!g_sceneTex || g_sceneDesc.Width != td.Width || g_sceneDesc.Height != td.Height || g_sceneDesc.Format != fmt) {
        ReleaseSceneCopy();
        D3D11_TEXTURE2D_DESC dstDesc = {};
        dstDesc.Width = td.Width;
        dstDesc.Height = td.Height;
        dstDesc.MipLevels = 1;
        dstDesc.ArraySize = 1;
        dstDesc.Format = fmt;
        dstDesc.SampleDesc.Count = 1;
        dstDesc.SampleDesc.Quality = 0;
        dstDesc.Usage = D3D11_USAGE_DEFAULT;
        dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&dstDesc, nullptr, &g_sceneTex)) || !g_sceneTex) {
            srcTex->Release();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = fmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(g_sceneTex, &srvDesc, &g_sceneSrv)) || !g_sceneSrv) {
            ReleaseSceneCopy();
            srcTex->Release();
            return false;
        }
        g_sceneDesc = dstDesc;
    }

    if (td.SampleDesc.Count > 1) {
        c->ResolveSubresource(g_sceneTex, 0, srcTex, 0, fmt);
    } else if (td.MipLevels > 1 || td.ArraySize > 1) {
        c->CopySubresourceRegion(g_sceneTex, 0, 0, 0, 0, srcTex, 0, nullptr);
    } else {
        c->CopyResource(g_sceneTex, srcTex);
    }
    srcTex->Release();
    g_sceneCopiesTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool EnsureDepthCopy(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv) {
    if (!dsv || !g_device) return false;
    ID3D11Resource* dRes = nullptr;
    dsv->GetResource(&dRes);
    if (!dRes) return false;

    ID3D11Texture2D* srcTex = nullptr;
    if (FAILED(dRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&srcTex))) || !srcTex) {
        dRes->Release();
        return false;
    }
    D3D11_TEXTURE2D_DESC td = {};
    srcTex->GetDesc(&td);
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsv->GetDesc(&dsvDesc);

    DXGI_FORMAT copyTexFmt = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT copySrvFmt = DXGI_FORMAT_UNKNOWN;
    bool fmtOk = IsDepthFormatMapping(dsvDesc.Format, &copyTexFmt, &copySrvFmt) ||
                 IsDepthFormatMapping(td.Format, &copyTexFmt, &copySrvFmt);
    if (!fmtOk || td.SampleDesc.Count > 1) {
        srcTex->Release();
        dRes->Release();
        return false;
    }

    if (!g_depthTex || g_depthDesc.Width != td.Width || g_depthDesc.Height != td.Height || g_depthDesc.Format != copyTexFmt) {
        ReleaseDepthCopy();
        D3D11_TEXTURE2D_DESC dstDesc = {};
        dstDesc.Width = td.Width;
        dstDesc.Height = td.Height;
        dstDesc.MipLevels = 1;
        dstDesc.ArraySize = 1;
        dstDesc.Format = copyTexFmt;
        dstDesc.SampleDesc.Count = 1;
        dstDesc.Usage = D3D11_USAGE_DEFAULT;
        dstDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_device->CreateTexture2D(&dstDesc, nullptr, &g_depthTex)) || !g_depthTex) {
            srcTex->Release();
            dRes->Release();
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = copySrvFmt;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;
        if (FAILED(g_device->CreateShaderResourceView(g_depthTex, &srvDesc, &g_depthSrv)) || !g_depthSrv) {
            ReleaseDepthCopy();
            srcTex->Release();
            dRes->Release();
            return false;
        }
        g_depthDesc = dstDesc;
    }

    if (td.MipLevels > 1 || td.ArraySize > 1) {
        c->CopySubresourceRegion(g_depthTex, 0, 0, 0, 0, srcTex, 0, nullptr);
    } else {
        c->CopyResource(g_depthTex, srcTex);
    }
    srcTex->Release();
    dRes->Release();
    g_depthCopiesTotal.fetch_add(1, std::memory_order_relaxed);
    return true;
}

bool IsTwoChannelOrDepthFormat(DXGI_FORMAT fmt) {
    switch (fmt) {
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R8G8_SNORM:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R16G16_SNORM:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_UNORM:
        return true;
    default:
        return false;
    }
}

bool MatchTargetAndCheckRT(ID3D11DeviceContext* c, int* outTarget, uint32_t* outMask,
                           ID3D11ShaderResourceView** outAlbedoSrv,
                           ID3D11Resource** outRtRes, UINT* outRtW, UINT* outRtH, DXGI_FORMAT* outRtFmt,
                           ID3D11DepthStencilView** outDsv) {
    *outTarget = kTargetNone;
    *outMask = 0;
    *outAlbedoSrv = nullptr;

    // Render-thread cache keyed by g_gameTexGen: avoids SRWLock + 20-slot GetResource on every draw call!
    static uint64_t s_rtCachedGen = 0;
    static GameSrvSnapshot s_rtSnap[kTargetCount]{};
    static int s_rtKnownAlbSlot = -1;
    static int s_rtKnownPackSlot = -1;
    static uint32_t s_rtRediscoveryFrame = ~0u;

    const uint64_t curGen = g_gameTexGen.load(std::memory_order_acquire);
    if (curGen != s_rtCachedGen) {
        AcquireSRWLockShared(&g_gameTexLock);
        for (int i = 0; i < kTargetCount; ++i) s_rtSnap[i] = g_gameTex[i];
        s_rtCachedGen = g_gameTexGen.load(std::memory_order_relaxed);
        ReleaseSRWLockShared(&g_gameTexLock);
        s_rtRediscoveryFrame = ~0u;
    }

    auto targetHasMatchableSrv = [](const GameSrvSnapshot& gs) -> bool {
        return gs.valid && (!gs.albedoIsDefault || !gs.packedIsDefault);
    };

    bool anyGhostTex = false;
    bool needAlbSlot = !s_rtSnap[kTargetBody].albedoIsDefault && s_rtSnap[kTargetBody].valid;
    bool needPackSlot = false;
    if (s_rtSnap[kTargetBody].valid && !s_rtSnap[kTargetBody].packedIsDefault) {
        needPackSlot = true;
    }
    for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
        if (targetHasMatchableSrv(s_rtSnap[t])) {
            anyGhostTex = true;
            if (!s_rtSnap[t].packedIsDefault) needPackSlot = true;
            if (!s_rtSnap[t].albedoIsDefault) needAlbSlot = true;
        }
    }

    // internals: cached copy of the texture set, refreshed when the main thread publishes a new one
    static uint64_t s_rtIntGen = 0;
    static std::vector<ID3D11ShaderResourceView*> s_rtIntSrvs;
    static std::vector<ID3D11Resource*> s_rtIntRes;
    {
        const uint64_t ig = g_internalsGen.load(std::memory_order_acquire);
        if (ig != s_rtIntGen) {
            AcquireSRWLockShared(&g_gameTexLock);
            s_rtIntSrvs = g_internals.srvs;
            s_rtIntRes = g_internals.res;
            s_rtIntGen = g_internalsGen.load(std::memory_order_relaxed);
            ReleaseSRWLockShared(&g_gameTexLock);
        }
    }
    const bool internalsCanMatch = (!s_rtIntSrvs.empty() || !s_rtIntRes.empty()) && IsTargetActiveInternal(kTargetInternals);

    const GameSrvSnapshot& body = s_rtSnap[kTargetBody];
    const bool playerCanMatch = targetHasMatchableSrv(body);
    if (!playerCanMatch && !anyGhostTex && !internalsCanMatch) return false;

    int matchedTarget = kTargetNone;
    uint32_t mask = 0;
    ID3D11ShaderResourceView* matchedAlbedoSrv = nullptr;
    int discoveredAlbSlot = -1;
    int discoveredPackSlot = -1;

    const bool bodyNeedBoth = playerCanMatch && !body.albedoIsDefault && !body.packedIsDefault &&
                              (body.packedSrv || body.packedRes) && (body.albedoRes != body.packedRes);

    auto srvMatches = [](ID3D11ShaderResourceView* bound, ID3D11Resource** cachedRes,
                         ID3D11ShaderResourceView* targetSrv, ID3D11Resource* targetRes) -> bool {
        if (!bound) return false;
        if (targetSrv) return bound == targetSrv;
        if (!targetRes) return false;
        if (!*cachedRes) bound->GetResource(cachedRes);
        return (*cachedRes == targetRes);
    };

    const bool haveFastSlots =
        (!needAlbSlot || s_rtKnownAlbSlot >= kScanStartSlot) &&
        (!needPackSlot || s_rtKnownPackSlot >= kScanStartSlot);

    if (internalsCanMatch) {
        ID3D11ShaderResourceView* isrv[kScanSlotCount] = {};
        c->PSGetShaderResources(kScanStartSlot, kScanSlotCount, isrv);
        const bool needRes = !s_rtIntRes.empty();
        for (int i = 0; i < kScanSlotCount; ++i) {
            ID3D11ShaderResourceView* v = isrv[i];
            if (!v || matchedTarget != kTargetNone) continue;
            for (ID3D11ShaderResourceView* t : s_rtIntSrvs) {
                if (t == v) { matchedTarget = kTargetInternals; break; }
            }
            if (matchedTarget == kTargetNone && needRes) {
                ID3D11Resource* r = nullptr;
                v->GetResource(&r);
                if (r) {
                    for (ID3D11Resource* t : s_rtIntRes) {
                        if (t == r) { matchedTarget = kTargetInternals; break; }
                    }
                    r->Release();
                }
            }
        }
        for (int i = 0; i < kScanSlotCount; ++i) if (isrv[i]) isrv[i]->Release();
    }

    if (matchedTarget != kTargetNone) {
        // internals matched: nothing else to discover
    } else if (haveFastSlots) {
        ID3D11ShaderResourceView* vAlb = nullptr;
        ID3D11ShaderResourceView* vPack = nullptr;
        ID3D11Resource* resAlb = nullptr;
        ID3D11Resource* resPack = nullptr;
        if (needAlbSlot && s_rtKnownAlbSlot >= kScanStartSlot) {
            c->PSGetShaderResources(static_cast<UINT>(s_rtKnownAlbSlot), 1, &vAlb);
        }
        if (needPackSlot && s_rtKnownPackSlot >= kScanStartSlot) {
            c->PSGetShaderResources(static_cast<UINT>(s_rtKnownPackSlot), 1, &vPack);
        }

        // Check active ghost targets FIRST so isolated ghost clones never leak into player target
        if (anyGhostTex) {
            for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
                const GameSrvSnapshot& gs = s_rtSnap[t];
                if (!targetHasMatchableSrv(gs)) continue;
                bool match = false;
                if (!gs.packedIsDefault && (gs.packedSrv || gs.packedRes)) {
                    match = srvMatches(vPack, &resPack, gs.packedSrv, gs.packedRes);
                } else if (!gs.albedoIsDefault && (gs.albedoSrv || gs.albedoRes)) {
                    match = srvMatches(vAlb, &resAlb, gs.albedoSrv, gs.albedoRes);
                }
                if (match) {
                    matchedTarget = t;
                    if (!gs.packedIsDefault && s_rtKnownPackSlot >= kScanStartSlot &&
                        srvMatches(vPack, &resPack, gs.packedSrv, gs.packedRes)) {
                        mask |= (1u << s_rtKnownPackSlot);
                    }
                    if (!gs.albedoIsDefault && s_rtKnownAlbSlot >= kScanStartSlot &&
                        srvMatches(vAlb, &resAlb, gs.albedoSrv, gs.albedoRes)) {
                        mask |= (1u << s_rtKnownAlbSlot);
                        matchedAlbedoSrv = vAlb;
                        matchedAlbedoSrv->AddRef();
                    }
                    break;
                }
            }
        }

        if (matchedTarget == kTargetNone && playerCanMatch) {
            bool match = false;
            bool albMatch = !body.albedoIsDefault && (body.albedoSrv || body.albedoRes) &&
                            srvMatches(vAlb, &resAlb, body.albedoSrv, body.albedoRes);
            bool packMatch = !body.packedIsDefault && (body.packedSrv || body.packedRes) &&
                             srvMatches(vPack, &resPack, body.packedSrv, body.packedRes);
            if (!body.albedoIsDefault && (body.albedoSrv || body.albedoRes)) {
                match = albMatch && (!bodyNeedBoth || packMatch);
            } else if (!body.packedIsDefault && (body.packedSrv || body.packedRes)) {
                match = packMatch;
            }
            if (!match && !body.extraPairs.empty()) {
                for (const auto& ep : body.extraPairs) {
                    const bool epAlb = !ep.albedoIsDefault && (ep.albedoSrv || ep.albedoRes) &&
                                       srvMatches(vAlb, &resAlb, ep.albedoSrv, ep.albedoRes);
                    const bool epPack = !ep.packedIsDefault && (ep.packedSrv || ep.packedRes) &&
                                        srvMatches(vPack, &resPack, ep.packedSrv, ep.packedRes);
                    const bool epNeedBoth = !ep.albedoIsDefault && !ep.packedIsDefault &&
                                            (ep.packedSrv || ep.packedRes) && (ep.albedoRes != ep.packedRes);
                    bool epOk = false;
                    if (!ep.albedoIsDefault && (ep.albedoSrv || ep.albedoRes)) {
                        epOk = epAlb && (!epNeedBoth || epPack);
                    } else if (!ep.packedIsDefault && (ep.packedSrv || ep.packedRes)) {
                        epOk = epPack;
                    }
                    if (epOk) {
                        match = true;
                        albMatch = epAlb;
                        packMatch = epPack;
                        break;
                    }
                }
            }
            if (match) {
                matchedTarget = kTargetBody;
                if (albMatch && vAlb) {
                    mask |= (1u << s_rtKnownAlbSlot);
                    matchedAlbedoSrv = vAlb;
                    matchedAlbedoSrv->AddRef();
                }
                if (packMatch && s_rtKnownPackSlot >= kScanStartSlot) {
                    mask |= (1u << s_rtKnownPackSlot);
                }
            }
        }

        if (resAlb) resAlb->Release();
        if (resPack) resPack->Release();
        if (vAlb) vAlb->Release();
        if (vPack) vPack->Release();

        if (matchedTarget == kTargetNone) {
            const uint32_t frameId = g_frameCounter.load(std::memory_order_relaxed);
            if (g_matchedDrawsLastFrame.load(std::memory_order_relaxed) == 0 && (frameId - s_rtRediscoveryFrame) >= 60) {
                s_rtRediscoveryFrame = frameId;
                s_rtKnownAlbSlot = -1;
                s_rtKnownPackSlot = -1;
            }
            return false;
        }
    } else {
        ID3D11ShaderResourceView* srvs[kScanSlotCount] = {};
        c->PSGetShaderResources(kScanStartSlot, kScanSlotCount, srvs);
        ID3D11Resource* cachedRes[kScanSlotCount] = {};

        auto scanMatchForTarget = [&](int t, bool isGhost, bool requireBoth) -> bool {
            const GameSrvSnapshot& gs = s_rtSnap[t];
            if (!targetHasMatchableSrv(gs)) return false;

            ID3D11ShaderResourceView* foundAlb = nullptr;
            int foundAlbIdx = -1;
            int foundPackIdx = -1;

            for (int i = 0; i < kScanSlotCount; ++i) {
                ID3D11ShaderResourceView* v = srvs[i];
                if (!v) continue;

                const bool isAlb = !gs.albedoIsDefault && srvMatches(v, &cachedRes[i], gs.albedoSrv, gs.albedoRes);
                const bool isPack = !gs.packedIsDefault && srvMatches(v, &cachedRes[i], gs.packedSrv, gs.packedRes);
                if (isAlb && !foundAlb) {
                    foundAlb = v;
                    foundAlbIdx = kScanStartSlot + i;
                }
                if (isPack && foundPackIdx < 0) {
                    foundPackIdx = kScanStartSlot + i;
                }
            }

            bool ok = false;
            if (isGhost) {
                if (!gs.packedIsDefault) ok = (foundPackIdx >= 0);
                else if (!gs.albedoIsDefault) ok = (foundAlb != nullptr);
            } else {
                ok = requireBoth ? (foundAlb != nullptr && foundPackIdx >= 0)
                                 : (!gs.albedoIsDefault ? (foundAlb != nullptr) : (foundPackIdx >= 0));
            }
            if (!ok && !isGhost && !gs.extraPairs.empty()) {
                for (const auto& ep : gs.extraPairs) {
                    ID3D11ShaderResourceView* epAlb = nullptr;
                    int epAlbIdx = -1;
                    int epPackIdx = -1;
                    for (int i = 0; i < kScanSlotCount; ++i) {
                        ID3D11ShaderResourceView* v = srvs[i];
                        if (!v) continue;
                        if (!ep.albedoIsDefault && srvMatches(v, &cachedRes[i], ep.albedoSrv, ep.albedoRes) && !epAlb) {
                            epAlb = v;
                            epAlbIdx = kScanStartSlot + i;
                        }
                        if (!ep.packedIsDefault && srvMatches(v, &cachedRes[i], ep.packedSrv, ep.packedRes) && epPackIdx < 0) {
                            epPackIdx = kScanStartSlot + i;
                        }
                    }
                    const bool epNeedBoth = !ep.albedoIsDefault && !ep.packedIsDefault &&
                                            (ep.packedSrv || ep.packedRes) && (ep.albedoRes != ep.packedRes);
                    if (epNeedBoth ? (epAlb != nullptr && epPackIdx >= 0)
                                   : (!ep.albedoIsDefault ? (epAlb != nullptr) : (epPackIdx >= 0))) {
                        ok = true;
                        foundAlb = epAlb;
                        foundAlbIdx = epAlbIdx;
                        foundPackIdx = epPackIdx;
                        break;
                    }
                }
            }
            if (!ok) return false;

            matchedTarget = t;
            if (foundAlbIdx >= 0) {
                discoveredAlbSlot = foundAlbIdx;
                mask |= (1u << foundAlbIdx);
                matchedAlbedoSrv = foundAlb;
                matchedAlbedoSrv->AddRef();
            }
            if (foundPackIdx >= 0) {
                discoveredPackSlot = foundPackIdx;
                mask |= (1u << foundPackIdx);
            }
            return true;
        };

        if (anyGhostTex) {
            for (int t = kTargetGhostBase; t < kTargetCount; ++t) {
                if (scanMatchForTarget(t, true, false)) break;
            }
        }
        if (matchedTarget == kTargetNone && playerCanMatch) {
            scanMatchForTarget(kTargetBody, false, bodyNeedBoth);
        }

        for (int i = 0; i < kScanSlotCount; ++i) {
            if (cachedRes[i]) cachedRes[i]->Release();
            if (srvs[i]) srvs[i]->Release();
        }
        if (matchedTarget == kTargetNone) {
            return false;
        }
    }

    // Verify main color RTV (rejects DepthOnly, ShadowCaster, NORMALS_BUFFER, tiny reflection cubemaps)
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11DepthStencilView* dsv = nullptr;
    c->OMGetRenderTargets(1, &rtv, &dsv);
    if (!rtv) {
        if (dsv) dsv->Release();
        if (matchedAlbedoSrv) matchedAlbedoSrv->Release();
        return false;
    }

    ID3D11Resource* rtRes = nullptr;
    rtv->GetResource(&rtRes);
    rtv->Release();

    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFmt = DXGI_FORMAT_UNKNOWN;
    if (rtRes) {
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(rtRes->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex))) && tex) {
            D3D11_TEXTURE2D_DESC td = {};
            tex->GetDesc(&td);
            rtW = td.Width;
            rtH = td.Height;
            rtFmt = td.Format;
            tex->Release();
        }
    }

    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    c->RSGetViewports(&nvp, &vp);
    bool validRt = rtRes && rtW > 0 && rtH > 0 && !IsTwoChannelOrDepthFormat(rtFmt);
    if (validRt && nvp > 0 && vp.Width > 64.0f && vp.Height > 64.0f) {
        if (rtW < static_cast<UINT>(vp.Width * 0.5f) || rtH < static_cast<UINT>(vp.Height * 0.5f)) {
            validRt = false;
        }
    }

    if (!validRt) {
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        if (matchedAlbedoSrv) matchedAlbedoSrv->Release();
        return false;
    }

    // Latch discovered slot indices only after confirming main color RT!
    if (discoveredAlbSlot >= kScanStartSlot) {
        s_rtKnownAlbSlot = discoveredAlbSlot;
    }
    if (discoveredPackSlot >= kScanStartSlot) {
        s_rtKnownPackSlot = discoveredPackSlot;
    }

    *outTarget = matchedTarget;
    *outMask = mask;
    *outAlbedoSrv = matchedAlbedoSrv;   // caller owns ref
    *outRtRes = rtRes;                  // caller owns ref
    *outRtW = rtW;
    *outRtH = rtH;
    *outRtFmt = rtFmt;
    *outDsv = dsv;                      // caller owns ref
    return true;
}

void RestoreState(ID3D11DeviceContext* c, BoundState& b) {
    if (!b.active) return;
    c->PSSetShader(b.ps, nullptr, 0);
    c->PSSetConstantBuffers(kCbSlot, 1, &b.cb);
    c->PSSetShaderResources(kSceneSlot, kViewCount, b.srvs);
    c->PSSetSamplers(14, 2, b.samplers);
    if (b.restoreOm) {
        c->OMSetBlendState(b.blend, b.blendFactor, b.sampleMask);
        c->OMSetDepthStencilState(b.ds, b.stencilRef);
    }

    if (b.ps) b.ps->Release();
    if (b.cb) b.cb->Release();
    for (ID3D11ShaderResourceView*& v : b.srvs) {
        if (v) v->Release();
    }
    for (ID3D11SamplerState*& s : b.samplers) {
        if (s) s->Release();
    }
    if (b.blend) b.blend->Release();
    if (b.ds) b.ds->Release();
    b = {};
}

} // namespace

// ================= Draw Interception & Diagnostics =================

bool TryHandleDraw(ID3D11DeviceContext* context, UINT /*countPerInstance*/, DrawIssueFn issueFn, void* issueCtx) {
    if (!context || !issueFn || !g_initialized.load(std::memory_order_acquire)) return false;
    if (!NativeCarX::IsInSessionFast()) return false;
    if (context->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;

    g_totalDrawsFrame.fetch_add(1, std::memory_order_relaxed);

    const bool playerWant = IsTargetActiveInternal(kTargetBody);
    const bool anyGhostWant = (g_activeGhostShaderMask.load(std::memory_order_acquire) != 0);

    // Run detection when player or any ghost has an active shader, or every 30 frames for diagnostics
    const uint32_t frameId = g_frameCounter.load(std::memory_order_relaxed);
    const bool runDiagOnly = (!playerWant && !anyGhostWant) && ((frameId % 30) != 0);
    if (runDiagOnly) return false;

    int matchedTarget = kTargetNone;
    uint32_t slotMask = 0;
    ID3D11ShaderResourceView* albedoSrv = nullptr;
    ID3D11Resource* rtRes = nullptr;
    UINT rtW = 0, rtH = 0;
    DXGI_FORMAT rtFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11DepthStencilView* dsv = nullptr;

    if (!MatchTargetAndCheckRT(context, &matchedTarget, &slotMask, &albedoSrv,
                               &rtRes, &rtW, &rtH, &rtFmt, &dsv)) {
        const uintptr_t knownPs = g_lastGamePs.load(std::memory_order_relaxed);
        static uint32_t s_lastGhostMissDiagFrame = ~0u;
        if (anyGhostWant && knownPs && g_targetMatchedLastFrame[kTargetGhostBase].load(std::memory_order_relaxed) == 0 &&
            (frameId - s_lastGhostMissDiagFrame) >= 180) {
            ID3D11PixelShader* ps = nullptr;
            context->PSGetShader(&ps, nullptr, nullptr);
            if (ps) ps->Release();
            if (reinterpret_cast<uintptr_t>(ps) == knownPs) {
                ID3D11RenderTargetView* rtv = nullptr;
                context->OMGetRenderTargets(1, &rtv, nullptr);
                if (rtv) {
                    ID3D11Resource* rtR = nullptr;
                    rtv->GetResource(&rtR);
                    rtv->Release();
                    UINT w = 0, h = 0; DXGI_FORMAT f = DXGI_FORMAT_UNKNOWN;
                    if (rtR) {
                        ID3D11Texture2D* tx = nullptr;
                        if (SUCCEEDED(rtR->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tx))) && tx) {
                            D3D11_TEXTURE2D_DESC td{}; tx->GetDesc(&td);
                            w = td.Width; h = td.Height; f = td.Format;
                            tx->Release();
                        }
                        rtR->Release();
                    }
                    if (w >= 640 && !IsTwoChannelOrDepthFormat(f)) {
                        s_lastGhostMissDiagFrame = frameId;
                        ID3D11ShaderResourceView* srvs[kScanSlotCount] = {};
                        context->PSGetShaderResources(kScanStartSlot, kScanSlotCount, srvs);
                        ID3D11Resource* gPackRes = nullptr;
                        ID3D11ShaderResourceView* gPackSrv = nullptr;
                        {
                            AcquireSRWLockShared(&g_gameTexLock);
                            gPackRes = g_gameTex[kTargetGhostBase].packedRes;
                            gPackSrv = g_gameTex[kTargetGhostBase].packedSrv;
                            ReleaseSRWLockShared(&g_gameTexLock);
                        }
                        char info[384] = {};
                        int off = _snprintf_s(info, sizeof(info), _TRUNCATE,
                            "Ghost miss-diag PS=%p RT=%ux%u(fmt=%u) gPack=(srv=%p,res=%p) bound:",
                            ps, w, h, (unsigned)f, gPackSrv, gPackRes);
                        for (int i = 0; i < kScanSlotCount; ++i) {
                            if (!srvs[i]) continue;
                            ID3D11Resource* r = nullptr;
                            srvs[i]->GetResource(&r);
                            if (off > 0 && off < (int)sizeof(info) - 32) {
                                off += _snprintf_s(info + off, sizeof(info) - off, _TRUNCATE,
                                    " t%d=%p/%p", kScanStartSlot + i, srvs[i], r);
                            }
                            if (r) r->Release();
                            srvs[i]->Release();
                        }
                        Logger::Info(info);
                    }
                }
            }
        }
        return false;
    }

    if (ValidTarget(matchedTarget)) {
        g_targetMatchedFrame[matchedTarget].fetch_add(1, std::memory_order_relaxed);
    }
    // Record telemetry for player diagnostics (and first-time ghost verification)
    if (matchedTarget == kTargetBody) {
        g_matchedDrawsFrame.fetch_add(1, std::memory_order_relaxed);
        g_lastSlotMask.store(slotMask, std::memory_order_relaxed);
        g_lastRtWidth.store(rtW, std::memory_order_relaxed);
        g_lastRtHeight.store(rtH, std::memory_order_relaxed);
        g_lastRtFormat.store(static_cast<uint32_t>(rtFmt), std::memory_order_relaxed);
    }

    ID3D11PixelShader* origPs = nullptr;
    context->PSGetShader(&origPs, nullptr, nullptr);
    if (matchedTarget == kTargetBody) {
        g_lastGamePs.store(reinterpret_cast<uintptr_t>(origPs), std::memory_order_relaxed);
    }

    static bool s_loggedPlayerDrawOnce = false;
    static bool s_loggedGhostDrawOnce = false;
    if (matchedTarget == kTargetBody && !s_loggedPlayerDrawOnce) {
        s_loggedPlayerDrawOnce = true;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "CarFX FIRST MATCHED PLAYER CAR DRAW: target=%d slots=0x%X rt=%ux%u fmt=%u origPs=%p",
                    matchedTarget, slotMask, rtW, rtH, static_cast<unsigned>(rtFmt), origPs);
        Logger::Info(buf);
    } else if (IsGhostTarget(matchedTarget) && !s_loggedGhostDrawOnce) {
        s_loggedGhostDrawOnce = true;
        char buf[256];
        _snprintf_s(buf, sizeof(buf), _TRUNCATE,
                    "CarFX FIRST MATCHED GHOST CAR DRAW: target=%d slots=0x%X rt=%ux%u fmt=%u origPs=%p",
                    matchedTarget, slotMask, rtW, rtH, static_cast<unsigned>(rtFmt), origPs);
        Logger::Info(buf);
    }

    TargetSlotState& ts = g_targets[matchedTarget];
    ID3D11PixelShader* customPs = ts.ps.load(std::memory_order_acquire);
    if (!IsTargetActiveInternal(matchedTarget) || !customPs || !g_cb) {
        if (origPs) origPs->Release();
        if (albedoSrv) albedoSrv->Release();
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        return false;
    }

    int ov = ts.overrideMode.load(std::memory_order_acquire);
    int mode = (ov != kModeAuto) ? ov : ts.declMode.load(std::memory_order_acquire);
    if (mode != kDeclModeOverlay) mode = kDeclModeReplace;

    // 1. Copy scene / depth ONCE per frame right before the first body part is overwritten (replace)
    //    or if overlay shader samples background/depth!
    const bool needScene = ts.usesScene.load(std::memory_order_acquire);
    const bool needDepth = ts.usesDepth.load(std::memory_order_acquire);
    if (needScene && g_sceneCopiedFrame != frameId) {
        if (EnsureSceneCopy(context, rtRes)) g_sceneCopiedFrame = frameId;
    }
    if (needDepth && g_depthCopiedFrame != frameId && dsv) {
        if (EnsureDepthCopy(context, dsv)) g_depthCopiedFrame = frameId;
    }

    // 2. Update CarFx constant buffer b13
    D3D11_VIEWPORT vp = {};
    UINT nvp = 1;
    context->RSGetViewports(&nvp, &vp);

    LARGE_INTEGER nowQpc{};
    QueryPerformanceCounter(&nowQpc);
    const int64_t prevQpc = g_lastDrawQpc.exchange(nowQpc.QuadPart, std::memory_order_relaxed);
    const double freq = g_qpcFreq.QuadPart ? static_cast<double>(g_qpcFreq.QuadPart) : 1.0;

    const int dataTarget = (matchedTarget == kTargetInternals) ? kTargetBody : matchedTarget;   // telemetry / spatial data come from the player car
    FxConstants k{};
    k.time[0] = static_cast<float>((nowQpc.QuadPart - ts.loadQpc.QuadPart) / freq);
    k.time[1] = prevQpc ? static_cast<float>((nowQpc.QuadPart - prevQpc) / freq) : 0.0f;
    k.time[2] = static_cast<float>(g_matchedDrawsFrame.load(std::memory_order_relaxed));
    k.time[3] = static_cast<float>(matchedTarget);

    k.screen[0] = (nvp && vp.Width > 0.0f) ? vp.Width : static_cast<float>(rtW);
    k.screen[1] = (nvp && vp.Height > 0.0f) ? vp.Height : static_cast<float>(rtH);
    k.screen[2] = rtW > 0 ? 1.0f / static_cast<float>(rtW) : 0.0f;
    k.screen[3] = rtH > 0 ? 1.0f / static_cast<float>(rtH) : 0.0f;

    for (int p = 0; p < kParams; ++p) {
        for (int c = 0; c < 4; ++c) {
            k.param[p][c] = ts.param[p][c].load(std::memory_order_relaxed);
        }
    }

    ID3D11ShaderResourceView* tex0 = ts.userTex[0].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex1 = ts.userTex[1].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex2 = ts.userTex[2].load(std::memory_order_acquire);
    ID3D11ShaderResourceView* tex3 = ts.userTex[3].load(std::memory_order_acquire);

    k.frameInfo[0] = albedoSrv ? 1.0f : 0.0f;
    k.frameInfo[1] = tex0 ? 1.0f : 0.0f;
    k.frameInfo[2] = tex1 ? 1.0f : 0.0f;
    k.frameInfo[3] = static_cast<float>(mode);

    for (int i = 0; i < kGameInputs; ++i) {
        k.game[i / 4][i % 4] = g_gameInputs[dataTarget][i].load(std::memory_order_relaxed);
    }

    {
        AcquireSRWLockShared(&g_spatialLock[dataTarget]);
        const SpatialProximityInput sp = g_spatialInput[dataTarget];
        ReleaseSRWLockShared(&g_spatialLock[dataTarget]);
        memcpy(k.carPos, sp.carPos, sizeof(k.carPos));
        memcpy(k.carRight, sp.carRight, sizeof(k.carRight));
        memcpy(k.carUp, sp.carUp, sizeof(k.carUp));
        memcpy(k.carFwd, sp.carFwd, sizeof(k.carFwd));
        memcpy(k.camPos, sp.camPos, sizeof(k.camPos));
        memcpy(k.nearCarPos, sp.nearCarPos, sizeof(k.nearCarPos));
        memcpy(k.nearCarRight, sp.nearCarRight, sizeof(k.nearCarRight));
        memcpy(k.nearCarFwd, sp.nearCarFwd, sizeof(k.nearCarFwd));
        memcpy(k.nearCarClosest, sp.nearCarClosest, sizeof(k.nearCarClosest));
        memcpy(k.wallDirDist, sp.wallDirDist, sizeof(k.wallDirDist));
        memcpy(k.wallNearestHit, sp.wallNearestHit, sizeof(k.wallNearestHit));
        memcpy(k.wallNearestNormal, sp.wallNearestNormal, sizeof(k.wallNearestNormal));
        memcpy(k.wallFrontHit, sp.wallFrontHit, sizeof(k.wallFrontHit));
        memcpy(k.wallRearHit, sp.wallRearHit, sizeof(k.wallRearHit));
        memcpy(k.wallLeftHit, sp.wallLeftHit, sizeof(k.wallLeftHit));
        memcpy(k.wallRightHit, sp.wallRightHit, sizeof(k.wallRightHit));
        memcpy(k.wallDoorLeftHit, sp.wallDoorLeftHit, sizeof(k.wallDoorLeftHit));
        memcpy(k.wallDoorRightHit, sp.wallDoorRightHit, sizeof(k.wallDoorRightHit));
        memcpy(k.wallFrontLeftHit, sp.wallFrontLeftHit, sizeof(k.wallFrontLeftHit));
        memcpy(k.wallFrontRightHit, sp.wallFrontRightHit, sizeof(k.wallFrontRightHit));
        memcpy(k.wallRearLeftHit, sp.wallRearLeftHit, sizeof(k.wallRearLeftHit));
        memcpy(k.wallRearRightHit, sp.wallRearRightHit, sizeof(k.wallRearRightHit));
        memcpy(k.driftState, sp.driftState, sizeof(k.driftState));
        memcpy(k.camRight, sp.camRight, sizeof(k.camRight));
        memcpy(k.camUp, sp.camUp, sizeof(k.camUp));
        memcpy(k.camFwd, sp.camFwd, sizeof(k.camFwd));
    }

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(g_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData) {
        if (origPs) origPs->Release();
        if (albedoSrv) albedoSrv->Release();
        if (rtRes) rtRes->Release();
        if (dsv) dsv->Release();
        return false;
    }
    memcpy(mapped.pData, &k, sizeof(k));
    context->Unmap(g_cb, 0);

    // 3. In overlay mode, let the game draw base paint + baked livery + lighting FIRST!
    if (mode == kDeclModeOverlay) {
        issueFn(issueCtx);
    }

    // 4. Save game state, bind CarFX PS/CB/SRVs (and Blend/DS for overlay), draw, restore!
    BoundState saved{};
    saved.active = true;
    saved.ps = origPs;   // ownership transferred to saved
    context->PSGetConstantBuffers(kCbSlot, 1, &saved.cb);
    context->PSGetShaderResources(kSceneSlot, kViewCount, saved.srvs);
    context->PSGetSamplers(14, 2, saved.samplers);
    const bool depthWrite = (mode != kDeclModeOverlay) && g_stealthDepthWrite.load(std::memory_order_relaxed) &&
                            g_bodyDepthDs && matchedTarget == kTargetBody && ShouldHideCarInternals(matchedTarget);
    if (mode == kDeclModeOverlay || depthWrite) {
        saved.restoreOm = true;
        context->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
        context->OMGetDepthStencilState(&saved.ds, &saved.stencilRef);
    }

    ID3D11ShaderResourceView* views[kViewCount] = {
        needScene ? g_sceneSrv : nullptr,   // t32: g_fxScene
        tex0,                               // t33: g_fxTex0
        tex1,                               // t34: g_fxTex1
        tex2,                               // t35: g_fxTex2
        tex3,                               // t36: g_fxTex3
        needDepth ? g_depthSrv : nullptr,   // t37: g_fxDepth
        albedoSrv                           // t38: g_fxLivery
    };
    ID3D11SamplerState* samplers[2] = { g_samplerLinear, g_samplerPoint };

    context->PSSetShader(customPs, nullptr, 0);
    context->PSSetConstantBuffers(kCbSlot, 1, &g_cb);
    context->PSSetShaderResources(kSceneSlot, kViewCount, views);
    context->PSSetSamplers(14, 2, samplers);
    if (depthWrite) {
        context->OMSetDepthStencilState(g_bodyDepthDs, saved.stencilRef);
    }
    if (mode == kDeclModeOverlay) {
        if (g_overlayBlend) context->OMSetBlendState(g_overlayBlend, nullptr, 0xFFFFFFFFu);
        if (g_overlayDs) context->OMSetDepthStencilState(g_overlayDs, saved.stencilRef);
    }

    issueFn(issueCtx);

    RestoreState(context, saved);

    if (albedoSrv) albedoSrv->Release();
    if (rtRes) rtRes->Release();
    if (dsv) dsv->Release();
    return true;
}

DiagnosticStatus GetStatus(int target) {
    DiagnosticStatus st{};
    st.initialized = g_initialized.load(std::memory_order_acquire);
    st.enabled = IsGhostTarget(target)
        ? (ValidTarget(target) && g_targets[target].slotEnabled.load(std::memory_order_acquire))
        : g_enabled.load(std::memory_order_acquire);
    st.hookedDraw = g_hookedDraw.load(std::memory_order_acquire);

    const int tIdx = ValidTarget(target) ? target : kTargetBody;
    st.totalDrawsFrame = g_totalDrawsLastFrame.load(std::memory_order_relaxed);
    st.matchedDrawsFrame = g_targetMatchedFrame[tIdx].load(std::memory_order_relaxed);
    st.matchedDrawsLastFrame = g_targetMatchedLastFrame[tIdx].load(std::memory_order_relaxed);
    st.sceneCopiesTotal = g_sceneCopiesTotal.load(std::memory_order_relaxed);
    st.depthCopiesTotal = g_depthCopiesTotal.load(std::memory_order_relaxed);
    st.lastSlotMask = g_lastSlotMask.load(std::memory_order_relaxed);
    st.lastRtWidth = g_lastRtWidth.load(std::memory_order_relaxed);
    st.lastRtHeight = g_lastRtHeight.load(std::memory_order_relaxed);
    st.lastRtFormat = g_lastRtFormat.load(std::memory_order_relaxed);
    st.lastGamePs = g_lastGamePs.load(std::memory_order_relaxed);

    const int texIdx = ValidTarget(target) ? target : kTargetBody;
    AcquireSRWLockShared(&g_gameTexLock);
    const GameSrvSnapshot& b = g_gameTex[texIdx];
    st.bodyTargetsValid = b.valid;
    st.bodyAlbedoIsDefault = b.albedoIsDefault;
    st.bodyPackedIsDefault = b.packedIsDefault;
    st.bodyAlbedoTexId = b.albedoTexId;
    st.bodyPackedTexId = b.packedTexId;
    st.bodyAlbedoSrv = reinterpret_cast<uintptr_t>(b.albedoSrv);
    st.bodyPackedSrv = reinterpret_cast<uintptr_t>(b.packedSrv);
    ReleaseSRWLockShared(&g_gameTexLock);

    if (ValidTarget(target)) {
        TargetSlotState& ts = g_targets[target];
        std::lock_guard<std::mutex> lk(ts.publishMtx);
        st.compiled = (ts.ps.load(std::memory_order_relaxed) != nullptr);
        st.activeMode = ts.overrideMode.load(std::memory_order_relaxed);
        int eff = st.activeMode != kModeAuto ? st.activeMode : ts.declMode.load(std::memory_order_relaxed);
        st.resolvedMode = eff;
        st.activeStealth = ts.overrideStealth.load(std::memory_order_relaxed);
        st.declStealth = ts.declStealth.load(std::memory_order_relaxed);
        st.resolvedStealth = (st.activeStealth == kStealthOn) || (st.activeStealth == kStealthAuto && st.declStealth);
        st.usesScene = ts.usesScene.load(std::memory_order_relaxed);
        st.usesDepth = ts.usesDepth.load(std::memory_order_relaxed);
        st.usesLivery = ts.usesLivery.load(std::memory_order_relaxed);
        st.compilesTotal = ts.compiles;
        st.compileFailures = ts.failures;
        st.shaderPath = ts.shaderPath;
        st.lastError = ts.lastError;
        st.decl = ts.decl;
    }

    return st;
}

} // namespace CarFX
