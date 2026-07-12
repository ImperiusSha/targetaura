/**
 * targetaura - see targetaura.hpp for the design.
 */
#include "targetaura.hpp"
#include "nmnames.h"   // baked-in Notorious Monster name list (self-contained; no addon dependency)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

#define TARGETAURA_VERSION 1.0

// Halo save/restore: 0 = lean recorded state block (only the states DrawHalo touches); 1 = full
// D3DSBT_ALL (heavier but maximally safe). Flip to 1 if the lean path ever shows texture artifacts.
#define TARGETAURA_HALO_FULL_STATEBLOCK 0

// Category indices into m_Col.
enum { CAT_ENEMY = 0, CAT_CLAIM_US, CAT_CLAIM_OTHER, CAT_PLAYER, CAT_PARTY, CAT_NPC, CAT_DEFAULT };

// ---------------------------------------------------------------------------------------------------
// Draw-time gate. FFXI's draws are deferred (no per-actor `this` at DIP time), but the current entity's
// actor pointer is live on the call stack (fixed slot ~0x4EC) while its model is drawn. Checking a tight
// window there for a specific actor pointer is a false-positive-free per-target gate.
// ---------------------------------------------------------------------------------------------------
namespace
{
    const int SCAN_WORDS = 2048;                 // clamp for the stack-window scan

    volatile uintptr_t g_targetActor = 0;        // current target's actor pointer (set each frame)
    volatile bool      g_inHalo      = false;    // re-entry guard: DrawHalo re-issues a DIP
    DWORD              g_sbToken      = 0;        // reusable state block for DrawHalo save/restore
    bool               g_sbReady      = false;

