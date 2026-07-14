/**
 * targetaura - Category-coloured, depth-occluded aura on the current target's 3D model (Ashita v4).
 *
 * Identifying the target's model at draw time: FFXI renders through a deferred draw-packet flush, so there
 * is NO per-actor `this` at DrawIndexedPrimitive time. BUT the current entity's actor pointer is live on the
 * call stack while its model is being drawn, at a fixed slot (~0x4EC). So each DIP checks a tight window of
 * the live stack for the target's actor pointer (IEntity::GetActorPointer); a hit means "this draw belongs
 * to the target". The mesh is then re-issued enlarged in the category colour, with the ENTIRE device state
 * saved/restored via a D3D8 state block so the game's own textured draw is untouched (no white models; no
 * vertex-buffer locking). Notorious Monsters (baked-in list) and a user Watchlist get a distinct aura.
 *
 * Self-contained: no dependency on other addons/plugins. See the memory note "ffxi-render-per-entity-gating".
 */
#ifndef TARGETAURA_HPP_INCLUDED
#define TARGETAURA_HPP_INCLUDED

#include "Ashita.h"

#include <string>
#include <vector>

class targetaura final : public IPlugin
{
    IAshitaCore*      m_AshitaCore;
    ILogManager*      m_LogManager;
    IDirect3DDevice8* m_Device;

    bool m_Enabled;
    bool m_UiOpen;

    // Effect settings.
    bool  m_RimOnly;      // true = rim/outline (front-face cull); false = full additive glow
    bool  m_Fresnel;      // layered rim-light: soft fresnel-style glow, brightest at the silhouette
    int   m_FresnelLayers;// number of stacked rim shells (higher = softer gradient)
    int   m_CullOverride; // rim cull side: 0 = auto (CCW), 1 = none, 2 = CW, 3 = CCW
    int   m_GateCenter;   // stack word where the current actor lives (~0x4EC/4 = 315); gate scan centre
    int   m_GateSpan;     // +/- words scanned around the centre (tight window kills stale-pointer bleed)
    float m_Intensity;    // colour multiplier
    float m_Scale;        // enlargement factor (aura thickness)
    bool  m_DistScale;    // grow the aura with target distance so it stays visible far away
    float m_DistBoost;    // thickness multiplier reached at the far end of the ramp (1 = no growth)
    bool  m_Pulse;
    float m_PulseSpeed;
    float m_PulseAmount;

    // Two-colour pulse: cycle the aura colour between the base (category/special) colour and a 2nd colour.
    bool  m_TwoColor;
    float m_Col2[3];
    float m_TwoColorSpeed;

    // Per-category colours [category][rgb] 0..1. Index order:
    // 0 enemy(unclaimed mob), 1 claim-us, 2 claim-other, 3 player, 4 party/self, 5 npc, 6 default.
    float m_Col[7][3];

    // Special aura: Notorious Monsters (baked-in list) and/or a user name Watchlist get their own style.
    bool  m_SpEnabled;
    bool  m_SpNM;         // auto-detect NMs via the baked-in name list
    float m_SpCol[3];
    float m_SpScale;
    bool  m_SpPulse;
    float m_SpPulseSpeed;
    float m_SpPulseAmount;
    std::vector<std::string> m_Watch;   // name substrings, stored lower-case
    char  m_WatchInput[48];             // UI "add" text buffer

    // Rotating "sweep" scan effect (fixed-function texcoord-gen; may be a no-op if the game uses shaders).
    bool  m_Sweep;
    float m_SweepSpeed;
    float m_SweepWidth;   // fraction of the stripe that is the bright band (0..1)

    // Per-frame target state (D3D world coords; y = up).
    bool     m_HasTarget;
    float    m_TgtX, m_TgtY, m_TgtZ;
    float    m_TgtYMid;      // vertical scale anchor at mid-model height (feet anchor displaces the top)
    uint32_t m_TgtColor;     // packed 0x00RRGGBB, intensity + pulse already applied
    float    m_ActiveScale;  // aura thickness used this frame (special or normal)
    bool     m_TgtSpecial;   // target is an NM / watchlist match this frame
    char     m_TgtName[48];  // current target's name (for the UI "add target" button)
    uint32_t m_FrameMeshes;  // meshes auraed this frame (accumulates)
    uint32_t m_LastMeshes;   // ... last complete frame (status readout)
    int      m_CalFrames;    // >0 while auto-calibrating the gate offset (frames remaining)

    void        WriteChat(const char* body, uint8_t bodyColor = 0x6A);
    int         ResolveCategory(uint32_t index);
    uint32_t    PackRGB(const float rgb[3], float mul) const;
    bool        MatchesWatch(const char* name) const;
    bool        IsNM(const char* name) const;
    void        AddWatch(const char* name);
    std::string SettingsPath(void) const;
    void        LoadSettings(void);
    void        SaveSettings(void);
    void        StartCalibration(void);
    void        FinishCalibration(void);
    void        DrawHalo(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices,
                         UINT startIndex, UINT primCount, uint32_t rgb);
    void        RenderUI(void);

public:
    targetaura(void);
    ~targetaura(void) override;

    const char* GetName(void) const override;
    const char* GetAuthor(void) const override;
    const char* GetDescription(void) const override;
    const char* GetLink(void) const override;
    double GetVersion(void) const override;
    double GetInterfaceVersion(void) const override;
    int32_t GetPriority(void) const override;
    uint32_t GetFlags(void) const override;

    bool Initialize(IAshitaCore* core, ILogManager* logger, uint32_t id) override;
    void Release(void) override;
    bool HandleCommand(int32_t mode, const char* command, bool injected) override;

    bool Direct3DInitialize(IDirect3DDevice8* device) override;
    void Direct3DBeginScene(bool isRenderingBackBuffer) override;
    void Direct3DEndScene(bool isRenderingBackBuffer) override;
    void Direct3DPresent(const RECT* pSourceRect, const RECT* pDestRect, HWND hDestWindowOverride, const RGNDATA* pDirtyRegion) override;
    bool Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex,
                                      UINT NumVertices, UINT startIndex, UINT primCount) override;
};

#endif