    // Build the state block DrawHalo uses to save/restore device state around its extra draw. The lean
    // build RECORDS exactly the states DrawHalo modifies, so CaptureStateBlock/ApplyStateBlock only touch
    // those ~17 (world transform, 11 render states, texture 0, 4 stage-0 texture-stage states) instead of
    // the whole device -- much cheaper per mesh, and still a complete restore (no white-model leak).
    static bool build_halo_block(IDirect3DDevice8* dev)
    {
#if TARGETAURA_HALO_FULL_STATEBLOCK
        return dev->CreateStateBlock(D3DSBT_ALL, &g_sbToken) == D3D_OK;
#else
        if (dev->BeginStateBlock() != D3D_OK)
            return false;
        D3DMATRIX id;
        memset(&id, 0, sizeof(id));
        id._11 = id._22 = id._33 = id._44 = 1.0f;
        dev->SetTransform(D3DTS_WORLD, &id);
        dev->SetRenderState(D3DRS_ZENABLE,          TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        dev->SetRenderState(D3DRS_ZFUNC,            D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        dev->SetRenderState(D3DRS_CULLMODE,         D3DCULL_NONE);
        dev->SetRenderState(D3DRS_TEXTUREFACTOR,    0xFFFFFFFFu);
        dev->SetTexture(0, nullptr);
        dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);
        dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        dev->SetTransform(D3DTS_TEXTURE0, &id);
        return dev->EndStateBlock(&g_sbToken) == D3D_OK;
#endif
    }

    static bool stack_has_at(uintptr_t esp, uintptr_t val, int lo, int hi)
    {
        if (val == 0)
            return false;
        if (lo < 0)          lo = 0;
        if (hi > SCAN_WORDS)  hi = SCAN_WORDS;
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(esp);
        __try
        {
            for (int i = lo; i < hi; ++i)
                if (sp[i] == val)
                    return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
        return false;
    }

    // Calibration: count, per stack word, how often the target's actor pointer appears there. Over a few
    // frames the true "current actor" slot (present on every model mesh) wins by count.
    uint16_t g_calHist[SCAN_WORDS] = { 0 };

    static void cal_scan(uintptr_t esp)
    {
        const uintptr_t tgt = g_targetActor;
        if (tgt == 0)
            return;
        const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(esp);
        __try
        {
            for (int i = 0; i < SCAN_WORDS; ++i)
                if (sp[i] == tgt && g_calHist[i] < 0xFFFF)
                    g_calHist[i]++;
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // A small grey stripe texture (soft bright band, rest mid-grey) for the sweep effect. It MODULATES
    // the aura colour, so the band brightens the aura as it scrolls. Our own texture -> safe to lock.
    IDirect3DTexture8* g_stripeTex = nullptr;

    static void build_stripe(IDirect3DDevice8* dev)
    {
        if (g_stripeTex != nullptr || dev == nullptr)
            return;
        const UINT W = 64, H = 4;
        IDirect3DTexture8* tex = nullptr;
        if (dev->CreateTexture(W, H, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex) != D3D_OK || tex == nullptr)
            return;
        D3DLOCKED_RECT lr;
        if (tex->LockRect(0, &lr, nullptr, 0) == D3D_OK)
        {
            for (UINT y = 0; y < H; ++y)
            {
                uint32_t* row = reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
                for (UINT x = 0; x < W; ++x)
                {
                    const float u = static_cast<float>(x) / static_cast<float>(W - 1);
                    float d = u - 0.5f; if (d < 0.0f) d = -d;
                    float band = 1.0f - d / 0.16f; if (band < 0.0f) band = 0.0f; band *= band;
                    const float b = 0.45f + 0.55f * band;                 // mid-grey base, bright band
                    const uint32_t c = static_cast<uint32_t>(b * 255.0f + 0.5f);
                    row[x] = (0xFFu << 24) | (c << 16) | (c << 8) | c;
                }
            }
            tex->UnlockRect(0);
        }
        g_stripeTex = tex;
    }
}

// ---------------------------------------------------------------------------------------------------
// Construction / info
// ---------------------------------------------------------------------------------------------------
targetaura::targetaura(void)
    : m_AshitaCore(nullptr)
    , m_LogManager(nullptr)
    , m_Device(nullptr)
    , m_Enabled(true)
    , m_UiOpen(false)
    , m_RimOnly(false)         // release defaults from the user's tuned config
    , m_Fresnel(true)
    , m_FresnelLayers(8)
    , m_CullOverride(1)
    , m_GateCenter(310)
    , m_GateSpan(48)
    , m_Intensity(0.30f)
    , m_Scale(1.020f)
    , m_Pulse(true)
    , m_PulseSpeed(3.0f)
    , m_PulseAmount(1.00f)
    , m_TwoColor(false)
    , m_TwoColorSpeed(2.0f)
    , m_SpEnabled(true)
    , m_SpNM(true)
    , m_SpScale(1.01f)
    , m_SpPulse(true)
    , m_SpPulseSpeed(5.0f)
    , m_SpPulseAmount(0.50f)
    , m_Sweep(true)
    , m_SweepSpeed(1.0f)
    , m_SweepWidth(0.15f)
    , m_HasTarget(false)
    , m_TgtX(0.0f), m_TgtY(0.0f), m_TgtZ(0.0f)
    , m_TgtColor(0x00C02020)
    , m_ActiveScale(1.020f)
    , m_TgtSpecial(false)
    , m_FrameMeshes(0)
    , m_LastMeshes(0)
    , m_CalFrames(0)
{
    const float spc[3] = { 1.00f, 0.743f, 0.15f }; // special / NM aura - bright gold
    memcpy(this->m_SpCol, spc, sizeof(spc));
    const float c2[3] = { 1.00f, 1.00f, 1.00f };  // two-colour pulse: 2nd colour - white
    memcpy(this->m_Col2, c2, sizeof(c2));
    this->m_WatchInput[0] = '\0';
    this->m_TgtName[0]    = '\0';

    const float defc[7][3] = {
        { 0.85f, 0.65f, 0.10f }, // enemy - yellow
        { 0.90f, 0.15f, 0.15f }, // claim-us - red
        { 0.65f, 0.25f, 0.95f }, // claim-other - purple
        { 0.85f, 0.85f, 0.85f }, // player - white
        { 0.25f, 0.55f, 1.00f }, // party/self - blue
        { 0.25f, 0.85f, 0.35f }, // npc - green
        { 0.55f, 0.55f, 0.55f }, // default - grey
    };
    memcpy(this->m_Col, defc, sizeof(defc));
}

targetaura::~targetaura(void) {}

const char* targetaura::GetName(void) const        { return "targetaura"; }
const char* targetaura::GetAuthor(void) const      { return "Imperius"; }
const char* targetaura::GetDescription(void) const { return "Category-coloured aura on the current target's model."; }
const char* targetaura::GetLink(void) const        { return ""; }
double      targetaura::GetVersion(void) const     { return TARGETAURA_VERSION; }
double      targetaura::GetInterfaceVersion(void) const { return ASHITA_INTERFACE_VERSION; }
int32_t     targetaura::GetPriority(void) const    { return 0; }

uint32_t targetaura::GetFlags(void) const
{
    return static_cast<uint32_t>(Ashita::PluginFlags::UseCommands) |
           static_cast<uint32_t>(Ashita::PluginFlags::UseDirect3D);
}

bool targetaura::Initialize(IAshitaCore* core, ILogManager* logger, const uint32_t id)
{
    UNREFERENCED_PARAMETER(id);
    this->m_AshitaCore = core;
    this->m_LogManager = logger;
    this->LoadSettings();
    return true;
}

void targetaura::Release(void)
{
    this->SaveSettings();

    if (this->m_Device != nullptr && g_sbReady)
    {
        __try { this->m_Device->DeleteStateBlock(g_sbToken); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    if (g_stripeTex != nullptr)
    {
        __try { g_stripeTex->Release(); }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_stripeTex = nullptr;
    }
    g_sbReady     = false;
    g_targetActor = 0;
    if (this->m_LogManager != nullptr)
        this->m_LogManager->Log(static_cast<uint32_t>(Ashita::LogLevel::Info), "targetaura", "Unloaded.");
}

// ---------------------------------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------------------------------
void targetaura::WriteChat(const char* body, uint8_t bodyColor)
{
    if (this->m_AshitaCore == nullptr || this->m_AshitaCore->GetChatManager() == nullptr || body == nullptr)
        return;
    char buffer[256];
    _snprintf_s(buffer, sizeof(buffer), _TRUNCATE,
        "\x1E\x51" "[" "\x1E\x06" "targetaura" "\x1E\x51" "]" "\x1E\x01" " " "\x1E%c" "%s" "\x1E\x01", bodyColor, body);
    this->m_AshitaCore->GetChatManager()->AddChatMessage(1, false, buffer);
}

int targetaura::ResolveCategory(uint32_t index)
{
    IMemoryManager* mm = (this->m_AshitaCore != nullptr) ? this->m_AshitaCore->GetMemoryManager() : nullptr;
    if (mm == nullptr)
        return CAT_DEFAULT;
    IEntity* ent   = mm->GetEntity();
    IParty*  party = mm->GetParty();
    if (ent == nullptr || party == nullptr)
        return CAT_DEFAULT;

    for (uint32_t i = 0; i < 18; ++i)
        if (party->GetMemberIsActive(i) != 0 && party->GetMemberTargetIndex(i) == index)
            return CAT_PARTY;

    const uint32_t flags = ent->GetSpawnFlags(index);
    if ((flags & 0x10) != 0)
    {
        const uint32_t claim = ent->GetClaimStatus(index) & 0xFFFF;
        if (claim != 0)
        {
            for (uint32_t i = 0; i < 18; ++i)
                if (party->GetMemberIsActive(i) != 0)
                {
                    const uint32_t sid = party->GetMemberServerId(i);
                    if (sid == claim || (sid & 0xFFFF) == claim)
                        return CAT_CLAIM_US;
                }
            return CAT_CLAIM_OTHER;
        }
        return CAT_ENEMY;
    }
    if ((flags & 0x01) != 0) return CAT_PLAYER;
    if ((flags & 0x02) != 0) return CAT_NPC;
    return CAT_DEFAULT;
}

uint32_t targetaura::PackRGB(const float rgb[3], float mul) const
{
    auto clamp8 = [](float v) -> uint32_t {
        if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
        return static_cast<uint32_t>(v * 255.0f + 0.5f);
    };
    return (clamp8(rgb[0] * mul) << 16) | (clamp8(rgb[1] * mul) << 8) | clamp8(rgb[2] * mul);
}

// ---------------------------------------------------------------------------------------------------
// Special-target matching: NMs (baked-in list, binary search) + a user Watchlist (name substrings).
// ---------------------------------------------------------------------------------------------------
static void ta_lower(std::string& s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
}

bool targetaura::MatchesWatch(const char* name) const
{
    if (name == nullptr || name[0] == '\0' || this->m_Watch.empty())
        return false;
    std::string n(name);
    ta_lower(n);
    for (const std::string& w : this->m_Watch)
        if (!w.empty() && n.find(w) != std::string::npos)
            return true;
    return false;
}

bool targetaura::IsNM(const char* name) const
{
    if (name == nullptr || name[0] == '\0')
        return false;
    char buf[64];
    size_t i = 0;
    for (; name[i] != '\0' && i < sizeof(buf) - 1; ++i)
    {
        const char c = name[i];
        buf[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    buf[i] = '\0';

    int lo = 0, hi = g_nmNameCount - 1;      // g_nmNames is lower-case + strcmp-sorted
    while (lo <= hi)
    {
        const int mid = (lo + hi) / 2;
        const int cmp = strcmp(buf, g_nmNames[mid]);
        if (cmp == 0) return true;
        if (cmp < 0)  hi = mid - 1;
        else          lo = mid + 1;
    }
    return false;
}

void targetaura::AddWatch(const char* name)
{
    if (name == nullptr)
        return;
    std::string s(name);
    const size_t a = s.find_first_not_of(" \t");
    const size_t b = s.find_last_not_of(" \t");
    if (a == std::string::npos)
        return;
    s = s.substr(a, b - a + 1);
    ta_lower(s);
    if (s.empty())
        return;
    for (const std::string& w : this->m_Watch)
        if (w == s) return;             // no duplicates
    this->m_Watch.push_back(s);
}

// ---------------------------------------------------------------------------------------------------
// Settings persistence (self-managed ini in Ashita's config folder).
// ---------------------------------------------------------------------------------------------------
std::string targetaura::SettingsPath(void) const
{
    std::string base = "C:\\HorizonXI\\Game\\config\\";
    if (this->m_AshitaCore != nullptr)
    {
        const char* p = this->m_AshitaCore->GetInstallPath();
        if (p != nullptr && p[0] != '\0')
        {
            base = p;
            if (base.back() != '\\' && base.back() != '/')
                base += '\\';
            base += "config\\";
        }
    }
    return base + "targetaura.ini";
}

void targetaura::SaveSettings(void)
{
    FILE* f = fopen(this->SettingsPath().c_str(), "w");
    if (f == nullptr)
        return;
    fprintf(f, "enabled=%d\n",       this->m_Enabled ? 1 : 0);
    fprintf(f, "rimOnly=%d\n",       this->m_RimOnly ? 1 : 0);
    fprintf(f, "fresnel=%d\n",       this->m_Fresnel ? 1 : 0);
    fprintf(f, "fresnelLayers=%d\n", this->m_FresnelLayers);
    fprintf(f, "sweep=%d\n",         this->m_Sweep ? 1 : 0);
    fprintf(f, "sweepSpeed=%.4f\n",  this->m_SweepSpeed);
    fprintf(f, "sweepWidth=%.4f\n",  this->m_SweepWidth);
    fprintf(f, "cull=%d\n",          this->m_CullOverride);
    fprintf(f, "gateCenter=%d\n",    this->m_GateCenter);
    fprintf(f, "gateSpan=%d\n",      this->m_GateSpan);
    fprintf(f, "intensity=%.4f\n",   this->m_Intensity);
    fprintf(f, "scale=%.4f\n",       this->m_Scale);
    fprintf(f, "pulse=%d\n",         this->m_Pulse ? 1 : 0);
    fprintf(f, "pulseSpeed=%.4f\n",  this->m_PulseSpeed);
    fprintf(f, "pulseAmount=%.4f\n", this->m_PulseAmount);
    fprintf(f, "twoColor=%d\n",      this->m_TwoColor ? 1 : 0);
    fprintf(f, "twoCol=%.4f,%.4f,%.4f\n", this->m_Col2[0], this->m_Col2[1], this->m_Col2[2]);
    fprintf(f, "twoColorSpeed=%.4f\n", this->m_TwoColorSpeed);
    for (int i = 0; i < 7; ++i)
        fprintf(f, "col%d=%.4f,%.4f,%.4f\n", i, this->m_Col[i][0], this->m_Col[i][1], this->m_Col[i][2]);
    fprintf(f, "spEnabled=%d\n",       this->m_SpEnabled ? 1 : 0);
    fprintf(f, "spNM=%d\n",            this->m_SpNM ? 1 : 0);
    fprintf(f, "spCol=%.4f,%.4f,%.4f\n", this->m_SpCol[0], this->m_SpCol[1], this->m_SpCol[2]);
    fprintf(f, "spScale=%.4f\n",       this->m_SpScale);
    fprintf(f, "spPulse=%d\n",         this->m_SpPulse ? 1 : 0);
    fprintf(f, "spPulseSpeed=%.4f\n",  this->m_SpPulseSpeed);
    fprintf(f, "spPulseAmount=%.4f\n", this->m_SpPulseAmount);
    std::string joined;
    for (size_t i = 0; i < this->m_Watch.size(); ++i) { if (i) joined += '|'; joined += this->m_Watch[i]; }
    fprintf(f, "watch=%s\n", joined.c_str());
    fclose(f);
}

void targetaura::LoadSettings(void)
{
    FILE* f = fopen(this->SettingsPath().c_str(), "r");
    if (f == nullptr)
        return;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr)
    {
        char* nl = strpbrk(line, "\r\n"); if (nl != nullptr) *nl = '\0';
        char* eq = strchr(line, '=');     if (eq == nullptr) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;
        if      (_stricmp(key, "enabled")      == 0) this->m_Enabled      = atoi(val) != 0;
        else if (_stricmp(key, "rimOnly")      == 0) this->m_RimOnly      = atoi(val) != 0;
        else if (_stricmp(key, "fresnel")      == 0) this->m_Fresnel      = atoi(val) != 0;
        else if (_stricmp(key, "fresnelLayers")== 0) this->m_FresnelLayers= atoi(val);
        else if (_stricmp(key, "sweep")        == 0) this->m_Sweep        = atoi(val) != 0;
        else if (_stricmp(key, "sweepSpeed")   == 0) this->m_SweepSpeed   = static_cast<float>(atof(val));
        else if (_stricmp(key, "sweepWidth")   == 0) this->m_SweepWidth   = static_cast<float>(atof(val));
        else if (_stricmp(key, "cull")         == 0) this->m_CullOverride = atoi(val);
        else if (_stricmp(key, "gateCenter")   == 0) this->m_GateCenter   = atoi(val);
        else if (_stricmp(key, "gateSpan")     == 0) this->m_GateSpan     = atoi(val);
        else if (_stricmp(key, "intensity")    == 0) this->m_Intensity    = static_cast<float>(atof(val));
        else if (_stricmp(key, "scale")        == 0) this->m_Scale        = static_cast<float>(atof(val));
        else if (_stricmp(key, "pulse")        == 0) this->m_Pulse        = atoi(val) != 0;
        else if (_stricmp(key, "pulseSpeed")   == 0) this->m_PulseSpeed   = static_cast<float>(atof(val));
        else if (_stricmp(key, "pulseAmount")  == 0) this->m_PulseAmount  = static_cast<float>(atof(val));
        else if (_stricmp(key, "twoColor")     == 0) this->m_TwoColor     = atoi(val) != 0;
        else if (_stricmp(key, "twoCol")       == 0) sscanf_s(val, "%f,%f,%f", &this->m_Col2[0], &this->m_Col2[1], &this->m_Col2[2]);
        else if (_stricmp(key, "twoColorSpeed")== 0) this->m_TwoColorSpeed= static_cast<float>(atof(val));
        else if (_stricmp(key, "spEnabled")    == 0) this->m_SpEnabled    = atoi(val) != 0;
        else if (_stricmp(key, "spNM")         == 0) this->m_SpNM         = atoi(val) != 0;
        else if (_stricmp(key, "spScale")      == 0) this->m_SpScale      = static_cast<float>(atof(val));
        else if (_stricmp(key, "spPulse")      == 0) this->m_SpPulse      = atoi(val) != 0;
        else if (_stricmp(key, "spPulseSpeed") == 0) this->m_SpPulseSpeed = static_cast<float>(atof(val));
        else if (_stricmp(key, "spPulseAmount")== 0) this->m_SpPulseAmount= static_cast<float>(atof(val));
        else if (_stricmp(key, "spCol")        == 0) sscanf_s(val, "%f,%f,%f", &this->m_SpCol[0], &this->m_SpCol[1], &this->m_SpCol[2]);
        else if (_strnicmp(key, "col", 3) == 0 && key[3] >= '0' && key[3] <= '6')
        {
            const int i = key[3] - '0';
            sscanf_s(val, "%f,%f,%f", &this->m_Col[i][0], &this->m_Col[i][1], &this->m_Col[i][2]);
        }
        else if (_stricmp(key, "watch") == 0)
        {
            this->m_Watch.clear();
            std::string v(val);
            size_t start = 0;
            while (start <= v.size())
            {
                const size_t bar = v.find('|', start);
                std::string tok = (bar == std::string::npos) ? v.substr(start) : v.substr(start, bar - start);
                if (!tok.empty()) this->m_Watch.push_back(tok);   // stored lower-case already
                if (bar == std::string::npos) break;
                start = bar + 1;
            }
        }
    }
    fclose(f);
}

// ---------------------------------------------------------------------------------------------------
// Auto-calibration of the gate offset: run a wide stack scan for a few frames, then pick the stack word
// where the target's actor pointer showed up most often (the true "current actor" slot).
// ---------------------------------------------------------------------------------------------------
void targetaura::StartCalibration(void)
{
    memset(g_calHist, 0, sizeof(g_calHist));
    this->m_CalFrames = 40;
    this->WriteChat("calibrating... keep a visible enemy targeted for ~1 second.", 0x06);
}

void targetaura::FinishCalibration(void)
{
    int      best  = -1;
    uint16_t bestN = 0;
    for (int i = 0; i < SCAN_WORDS; ++i)
        if (g_calHist[i] > bestN) { bestN = g_calHist[i]; best = i; }
    memset(g_calHist, 0, sizeof(g_calHist));

    if (best >= 0 && bestN >= 8)
    {
        this->m_GateCenter = best;
        this->m_GateSpan   = 24;
        this->SaveSettings();
        char b[128];
        _snprintf_s(b, sizeof(b), _TRUNCATE, "calibrated: gate centre = %d (%u hits). Saved.", best, bestN);
        this->WriteChat(b, 0x06);
    }
    else
    {
        this->WriteChat("calibration failed - target a visible enemy whose model is drawing, then retry.", 0x44);
    }
}

// ---------------------------------------------------------------------------------------------------
// D3D setup + per-frame target resolve
// ---------------------------------------------------------------------------------------------------
bool targetaura::Direct3DInitialize(IDirect3DDevice8* device)
{
    this->m_Device = device;

    // Build the reusable state block + the sweep stripe texture up front (outside any scene).
    if (device != nullptr && !g_sbReady && build_halo_block(device))
        g_sbReady = true;
    build_stripe(device);

    return true;
}

void targetaura::Direct3DBeginScene(bool isRenderingBackBuffer)
{
    UNREFERENCED_PARAMETER(isRenderingBackBuffer);

    this->m_HasTarget = false;
    g_targetActor     = 0;

    if (!this->m_Enabled || this->m_AshitaCore == nullptr)
        return;

    IMemoryManager* mm = this->m_AshitaCore->GetMemoryManager();
    if (mm == nullptr)
        return;
    ITarget* target = mm->GetTarget();
    IEntity* ent    = mm->GetEntity();
    if (target == nullptr || ent == nullptr)
        return;

    const uint32_t sub = target->GetIsSubTargetActive();
    if (target->GetIsActive(sub) == 0)
        return;
    const uint32_t idx = target->GetTargetIndex(sub);

    const float x = ent->GetLocalPositionX(idx);
    const float z = ent->GetLocalPositionZ(idx);
    const float y = ent->GetLocalPositionY(idx);
    if (x == 0.0f && y == 0.0f)
        return;

    this->m_TgtX = x;
    this->m_TgtY = z;
    this->m_TgtZ = y;

    // Resolve category + special (NM / watchlist) status, then build the aura colour.
    const int   cat = this->ResolveCategory(idx);
    const char* nm  = ent->GetName(idx);
    _snprintf_s(this->m_TgtName, sizeof(this->m_TgtName), _TRUNCATE, "%s", (nm != nullptr) ? nm : "");
    const bool special = this->m_SpEnabled && ((this->m_SpNM && this->IsNM(nm)) || this->MatchesWatch(nm));
    this->m_TgtSpecial = special;

    const float t = static_cast<float>(GetTickCount()) * 0.001f;

    // Base colour + intensity: category style normally, special (NM/watchlist) style overrides it.
    const float* baseRgb;
    float        mul;
    if (special)
    {
        this->m_ActiveScale = this->m_SpScale;
        baseRgb = this->m_SpCol;
        mul = 1.0f;
        if (this->m_SpPulse)
            mul = (1.0f - this->m_SpPulseAmount) + this->m_SpPulseAmount * (0.5f + 0.5f * sinf(t * this->m_SpPulseSpeed));
    }
    else
    {
        this->m_ActiveScale = this->m_Scale;
        baseRgb = this->m_Col[cat];
        mul = this->m_Intensity;
        if (this->m_Pulse)
            mul *= (1.0f - this->m_PulseAmount) + this->m_PulseAmount * (0.5f + 0.5f * sinf(t * this->m_PulseSpeed));
    }

    // Two-colour pulse: cycle the colour between the base colour and the configured 2nd colour.
    float rgb[3] = { baseRgb[0], baseRgb[1], baseRgb[2] };
    if (this->m_TwoColor)
    {
        const float ph = 0.5f + 0.5f * sinf(t * this->m_TwoColorSpeed);
        rgb[0] = baseRgb[0] + (this->m_Col2[0] - baseRgb[0]) * ph;
        rgb[1] = baseRgb[1] + (this->m_Col2[1] - baseRgb[1]) * ph;
        rgb[2] = baseRgb[2] + (this->m_Col2[2] - baseRgb[2]) * ph;
    }

    this->m_TgtColor  = this->PackRGB(rgb, mul);
    this->m_HasTarget = true;

    g_targetActor = ent->GetActorPointer(idx);
}

void targetaura::Direct3DEndScene(bool isRenderingBackBuffer)
{
    UNREFERENCED_PARAMETER(isRenderingBackBuffer);
}

// ---------------------------------------------------------------------------------------------------
// The additive halo pass: Capture the current values of the states we're about to change into our state
// block, set our own state, re-issue the geometry enlarged in the aura colour, then Apply the block to
// restore them. Because the block tracks EXACTLY the states we touch (see build_halo_block), the game's
// own textured model draw is left untouched -- no white models, no vertex-buffer locking.
// ---------------------------------------------------------------------------------------------------
void targetaura::DrawHalo(D3DPRIMITIVETYPE type, UINT minIndex, UINT numVertices,
                          UINT startIndex, UINT primCount, uint32_t rgb)
{
    IDirect3DDevice8* dev = this->m_Device;
    if (dev == nullptr)
        return;

    __try
    {
        if (!g_sbReady)
        {
            if (!build_halo_block(dev))
                return;                          // can't guarantee a clean restore -> draw nothing
            g_sbReady = true;
        }

        // Rim cull (front-face) for the rim + fresnel looks; no cull for the full additive fill.
        DWORD cull = D3DCULL_NONE;
        if (this->m_RimOnly || this->m_Fresnel)
        {
            switch (this->m_CullOverride)
            {
                case 1:  cull = D3DCULL_NONE; break;
                case 2:  cull = D3DCULL_CW;   break;
                case 3:  cull = D3DCULL_CCW;  break;
                // auto: fresnel looks best with no cull (full layered glow); the plain rim wants CCW.
                default: cull = this->m_Fresnel ? D3DCULL_NONE : D3DCULL_CCW; break;
            }
        }

        dev->CaptureStateBlock(g_sbToken);       // snapshot the tracked states' current values (once)

        dev->SetRenderState(D3DRS_ZENABLE,          TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE,     FALSE);
        dev->SetRenderState(D3DRS_ZFUNC,            D3DCMP_LESSEQUAL);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND,         D3DBLEND_SRCALPHA);   // additive, scaled by per-shell alpha
        dev->SetRenderState(D3DRS_DESTBLEND,        D3DBLEND_ONE);
        dev->SetRenderState(D3DRS_LIGHTING,         FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE,        FALSE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,  FALSE);
        dev->SetRenderState(D3DRS_CULLMODE,         cull);

        // Sweep: MODULATE the aura by a scrolling grey stripe (camera-space-position texcoord gen). Only
        // works if the game's model draws use fixed-function T&L; a no-op otherwise.
        const bool sweep = this->m_Sweep && g_stripeTex != nullptr;
        if (sweep)
        {
            const float ts = static_cast<float>(GetTickCount()) * 0.001f;
            const float k  = 1.0f / (0.4f + this->m_SweepWidth * 3.0f);
            D3DMATRIX tm;
            memset(&tm, 0, sizeof(tm));
            tm._21 = k;                          // camera-space Y -> u
            tm._41 = this->m_SweepSpeed * ts;    // scroll u over time
            tm._42 = 0.5f;                        // v = middle texture row
            dev->SetTransform(D3DTS_TEXTURE0, &tm);
            dev->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEPOSITION);
            dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
            dev->SetTexture(0, g_stripeTex);
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_MODULATE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
            dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        }
        else
        {
            dev->SetTexture(0, nullptr);
            dev->SetTextureStageState(0, D3DTSS_COLOROP,   D3DTOP_SELECTARG1);
            dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);
        }
        dev->SetTextureStageState(0, D3DTSS_ALPHAOP,   D3DTOP_SELECTARG1);
        dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TFACTOR);

        // One shell normally; several stacked rim shells (soft fresnel rim-light) when enabled.
        int layers = this->m_Fresnel ? this->m_FresnelLayers : 1;
        if (layers < 1)  layers = 1;
        if (layers > 16) layers = 16;

        for (int k = 0; k < layers; ++k)
        {
            float scale, alpha;
            if (this->m_Fresnel)
            {
                const float frac = (layers > 1) ? (static_cast<float>(k) / (layers - 1)) : 0.0f;
                scale = 1.0f + (this->m_ActiveScale - 1.0f) * (0.5f + 1.4f * frac); // inner tight -> outer wide
                const float f = 1.0f - frac;
                alpha = f * f;                                                       // brightest inner, fades out
            }
            else
            {
                scale = this->m_ActiveScale;
                alpha = 1.0f;
            }

            D3DMATRIX m;
            memset(&m, 0, sizeof(m));
            m._11 = scale; m._22 = scale; m._33 = scale; m._44 = 1.0f;
            m._41 = this->m_TgtX * (1.0f - scale);
            m._42 = this->m_TgtY * (1.0f - scale);
            m._43 = this->m_TgtZ * (1.0f - scale);

            const uint32_t a = static_cast<uint32_t>(alpha * 255.0f + 0.5f);
            dev->SetTransform(D3DTS_WORLD, &m);
            dev->SetRenderState(D3DRS_TEXTUREFACTOR, (a << 24) | (rgb & 0x00FFFFFFu));
            dev->DrawIndexedPrimitive(type, minIndex, numVertices, startIndex, primCount);
        }

        dev->ApplyStateBlock(g_sbToken);         // restore the tracked states (once)
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
}

// ---------------------------------------------------------------------------------------------------
// Per-draw interception: gate on whether the target's actor pointer sits in the tight "current actor"
// stack window; if so, re-issue this mesh as a coloured aura.
// ---------------------------------------------------------------------------------------------------
bool targetaura::Direct3DDrawIndexedPrimitive(D3DPRIMITIVETYPE PrimitiveType, UINT minIndex,
                                              UINT NumVertices, UINT startIndex, UINT primCount)
{
    if (g_inHalo)                                   // our own DrawHalo re-issued this DIP
        return false;
    if (!this->m_Enabled || !this->m_HasTarget || this->m_Device == nullptr || g_targetActor == 0)
        return false;

    uintptr_t espv = 0;
    __asm { mov espv, esp }

    // Auto-calibration: while active, tally where the target pointer sits across a wide window.
    if (this->m_CalFrames > 0)
        cal_scan(espv);

    // THE GATE: this draw belongs to the target iff its actor pointer is in the narrow "current actor"
    // stack slot (~0x4EC). A tight window (not the whole stack) rejects stale pointers from nearby
    // entities, which otherwise makes the aura bleed onto self/neighbours in crowded scenes.
    if (!stack_has_at(espv, g_targetActor, this->m_GateCenter - this->m_GateSpan,
                      this->m_GateCenter + this->m_GateSpan + 1))
        return false;

    this->m_FrameMeshes++;

    // Re-issue this mesh as the aura (a single shell, or stacked rim shells for the fresnel look).
    g_inHalo = true;
    this->DrawHalo(PrimitiveType, minIndex, NumVertices, startIndex, primCount, this->m_TgtColor & 0x00FFFFFFu);
    g_inHalo = false;
    return false;
}

// ---------------------------------------------------------------------------------------------------
// Config UI (dark + gold, "Cinematic Hero" palette)
// ---------------------------------------------------------------------------------------------------
void targetaura::Direct3DPresent(const RECT* a, const RECT* b, HWND c, const RGNDATA* d)
{
    UNREFERENCED_PARAMETER(a); UNREFERENCED_PARAMETER(b);
    UNREFERENCED_PARAMETER(c); UNREFERENCED_PARAMETER(d);

    // Once per frame (after all BeginScene/EndScene pairs): snapshot the aura mesh count and reset.
    this->m_LastMeshes  = this->m_FrameMeshes;
    this->m_FrameMeshes = 0;

    // Auto-calibration countdown.
    if (this->m_CalFrames > 0)
    {
        this->m_CalFrames--;
        if (this->m_CalFrames == 0)
            this->FinishCalibration();
    }

    if (this->m_UiOpen)
        this->RenderUI();
}

void targetaura::RenderUI(void)
{
    IGuiManager* g = (this->m_AshitaCore != nullptr) ? this->m_AshitaCore->GetGuiManager() : nullptr;
    if (g == nullptr)
        return;

    const ImVec4 gold (0.957f, 0.855f, 0.592f, 1.0f);
    const ImVec4 muted(0.600f, 0.580f, 0.540f, 1.0f);

    g->PushStyleColor(ImGuiCol_WindowBg,        ImVec4(0.051f, 0.051f, 0.051f, 0.96f));
    g->PushStyleColor(ImGuiCol_Border,          ImVec4(0.300f, 0.275f, 0.235f, 1.0f));
    g->PushStyleColor(ImGuiCol_Text,            ImVec4(0.878f, 0.855f, 0.812f, 1.0f));
    g->PushStyleColor(ImGuiCol_TextDisabled,    muted);
    g->PushStyleColor(ImGuiCol_FrameBg,         ImVec4(0.137f, 0.125f, 0.106f, 1.0f));
    g->PushStyleColor(ImGuiCol_FrameBgHovered,  ImVec4(0.176f, 0.161f, 0.137f, 1.0f));
    g->PushStyleColor(ImGuiCol_FrameBgActive,   ImVec4(0.200f, 0.180f, 0.150f, 1.0f));
    g->PushStyleColor(ImGuiCol_TitleBg,         ImVec4(0.098f, 0.090f, 0.075f, 1.0f));
    g->PushStyleColor(ImGuiCol_TitleBgActive,   ImVec4(0.098f, 0.090f, 0.075f, 1.0f));
    g->PushStyleColor(ImGuiCol_Header,          ImVec4(0.573f, 0.512f, 0.355f, 0.45f));
    g->PushStyleColor(ImGuiCol_HeaderHovered,   ImVec4(0.573f, 0.512f, 0.355f, 0.65f));
    g->PushStyleColor(ImGuiCol_HeaderActive,    ImVec4(0.573f, 0.512f, 0.355f, 0.85f));
    g->PushStyleColor(ImGuiCol_Button,          ImVec4(0.200f, 0.180f, 0.140f, 1.0f));
    g->PushStyleColor(ImGuiCol_ButtonHovered,   ImVec4(0.280f, 0.250f, 0.190f, 1.0f));
    g->PushStyleColor(ImGuiCol_ButtonActive,    ImVec4(0.360f, 0.320f, 0.240f, 1.0f));
    g->PushStyleColor(ImGuiCol_SliderGrab,      gold);
    g->PushStyleColor(ImGuiCol_SliderGrabActive,ImVec4(0.98f, 0.90f, 0.66f, 1.0f));
    g->PushStyleColor(ImGuiCol_CheckMark,       gold);
    g->PushStyleColor(ImGuiCol_Separator,       ImVec4(0.300f, 0.275f, 0.235f, 1.0f));

    g->PushStyleVar(ImGuiStyleVar_WindowRounding,  10.0f);
    g->PushStyleVar(ImGuiStyleVar_FrameRounding,    5.0f);
    g->PushStyleVar(ImGuiStyleVar_FrameBorderSize,  1.0f);
    g->PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(15.0f, 13.0f));
    g->PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(8.0f, 7.0f));

    g->SetNextWindowSize(ImVec2(360.0f, 0.0f), ImGuiCond_FirstUseEver);
    if (g->Begin("targetaura", &this->m_UiOpen, 0))
    {
        g->PushStyleColor(ImGuiCol_Text, gold);
        g->Text("TARGET AURA");
        g->PopStyleColor(1);
        g->SameLine(0.0f, 10.0f);
        g->TextColored(muted, "v1.0");
        g->SameLine(0.0f, 12.0f);
        if (this->m_HasTarget && this->m_TgtSpecial)
            g->TextColored(gold, "[SPECIAL: %u]", this->m_LastMeshes);
        else if (this->m_HasTarget)
            g->TextColored(ImVec4(0.45f, 0.80f, 0.50f, 1.0f), "[Target: %u]", this->m_LastMeshes);
        else
            g->TextColored(muted, "[no target]");
        g->Separator();

        g->Checkbox("Enabled", &this->m_Enabled);

        g->PushStyleColor(ImGuiCol_Text, gold);
        g->Text("Appearance");
        g->PopStyleColor(1);
        g->Checkbox("Outline only", &this->m_RimOnly);
        if (!this->m_RimOnly)
        {
            g->SameLine(0.0f, 10.0f);
            g->TextColored(muted, "= full glow");
        }
        g->Checkbox("Fresnel rim-light", &this->m_Fresnel);
        if (this->m_Fresnel)
        {
            g->SameLine(0.0f, 10.0f);
            g->TextColored(muted, "soft edge glow");
            g->SliderInt("Fresnel layers", &this->m_FresnelLayers, 2, 12);
        }
        g->Checkbox("Sweep", &this->m_Sweep);
        if (this->m_Sweep)
        {
            g->SliderFloat("Sweep speed", &this->m_SweepSpeed, 0.1f, 4.0f, "%.2f");
            g->SliderFloat("Sweep width", &this->m_SweepWidth, 0.02f, 0.50f, "%.2f");
        }
        g->SliderFloat("Thickness", &this->m_Scale, 1.005f, 1.30f, "%.3f");
        g->SliderFloat("Intensity", &this->m_Intensity, 0.0f, 1.5f, "%.2f");

        if (g->CollapsingHeader("Animation", 0))
        {
            g->Checkbox("Pulse", &this->m_Pulse);
            g->SliderFloat("Pulse speed", &this->m_PulseSpeed, 0.5f, 8.0f, "%.1f");
            g->SliderFloat("Pulse strength", &this->m_PulseAmount, 0.0f, 1.0f, "%.2f");
            g->Separator();
            g->Checkbox("Two-colour pulse", &this->m_TwoColor);
            if (this->m_TwoColor)
            {
                g->ColorEdit3("Second colour", this->m_Col2);
                g->SliderFloat("Two-colour speed", &this->m_TwoColorSpeed, 0.2f, 6.0f, "%.1f");
            }
        }

        if (g->CollapsingHeader("Colours (by category)", 0))
        {
            g->ColorEdit3("Enemy (unclaimed)", this->m_Col[CAT_ENEMY]);
            g->ColorEdit3("Claim (you)",       this->m_Col[CAT_CLAIM_US]);
            g->ColorEdit3("Claim (other)",     this->m_Col[CAT_CLAIM_OTHER]);
            g->ColorEdit3("Player",            this->m_Col[CAT_PLAYER]);
            g->ColorEdit3("Party/Alliance",    this->m_Col[CAT_PARTY]);
            g->ColorEdit3("NPC",               this->m_Col[CAT_NPC]);
            g->ColorEdit3("Default",           this->m_Col[CAT_DEFAULT]);
        }

        if (g->CollapsingHeader("Special targets", 0))
        {
            g->Checkbox("Special aura enabled", &this->m_SpEnabled);
            g->Checkbox("Auto-detect NMs", &this->m_SpNM);
            g->ColorEdit3("Special colour", this->m_SpCol);
            g->SliderFloat("Special thickness", &this->m_SpScale, 1.005f, 1.40f, "%.3f");
            g->Checkbox("Special pulse", &this->m_SpPulse);
            if (this->m_SpPulse)
            {
                g->SliderFloat("Special speed", &this->m_SpPulseSpeed, 0.5f, 10.0f, "%.1f");
                g->SliderFloat("Special strength", &this->m_SpPulseAmount, 0.0f, 1.0f, "%.2f");
            }
            g->Separator();
            g->TextColored(muted, "Names (substring, case-insensitive):");
            g->PushItemWidth(200.0f);
            g->InputText("##watchadd", this->m_WatchInput, sizeof(this->m_WatchInput), 0);
            g->PopItemWidth();
            g->SameLine(0.0f, 6.0f);
            if (g->Button("+ Name"))
            {
                this->AddWatch(this->m_WatchInput);
                this->m_WatchInput[0] = '\0';
            }
            g->SameLine(0.0f, 6.0f);
            if (g->Button("+ Target") && this->m_TgtName[0] != '\0')
                this->AddWatch(this->m_TgtName);
            int removeIdx = -1;
            for (int i = 0; i < static_cast<int>(this->m_Watch.size()); ++i)
            {
                char rid[24];
                _snprintf_s(rid, sizeof(rid), _TRUNCATE, "X##w%d", i);
                if (g->Button(rid))
                    removeIdx = i;
                g->SameLine(0.0f, 8.0f);
                g->Text("%s", this->m_Watch[i].c_str());
            }
            if (removeIdx >= 0)
                this->m_Watch.erase(this->m_Watch.begin() + removeIdx);
            if (this->m_Watch.empty())
                g->TextColored(muted, "(empty - add names or target)");
        }

        if (g->CollapsingHeader("Advanced", 0))
        {
            g->TextColored(muted, "Cull side (adjust on graphics glitches)");
            g->SliderInt("Cull", &this->m_CullOverride, 0, 3);
            g->TextColored(muted, "0=auto  1=none  2=CW  3=CCW");
            g->Separator();
            g->TextColored(muted, "Gate window (only if aura is missing / bleeding):");
            g->SliderInt("Gate centre", &this->m_GateCenter, 0, 800);
            g->SliderInt("Gate width", &this->m_GateSpan, 1, 400);
            g->Text("Aura meshes: %u", this->m_LastMeshes);
            if (g->Button("Calibrate gate now"))
                this->StartCalibration();
            g->SameLine(0.0f, 8.0f);
            g->TextColored(muted, "(auto-find offset)");
        }

        g->Separator();
        if (g->Button("Save settings"))
        {
            this->SaveSettings();
            this->WriteChat("Settings saved.");
        }
        g->SameLine(0.0f, 8.0f);
        g->TextColored(muted, "(auto on unload)");
    }
    g->End();

    g->PopStyleVar(5);
    g->PopStyleColor(19);
}

// ---------------------------------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------------------------------
bool targetaura::HandleCommand(int32_t mode, const char* command, bool injected)
{
    UNREFERENCED_PARAMETER(mode);
    UNREFERENCED_PARAMETER(injected);

    if (command == nullptr)
        return false;

    std::vector<std::string> args;
    if (Ashita::Commands::GetCommandArgs(command, &args) < 1)
        return false;

    if (_stricmp(args[0].c_str(), "/targetaura") != 0 && _stricmp(args[0].c_str(), "/taura") != 0)
        return false;

    const char* sub = (args.size() > 1) ? args[1].c_str() : "";

    if (_stricmp(sub, "on") == 0)
        { this->m_Enabled = true; this->WriteChat("enabled."); }
    else if (_stricmp(sub, "off") == 0)
        { this->m_Enabled = false; this->WriteChat("disabled."); }
    else if (_stricmp(sub, "save") == 0)
        { this->SaveSettings(); this->WriteChat("Settings saved."); }
    else if (_stricmp(sub, "calibrate") == 0 || _stricmp(sub, "cal") == 0)
        { this->StartCalibration(); }
    else if (_stricmp(sub, "help") == 0)
    {
        this->WriteChat("commands:", 0x06);
        this->WriteChat("  /targetaura       - open / close the config panel");
        this->WriteChat("  /taura on | off   - enable / disable the aura");
        this->WriteChat("  /taura save       - save settings now (also auto-saves on unload)");
        this->WriteChat("  /taura calibrate  - auto-find the gate offset (use if the aura is missing)");
        this->WriteChat("  /taura help       - this list");
    }
    else
        this->m_UiOpen = !this->m_UiOpen;

    return true;
}

// ---------------------------------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------------------------------
extern "C"
{
    __declspec(noinline) IPlugin* __stdcall expCreatePlugin(const char* args)
    {
        UNREFERENCED_PARAMETER(args);
        try { return new targetaura(); }
        catch (...) { return nullptr; }
    }

    __declspec(noinline) void __stdcall expDestroyPlugin(void* instance)
    {
        if (instance != nullptr)
            delete static_cast<targetaura*>(instance);
    }

    __declspec(noinline) double __stdcall expGetInterfaceVersion(void)
    {
        return ASHITA_INTERFACE_VERSION;
    }
}
