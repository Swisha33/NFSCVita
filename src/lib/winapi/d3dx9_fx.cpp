// ID3DXEffect of d3dx9_30 for Carbon: compiled effects (fx_2_0, tag 0xFEFF0901) read from NFSC.exe's RCDATA
// resources (IDI_*_FX). Parameters, annotations, techniques and passes are real: the game gets handles, sets and
// reads values, and walks techniques and passes. The shaders inside (vertex and pixel bytecode) are kept for the
// renderer; drawing with them is the next step (docs/STATUS.md, milestone 3).
//
// The fx_2_0 layout and the interface order follow the NFS Carbon macOS port's kit/dx/d3dx9.cpp
// (github.com/vasist15-source/nfs-carbon-mac, MIT), which follows Wine's d3dx9 effect.c.

#include <winapi/d3d9_internal.h>
#include <winapi/kernel32.h>
#include <platform/config.h>
#include <platform/log.h>
#include "crt.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace win32 { namespace d3d9
{

namespace
{

using x86::reg32;

const reg32 E_NOTIMPL_ = 0x80004001;

enum : uint32_t { ClassScalar, ClassVector, ClassMatrixRows, ClassMatrixColumns, ClassObject, ClassStruct };
enum : uint32_t
{
    TypeBool = 1, TypeInt = 2, TypeFloat = 3, TypeString = 4, TypeTexture = 5, TypeTextureCube = 9, TypeSampler = 10,
    TypeSamplerCube = 14, TypePixelShader = 15, TypeVertexShader = 16
};
enum : uint32_t { StateVertexShader = 146, StatePixelShader = 147 };

// Handles: tagged values far above any guest address. Anything else non-zero is a guest string (a name).
enum : reg32
{
    HandleParam = 0xF1000000, HandleTechnique = 0xF2000000, HandlePass = 0xF3000000, HandleAnnotation = 0xF4000000,
    HandleTagMask = 0xFF000000
};

reg32 ld(reg32 a)               { reg32 v; memcpy(&v, g_guestBase + a, 4); return v; }
void st(reg32 a, reg32 v)       { memcpy(g_guestBase + a, &v, 4); }
reg32 arg(const x86::CPU& cpu, unsigned i) { return ld(cpu.esp + 4 + 4 * i); }

std::string guestString(reg32 a, size_t limit = 256)
{
    std::string s;
    while (a && s.size() < limit)
    {
        const char c = char(g_guestBase[a + reg32(s.size())]);
        if (!c)
            break;
        s += c;
    }
    return s;
}

bool sameName(const std::string& a, const std::string& b)
{
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (toupper(uint8_t(a[i])) != toupper(uint8_t(b[i])))
            return false;
    return true;
}

// A guest copy of a string, made once and kept (descriptions hand these out).
reg32 keptString(const std::string& s, reg32& slot)
{
    if (!slot)
    {
        slot = crt::allocate(reg32(s.size() + 1), true);
        if (slot)
            memcpy(g_guestBase + slot, s.c_str(), s.size() + 1);
    }
    return slot;
}

struct FxType
{
    uint32_t type = 0, cls = 0, rows = 0, cols = 0, elements = 0;
    std::string name, semantic;
    std::vector<FxType> members;

    bool numeric() const    { return cls <= ClassMatrixColumns; }
    bool sampler() const    { return cls == ClassObject && type >= TypeSampler && type <= TypeSamplerCube; }
    bool texture() const    { return cls == ClassObject && type >= TypeTexture && type <= TypeTextureCube; }
    uint32_t count() const  { return elements ? elements : 1; }
    // Bytes of one element.
    uint32_t elementBytes() const
    {
        if (numeric())
            return 4 * rows * cols;
        if (cls == ClassStruct)
        {
            uint32_t n = 0;
            for (const FxType& m : members)
                n += m.bytes();
            return n;
        }
        return 4;
    }
    uint32_t bytes() const  { return elementBytes() * count(); }
};

struct FxState
{
    uint32_t op = 0, index = 0;
    FxType t;
    std::vector<uint8_t> value;
    std::vector<uint8_t> preshader;
    uint32_t object() const
    {
        uint32_t v = 0;
        if (value.size() >= 4)
            memcpy(&v, value.data(), 4);
        return v;
    }
};

// The value of a shared parameter, one per (pool, name), kept for the life of the process: every effect of the pool
// reads and writes it in place. (Before run 96 each set was copied into every other effect of the pool: with ~28
// effects that was ~5% of the game thread, plus the C library time under it.)
struct SharedSlot
{
    std::vector<uint8_t> value;
    Object* texture = nullptr;      // bound: one reference
};

struct FxParam
{
    FxType t;
    std::vector<uint8_t> value;     // own value; shared parameters use slot->value (v())
    std::vector<FxState> sampler;
    std::vector<uint32_t> annotations;
    Object* texture = nullptr;      // set with SetTexture (bound: one reference); shared: slot->texture (tex())
    bool shared = false;            // one value across the effects of a pool
    SharedSlot* slot = nullptr;     // shared parameters of an effect in a pool (joinPool)
    reg32 guestName = 0, guestSemantic = 0;
    std::vector<uint8_t>& v()               { return slot ? slot->value : value; }
    const std::vector<uint8_t>& v() const   { return slot ? slot->value : value; }
    Object*& tex()                          { return slot ? slot->texture : texture; }
    Object* tex() const                     { return slot ? slot->texture : texture; }
};

enum FxName : uint8_t { FxNameUnknown, FxNameOther, FxNameSky, FxNameWorldReflect, FxNameWorldPrelit };

struct FxPass
{
    std::string name;
    std::vector<FxState> states;
    std::vector<uint32_t> annotations;
    reg32 guestName = 0, guestVs = 0, guestPs = 0;
    // Sampler registers of the pass's pixel shader (register, parameter name), read on first use.
    std::vector<std::pair<uint32_t, std::string>> samplers;
    std::vector<int> samplerParams;     // index of each sampler's parameter (-1: none), with samplers
    bool samplersParsed = false;
};

struct FxTechnique
{
    std::string name;
    std::vector<FxPass> passes;
    std::vector<uint32_t> annotations;
    reg32 guestName = 0;
};

struct FxObject
{
    std::vector<uint8_t> data;      // shader bytecode or a string
    std::string param;              // a parameter this object names instead
    int paramIndex = -2;            // index of param in the effect (-2: not looked up yet)
};

// Bounds-checked reader over the effect body (offsets are relative to the byte after the 8-byte header).
struct Reader
{
    const uint8_t* d;
    size_t n;
    size_t p = 0;
    bool ok = true;

    uint32_t at(size_t off)
    {
        if (off + 4 > n)
        {
            ok = false;
            return 0;
        }
        uint32_t v;
        memcpy(&v, d + off, 4);
        return v;
    }
    uint32_t u32()
    {
        const uint32_t v = at(p);
        p += 4;
        return v;
    }
    std::string str(uint32_t off)
    {
        if (!off)
            return std::string();
        const uint32_t size = at(off);
        if (!ok || off + 4 + size > n)
        {
            ok = false;
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(d) + off + 4, size);
        return s.substr(0, s.find('\0'));
    }
    std::vector<uint8_t> bytes(size_t off, size_t size)
    {
        if (off + size > n)
        {
            ok = false;
            return std::vector<uint8_t>();
        }
        return std::vector<uint8_t>(d + off, d + off + size);
    }
};

FxType readType(Reader& r, size_t& pos)
{
    FxType t;
    const size_t save = r.p;
    r.p = pos;
    t.type = r.u32();
    t.cls = r.u32();
    t.name = r.str(r.u32());
    t.semantic = r.str(r.u32());
    t.elements = r.u32();
    if (t.cls <= ClassMatrixColumns)
    {
        t.cols = r.u32();
        t.rows = r.u32();
    }
    else if (t.cls == ClassStruct)
    {
        const uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && r.ok && i < 256; ++i)
        {
            size_t member = r.p;
            t.members.push_back(readType(r, member));
            r.p = member;
        }
    }
    pos = r.p;
    r.p = save;
    return t;
}

FxType typeAt(Reader& r, uint32_t off)
{
    size_t pos = off;
    return readType(r, pos);
}

FxState readState(Reader& r);

void readValue(Reader& r, const FxType& t, uint32_t off, std::vector<uint8_t>& value, std::vector<FxState>* sampler)
{
    if (t.sampler() && sampler)
    {
        const size_t save = r.p;
        r.p = off;
        const uint32_t count = r.u32();
        for (uint32_t i = 0; i < count && r.ok && i < 64; ++i)
            sampler->push_back(readState(r));
        r.p = save;
        return;
    }
    value = r.bytes(off, t.bytes());
}

FxState readState(Reader& r)
{
    FxState s;
    s.op = r.u32();
    s.index = r.u32();
    const uint32_t typeOffset = r.u32(), valueOffset = r.u32();
    s.t = typeAt(r, typeOffset);
    readValue(r, s.t, valueOffset, s.value, nullptr);
    return s;
}

}

// Effect families drawn with the port's own programs (Vita preset, as NFSUG2_VITA does it): every technique of the
// family uses one program pair; other effects keep the fixed-function stand-in.
enum FxFamily : uint8_t
{
    FamilyNone,
    FamilyWorld,        // cworld: the "lowlod" world shaders
    FamilyScreen,       // cscreen: clip-space quads, the texture of sampler 0 shown as it is
    FamilyCar,          // ccar: harmonic diffuse light, paint and decals, no specular or environment map
    FamilyMask,         // cmask: front-end quads cut out by a mask texture (the minimap)
    FamilyParticle      // cparticle: camera-facing particle and flare quads (not streak_flares)
};

FxFamily familyOf(const std::string& name)
{
    static const bool enabled = pf::Config::getBool("carbon_shaders", true);
    if (!enabled)
        return FamilyNone;
    static const char* const world[] = { "IDI_WORLD_FX", "IDI_WORLDNORMALMAP_FX", "IDI_GLASSREFLECTSHADER_FX",
                                         "IDI_WORLDREFLECT_FX", "IDI_WORLDPRELIT_FX", "IDI_SKY_FX" };
    for (const char* w : world)
        if (name == w)
            return FamilyWorld;
    // The scene is drawn into a 640x480 target; visual treatment puts it on the back buffer (run 53's dump).
    if (name == "IDI_VISUALTREATMENT_FX")
        return FamilyScreen;
    // IDI_CARNORMALMAP_FX (the tyres) has IDI_CAR_FX's inputs; drawn without its normal map. Through the stand-in its
    // model-space positions landed as clip coordinates: a tread-textured block mid-screen (runs 87-91).
    if (name == "IDI_CAR_FX" || name == "IDI_CARNORMALMAP_FX")
        return FamilyCar;
    if (name == "IDI_FE_MASK_FX")
        return FamilyMask;
    if (name == "IDI_PARTICLES_FX" && pf::Config::getBool("carbon_particles", true))
        return FamilyParticle;
    return FamilyNone;
}

class FxEffect : public Object, public EffectPass
{
public:
    FxEffect(Object* device, Object* pool, const std::string& name)
        :   Object(KindFxEffect), m_device(device), m_pool(pool), m_name(name), m_family(familyOf(name))
    {
        if (m_device)
            m_device->addRef();
        s_all.push_back(this);
        ++s_generation;
        if (m_family != FamilyNone)
            m_block = carbonBlockCreate();
    }
    ~FxEffect() override
    {
        leaveDevice();
        if (m_block != NoBlock)
            carbonBlockDestroy(m_block);
        for (FxParam& p : m_params)
            if (p.texture)
                p.texture->unbind();
        ++s_generation;
        for (size_t i = 0; i < s_all.size(); ++i)
            if (s_all[i] == this)
            {
                s_all.erase(s_all.begin() + i);
                break;
            }
        if (m_device)
            m_device->release();
    }

    bool parse(const uint8_t* blob, size_t size);
    void describe() const;
    void joinPool();

    // ---- Handles ----

    FxParam* param(reg32 h)
    {
        if ((h & HandleTagMask) == HandleParam)
            return (h & 0xffffff) < m_params.size() ? &m_params[h & 0xffffff] : nullptr;
        if ((h & HandleTagMask) == HandleAnnotation)
            return (h & 0xffffff) < m_annotations.size() ? &m_annotations[h & 0xffffff] : nullptr;
        if (!h || (h & 0xF0000000) == 0xF0000000)
            return nullptr;
        // A name: cached by guest address, checked against the name (the string at h may change).
        for (const std::pair<reg32, int>& c : m_nameIndices)
            if (c.first == h && m_params[size_t(c.second)].t.name == reinterpret_cast<const char*>(g_guestBase + h))
                return &m_params[size_t(c.second)];
        const int i = paramIndex(guestString(h));
        if (i >= 0 && m_nameIndices.size() < 64)
            m_nameIndices.emplace_back(h, i);
        return i >= 0 ? &m_params[size_t(i)] : nullptr;
    }
    // paramIndex for string literals (the families' parameter names), cached by pointer.
    int literalIndex(const char* name) const
    {
        for (const std::pair<const char*, int>& c : m_literalIndices)
            if (c.first == name)
                return c.second;
        const int i = paramIndex(std::string(name));
        m_literalIndices.emplace_back(name, i);
        return i;
    }
    int paramIndex(const std::string& name) const
    {
        for (size_t i = 0; i < m_params.size(); ++i)
            if (m_params[i].t.name == name)
                return int(i);
        return -1;
    }
    int technique(reg32 h) const
    {
        if ((h & HandleTagMask) == HandleTechnique)
            return (h & 0xffffff) < m_techniques.size() ? int(h & 0xffffff) : -1;
        if (!h || (h & 0xF0000000) == 0xF0000000)
            return -1;
        const std::string name = guestString(h);
        for (size_t i = 0; i < m_techniques.size(); ++i)
            if (m_techniques[i].name == name)
                return int(i);
        return -1;
    }
    FxPass* pass(reg32 h)
    {
        if ((h & HandleTagMask) != HandlePass)
            return nullptr;
        const uint32_t t = (h >> 12) & 0xfff, p = h & 0xfff;
        return t < m_techniques.size() && p < m_techniques[t].passes.size() ? &m_techniques[t].passes[p] : nullptr;
    }
    // Annotation indices of a parameter, technique or pass handle.
    const std::vector<uint32_t>* annotationsOf(reg32 h)
    {
        if (FxPass* p = pass(h))
            return &p->annotations;
        if ((h & HandleTagMask) == HandleTechnique)
        {
            const int t = technique(h);
            return t >= 0 ? &m_techniques[size_t(t)].annotations : nullptr;
        }
        if (FxParam* p = param(h))
            return &p->annotations;
        const int t = technique(h);
        return t >= 0 ? &m_techniques[size_t(t)].annotations : nullptr;
    }

    // A shared parameter's value reaches the other effects of the pool.
    // (Run 65: the name search over every effect on each set took 7% of the game thread's time.)
    // Shared parameters live in their pool slot (FxParam::slot): nothing to copy.
    void shareParam(const FxParam&) {}
    static void setTexture(FxParam& p, Object* texture)
    {
        Object*& current = p.tex();
        if (current == texture)
            return;
        if (texture)
            texture->bind();
        if (current)
            current->unbind();
        current = texture;
    }

    Object* device() const              { return m_device; }
    Object* pool() const                { return m_pool; }
    const std::string& name() const     { return m_name; }
    FxFamily family() const             { return m_family; }
    uint16_t block() const              { return m_block; }

    // EffectPass: a draw of a family pass. Textures come from the device stages applyPass set.
    void record(EffectDraw& effect, const render::TextureStorage* textures[render::MaxSamplers]) const override
    {
        effect.own = m_block;
        effect.shared = NoBlock;
        effect.kind = m_family == FamilyScreen ? EffectKindCarbonScreen
                    : m_family == FamilyCar ? EffectKindCarbonCar
                    : m_family == FamilyMask ? EffectKindCarbonMask
                    : m_family == FamilyParticle ? EffectKindCarbonParticle
                    : m_skyProcedural ? EffectKindCarbonSky : EffectKindCarbonWorld;
        // Car family: the stage the diffuse map is on (s0 in IDI_CAR_FX technique 2, s1 in technique 3).
        effect.pass = m_family == FamilyCar ? m_diffuseStage : 0;
        effect.pad[0] = effect.pad[1] = 0;
        for (uint32_t i = 0; i < render::MaxSamplers; ++i)
            textures[i] = nullptr;
    }
    const char* effectName() const override { return m_name.c_str(); }
    void dump(const void* vertices, uint8_t layout) const override
    {
        float m[16], pos[3] = { 0, 0, 0 }, pb[4] = {};
        floatsOf("WorldViewProj", m, 16);
        floatsOf("cvVertexPowerBrightness", pb, 4);
        if (vertices)
            memcpy(pos, vertices, sizeof(pos));
        float clip[4];
        for (int c = 0; c < 4; ++c)
            clip[c] = pos[0] * m[c] + pos[1] * m[4 + c] + pos[2] * m[8 + c] + m[12 + c];
        const char* tech = m_technique < m_techniques.size() ? m_techniques[m_technique].name.c_str() : "?";
        pf::logInfo("dump:   effect %s technique %u %s, layout %u, vertex 0 (%.2f %.2f %.2f) -> clip (%.2f %.2f %.2f "
                    "%.2f), power/brightness %.2f %.2f", m_name.c_str(), unsigned(m_technique), tech, unsigned(layout),
                    pos[0], pos[1], pos[2], clip[0], clip[1], clip[2], clip[3], pb[0], pb[1]);
        pf::logInfo("dump:   WorldViewProj %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f %.3f %.3f / %.3f %.3f "
                    "%.3f %.3f", m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12],
                    m[13], m[14], m[15]);
    }
    void leaveDevice()
    {
        if (m_device && m_device->kind() == KindDevice)
        {
            Device* d = static_cast<Device*>(m_device);
            if (d->effectPass() == this)
                d->setEffectPass(nullptr);
        }
    }
    // The effects applyFamily tells apart by name (looked up once).
    FxName nameKind() const
    {
        if (m_nameKind == FxNameUnknown)
            m_nameKind = m_name == "IDI_SKY_FX" ? FxNameSky : m_name == "IDI_WORLDREFLECT_FX" ? FxNameWorldReflect
                       : m_name == "IDI_WORLDPRELIT_FX" ? FxNameWorldPrelit : FxNameOther;
        return m_nameKind;
    }
    // Floats of a numeric parameter (n of them, zero-filled), false when the effect has no such parameter.
    bool floatsOf(const char* name, float* out, uint32_t n) const
    {
        const int i = literalIndex(name);
        if (i < 0)
            return false;
        const std::vector<uint8_t>& v = m_params[size_t(i)].v();
        memset(out, 0, n * 4);
        memcpy(out, v.data(), std::min<size_t>(v.size(), n * 4));
        return true;
    }

    std::vector<FxParam>        m_params;
    std::vector<FxParam>        m_annotations;
    std::vector<FxTechnique>    m_techniques;
    std::map<uint32_t, FxObject> m_objects;
    uint32_t                    m_technique = 0;
    uint32_t                    m_pass = 0xffffffff;
    bool                        m_begun = false;
    reg32                       m_guestCreator = 0;
    bool                        m_skyProcedural = false;
    mutable FxName m_nameKind = FxNameUnknown;
    std::vector<std::pair<reg32, int>> m_nameIndices;       // param() cache for name handles
    mutable std::vector<std::pair<const char*, int>> m_literalIndices;     // literalIndex cache   // IDI_SKY_FX technique 0 open: csky programs
    uint8_t                     m_diffuseStage = 1;     // car family: stage of DIFFUSEMAP_SAMPLER in the open pass

private:
    Object*                     m_device;
    Object*                     m_pool;
    std::string                 m_name;
    FxFamily                    m_family;
    uint16_t                    m_block = NoBlock;
    static std::vector<FxEffect*> s_all;
    static uint32_t s_generation;       // changes when an effect is created or destroyed
};

std::vector<FxEffect*> FxEffect::s_all;
uint32_t FxEffect::s_generation = 0;

namespace
{

void readAnnotations(Reader& r, uint32_t count, FxEffect& fx, std::vector<uint32_t>& out)
{
    for (uint32_t i = 0; i < count && r.ok && i < 256; ++i)
    {
        const uint32_t typeOffset = r.u32(), valueOffset = r.u32();
        FxParam a;
        a.t = typeAt(r, typeOffset);
        readValue(r, a.t, valueOffset, a.value, nullptr);
        out.push_back(uint32_t(fx.m_annotations.size()));
        fx.m_annotations.push_back(std::move(a));
    }
}

}

bool FxEffect::parse(const uint8_t* blob, size_t size)
{
    if (size < 8)
        return false;
    uint32_t tag, start;
    memcpy(&tag, blob, 4);
    memcpy(&start, blob + 4, 4);
    if (tag != 0xFEFF0901)
        return false;
    Reader r{ blob + 8, size - 8 };
    r.p = start;
    const uint32_t paramCount = r.u32(), techniqueCount = r.u32();
    r.u32();
    r.u32();
    for (uint32_t i = 0; i < paramCount && r.ok && i < 4096; ++i)
    {
        FxParam p;
        const uint32_t typeOffset = r.u32(), valueOffset = r.u32();
        p.shared = (r.u32() & 1) != 0;
        readAnnotations(r, r.u32(), *this, p.annotations);
        p.t = typeAt(r, typeOffset);
        readValue(r, p.t, valueOffset, p.value, &p.sampler);
        if (p.value.size() < p.t.bytes() && p.t.cls != ClassObject)
            p.value.resize(p.t.bytes(), 0);
        m_params.push_back(std::move(p));
    }
    for (uint32_t i = 0; i < techniqueCount && r.ok && i < 256; ++i)
    {
        FxTechnique t;
        t.name = r.str(r.u32());
        const uint32_t annotations = r.u32(), passes = r.u32();
        readAnnotations(r, annotations, *this, t.annotations);
        for (uint32_t j = 0; j < passes && r.ok && j < 256; ++j)
        {
            FxPass p;
            p.name = r.str(r.u32());
            const uint32_t passAnnotations = r.u32(), states = r.u32();
            readAnnotations(r, passAnnotations, *this, p.annotations);
            for (uint32_t k = 0; k < states && r.ok && k < 1024; ++k)
                p.states.push_back(readState(r));
            t.passes.push_back(std::move(p));
        }
        m_techniques.push_back(std::move(t));
    }
    const uint32_t strings = r.u32(), resources = r.u32();
    for (uint32_t i = 0; i < strings && r.ok && i < 65536; ++i)
    {
        const uint32_t id = r.u32(), length = r.u32();
        m_objects[id].data = r.bytes(r.p, length);
        r.p += (length + 3) & ~3u;
    }
    for (uint32_t i = 0; i < resources && r.ok && i < 65536; ++i)
    {
        const uint32_t tech = r.u32(), index = r.u32();
        r.u32();                                            // element
        const uint32_t state = r.u32(), usage = r.u32(), length = r.u32();
        std::vector<uint8_t> data = r.bytes(r.p, length);
        r.p += (length + 3) & ~3u;
        FxState* s = nullptr;
        if (tech == 0xffffffff)
        {
            if (index < m_params.size() && state < m_params[index].sampler.size())
                s = &m_params[index].sampler[state];
        }
        else if (tech < m_techniques.size() && index < m_techniques[tech].passes.size()
                 && state < m_techniques[tech].passes[index].states.size())
        {
            s = &m_techniques[tech].passes[index].states[state];
        }
        if (!s)
            continue;
        if (s->t.cls != ClassObject)
        {
            if (usage == 0)
                s->preshader = std::move(data);     // a value computed from parameters
            continue;
        }
        FxObject& o = m_objects[s->object()];
        if (usage == 0)
            o.data = std::move(data);
        else if (usage == 1)
            o.param = std::string(reinterpret_cast<const char*>(data.data()), data.size()).c_str();
    }
    return r.ok;
}

void FxEffect::describe() const
{
    size_t passes = 0, shaders = 0;
    for (const FxTechnique& t : m_techniques)
        for (const FxPass& p : t.passes)
        {
            ++passes;
            for (const FxState& s : p.states)
                if ((s.op == StateVertexShader || s.op == StatePixelShader) && s.t.cls == ClassObject)
                {
                    const auto it = m_objects.find(s.object());
                    shaders += it != m_objects.end() && it->second.data.size() >= 4;
                }
        }
    pf::logInfo("d3dx9: effect %s: %u parameters, %u techniques, %u passes, %u shaders", m_name.c_str(),
                unsigned(m_params.size()), unsigned(m_techniques.size()), unsigned(passes), unsigned(shaders));
}

void FxEffect::joinPool()
{
    if (!m_pool)
        return;
    static std::map<std::pair<Object*, std::string>, SharedSlot*> s_slots;
    for (FxParam& p : m_params)
    {
        if (!p.shared)
            continue;
        SharedSlot*& slot = s_slots[std::make_pair(m_pool, p.t.name)];
        if (!slot)
        {
            // The first effect of the pool with the parameter gives its default value.
            slot = new SharedSlot();
            slot->value = p.value;
        }
        if (slot->value.size() != p.value.size())
            continue;       // same name, other type: keeps its own value
        p.slot = slot;
    }
}

// ---- Methods -------------------------------------------------------------------------------------------------------

namespace
{

FxEffect* self(const x86::CPU& cpu)
{
    Object* o = lookup(arg(cpu, 0));
    return o && o->kind() == KindFxEffect ? static_cast<FxEffect*>(o) : nullptr;
}

void logOnce(const char* method)
{
    static std::vector<std::string> seen;
    for (const std::string& s : seen)
        if (s == method)
            return;
    seen.push_back(method);
    pf::logError("d3dx9: ID3DXEffect::%s is not implemented (answers E_NOTIMPL)", method);
}

typedef reg32 (*FxMethod)(FxEffect& fx, const x86::CPU& cpu);

struct FxEntry
{
    const char* name;
    uint32_t    args;       // stack arguments including this
    FxMethod    method;     // null: E_NOTIMPL (logged once)
};

// ---- Descriptions and handles ----

reg32 GetDesc(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 out = arg(cpu, 1);
    if (!out)
        return D3DERR_INVALIDCALL;
    st(out + 0, keptString("NFSCVita fx_2_0", fx.m_guestCreator));
    st(out + 4, reg32(fx.m_params.size()));
    st(out + 8, reg32(fx.m_techniques.size()));
    st(out + 12, 0);
    return D3D_OK;
}

void writeParamDesc(FxParam& p, reg32 out)
{
    st(out + 0, keptString(p.t.name, p.guestName));
    st(out + 4, p.t.semantic.empty() ? 0 : keptString(p.t.semantic, p.guestSemantic));
    st(out + 8, p.t.cls);
    st(out + 12, p.t.type);
    st(out + 16, p.t.rows);
    st(out + 20, p.t.cols);
    st(out + 24, p.t.elements);
    st(out + 28, reg32(p.annotations.size()));
    st(out + 32, reg32(p.t.members.size()));
    st(out + 36, p.shared ? 1 : 0);
    st(out + 40, p.t.bytes());
}

reg32 GetParameterDesc(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 out = arg(cpu, 2);
    if (!p || !out)
        return D3DERR_INVALIDCALL;
    writeParamDesc(*p, out);
    return D3D_OK;
}

reg32 GetTechniqueDesc(FxEffect& fx, const x86::CPU& cpu)
{
    const int t = fx.technique(arg(cpu, 1));
    const reg32 out = arg(cpu, 2);
    if (t < 0 || !out)
        return D3DERR_INVALIDCALL;
    FxTechnique& tech = fx.m_techniques[size_t(t)];
    st(out + 0, keptString(tech.name, tech.guestName));
    st(out + 4, reg32(tech.passes.size()));
    st(out + 8, reg32(tech.annotations.size()));
    return D3D_OK;
}

reg32 GetPassDesc(FxEffect& fx, const x86::CPU& cpu)
{
    FxPass* p = fx.pass(arg(cpu, 1));
    const reg32 out = arg(cpu, 2);
    if (!p || !out)
        return D3DERR_INVALIDCALL;
    st(out + 0, keptString(p->name, p->guestName));
    st(out + 4, reg32(p->annotations.size()));
    // Shader functions: guest copies of the bytecode.
    for (int which = 0; which < 2; ++which)
    {
        reg32& slot = which ? p->guestPs : p->guestVs;
        if (!slot)
            for (const FxState& s : p->states)
                if (s.op == (which ? StatePixelShader : StateVertexShader) && s.t.cls == ClassObject)
                {
                    const auto it = fx.m_objects.find(s.object());
                    if (it != fx.m_objects.end() && it->second.data.size() >= 4)
                    {
                        slot = crt::allocate(reg32(it->second.data.size()));
                        if (slot)
                            memcpy(g_guestBase + slot, it->second.data.data(), it->second.data.size());
                    }
                }
        st(out + 8 + 4 * which, slot);
    }
    return D3D_OK;
}

reg32 GetParameter(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 parent = arg(cpu, 1), index = arg(cpu, 2);
    if (parent)
        return 0;                       // struct members: not used by Carbon so far
    return index < fx.m_params.size() ? HandleParam | index : 0;
}

reg32 GetParameterByName(FxEffect& fx, const x86::CPU& cpu)
{
    if (arg(cpu, 1))
        return 0;
    const int i = fx.paramIndex(guestString(arg(cpu, 2)));
    return i >= 0 ? HandleParam | reg32(i) : 0;
}

reg32 GetParameterBySemantic(FxEffect& fx, const x86::CPU& cpu)
{
    if (arg(cpu, 1))
        return 0;
    const std::string semantic = guestString(arg(cpu, 2));
    for (size_t i = 0; i < fx.m_params.size(); ++i)
        if (sameName(fx.m_params[i].t.semantic, semantic))
            return HandleParam | reg32(i);
    return 0;
}

reg32 GetTechnique(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 index = arg(cpu, 1);
    return index < fx.m_techniques.size() ? HandleTechnique | index : 0;
}

reg32 GetTechniqueByName(FxEffect& fx, const x86::CPU& cpu)
{
    const std::string name = guestString(arg(cpu, 1));
    for (size_t i = 0; i < fx.m_techniques.size(); ++i)
        if (fx.m_techniques[i].name == name)
            return HandleTechnique | reg32(i);
    return 0;
}

reg32 GetPass(FxEffect& fx, const x86::CPU& cpu)
{
    const int t = fx.technique(arg(cpu, 1));
    const reg32 index = arg(cpu, 2);
    if (t < 0 || index >= fx.m_techniques[size_t(t)].passes.size())
        return 0;
    return HandlePass | (reg32(t) << 12) | index;
}

reg32 GetPassByName(FxEffect& fx, const x86::CPU& cpu)
{
    const int t = fx.technique(arg(cpu, 1));
    if (t < 0)
        return 0;
    const std::string name = guestString(arg(cpu, 2));
    const std::vector<FxPass>& passes = fx.m_techniques[size_t(t)].passes;
    for (size_t i = 0; i < passes.size(); ++i)
        if (passes[i].name == name)
            return HandlePass | (reg32(t) << 12) | reg32(i);
    return 0;
}

reg32 GetFunction(FxEffect&, const x86::CPU&)
{
    return 0;
}

reg32 GetAnnotation(FxEffect& fx, const x86::CPU& cpu)
{
    const std::vector<uint32_t>* list = fx.annotationsOf(arg(cpu, 1));
    const reg32 index = arg(cpu, 2);
    return list && index < list->size() ? HandleAnnotation | (*list)[index] : 0;
}

reg32 GetAnnotationByName(FxEffect& fx, const x86::CPU& cpu)
{
    const std::vector<uint32_t>* list = fx.annotationsOf(arg(cpu, 1));
    if (!list)
        return 0;
    const std::string name = guestString(arg(cpu, 2));
    for (uint32_t a : *list)
        if (fx.m_annotations[a].t.name == name)
            return HandleAnnotation | a;
    return 0;
}

// ---- Values ----
// Numeric values are kept as the effect file stores them: 4 bytes per component, bool/int as integers, floats as
// floats. Matrices are kept row-major whatever their class (the renderer transposes for column-major constants).

uint32_t components(const FxParam& p)
{
    return p.t.numeric() ? uint32_t(p.v().size() / 4) : 0;
}

void putFloat(FxParam& p, uint32_t i, float f)
{
    if (i >= components(p))
        return;
    if (p.t.type == TypeFloat)
        memcpy(&p.v()[4 * i], &f, 4);
    else
    {
        const int32_t v = p.t.type == TypeBool ? (f != 0.0f) : int32_t(f);
        memcpy(&p.v()[4 * i], &v, 4);
    }
}

void putInt(FxParam& p, uint32_t i, int32_t n)
{
    if (i >= components(p))
        return;
    if (p.t.type == TypeFloat)
    {
        const float f = float(n);
        memcpy(&p.v()[4 * i], &f, 4);
    }
    else
    {
        const int32_t v = p.t.type == TypeBool ? (n != 0) : n;
        memcpy(&p.v()[4 * i], &v, 4);
    }
}

float getFloat(const FxParam& p, uint32_t i)
{
    if (i >= components(p))
        return 0.0f;
    if (p.t.type == TypeFloat)
    {
        float f;
        memcpy(&f, &p.v()[4 * i], 4);
        return f;
    }
    int32_t v;
    memcpy(&v, &p.v()[4 * i], 4);
    return float(v);
}

int32_t getInt(const FxParam& p, uint32_t i)
{
    if (i >= components(p))
        return 0;
    if (p.t.type == TypeFloat)
    {
        float f;
        memcpy(&f, &p.v()[4 * i], 4);
        return int32_t(f);
    }
    int32_t v;
    memcpy(&v, &p.v()[4 * i], 4);
    return v;
}

float asFloat(reg32 bits)
{
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

reg32 SetValue(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), bytes = arg(cpu, 3);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    if (p->t.texture())
    {
        FxEffect::setTexture(*p, lookup(ld(data)));
        fx.shareParam(*p);
        return D3D_OK;
    }
    const size_t n = std::min<size_t>(bytes, p->v().size());
    memcpy(p->v().data(), g_guestBase + data, n);
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetValue(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), bytes = arg(cpu, 3);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    if (p->t.texture())
    {
        if (p->tex())
            p->tex()->addRef();
        st(data, p->tex() ? p->tex()->guest() : 0);
        return D3D_OK;
    }
    memcpy(g_guestBase + data, p->v().data(), std::min<size_t>(bytes, p->v().size()));
    return D3D_OK;
}

reg32 SetBool(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p)
        return D3DERR_INVALIDCALL;
    putInt(*p, 0, arg(cpu, 2) != 0);
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetBool(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p || !arg(cpu, 2))
        return D3DERR_INVALIDCALL;
    st(arg(cpu, 2), getInt(*p, 0) != 0);
    return D3D_OK;
}

reg32 SetInt(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p)
        return D3DERR_INVALIDCALL;
    putInt(*p, 0, int32_t(arg(cpu, 2)));
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetInt(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p || !arg(cpu, 2))
        return D3DERR_INVALIDCALL;
    st(arg(cpu, 2), reg32(getInt(*p, 0)));
    return D3D_OK;
}

reg32 SetFloat(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p)
        return D3DERR_INVALIDCALL;
    putFloat(*p, 0, asFloat(arg(cpu, 2)));
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetFloat(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p || !arg(cpu, 2))
        return D3DERR_INVALIDCALL;
    const float f = getFloat(*p, 0);
    memcpy(g_guestBase + arg(cpu, 2), &f, 4);
    return D3D_OK;
}

// Arrays of `count` items of `width` components from guest memory (ints or floats).
reg32 setArray(FxEffect& fx, const x86::CPU& cpu, uint32_t width, bool ints, bool boolean)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), count = arg(cpu, 3);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < count * width; ++i)
    {
        const reg32 v = ld(data + 4 * i);
        if (ints)
            putInt(*p, i, boolean ? (v != 0) : int32_t(v));
        else
            putFloat(*p, i, asFloat(v));
    }
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 getArray(FxEffect& fx, const x86::CPU& cpu, uint32_t width, bool ints)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), count = arg(cpu, 3);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < count * width; ++i)
    {
        if (ints)
            st(data + 4 * i, reg32(getInt(*p, i)));
        else
        {
            const float f = getFloat(*p, i);
            memcpy(g_guestBase + data + 4 * i, &f, 4);
        }
    }
    return D3D_OK;
}

reg32 SetBoolArray(FxEffect& fx, const x86::CPU& cpu)   { return setArray(fx, cpu, 1, true, true); }
reg32 GetBoolArray(FxEffect& fx, const x86::CPU& cpu)   { return getArray(fx, cpu, 1, true); }
reg32 SetIntArray(FxEffect& fx, const x86::CPU& cpu)    { return setArray(fx, cpu, 1, true, false); }
reg32 GetIntArray(FxEffect& fx, const x86::CPU& cpu)    { return getArray(fx, cpu, 1, true); }
reg32 SetFloatArray(FxEffect& fx, const x86::CPU& cpu)  { return setArray(fx, cpu, 1, false, false); }
reg32 GetFloatArray(FxEffect& fx, const x86::CPU& cpu)  { return getArray(fx, cpu, 1, false); }

// A vector parameter takes as many components as it has (up to 4).
reg32 SetVector(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < 4; ++i)
        putFloat(*p, i, asFloat(ld(data + 4 * i)));
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetVector(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const float f = getFloat(*p, i);
        memcpy(g_guestBase + data + 4 * i, &f, 4);
    }
    return D3D_OK;
}

reg32 SetVectorArray(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p)
        return D3DERR_INVALIDCALL;
    // Vectors of an array are as wide as the parameter's columns.
    const uint32_t width = p->t.cols ? p->t.cols : 4;
    const reg32 data = arg(cpu, 2), count = arg(cpu, 3);
    if (!data)
        return D3DERR_INVALIDCALL;
    for (uint32_t v = 0; v < count; ++v)
        for (uint32_t c = 0; c < width; ++c)
            putFloat(*p, v * width + c, asFloat(ld(data + 16 * v + 4 * c)));
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetVectorArray(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), count = arg(cpu, 3);
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    const uint32_t width = p->t.cols ? p->t.cols : 4;
    for (uint32_t v = 0; v < count; ++v)
        for (uint32_t c = 0; c < 4; ++c)
        {
            const float f = c < width ? getFloat(*p, v * width + c) : 0.0f;
            memcpy(g_guestBase + data + 16 * v + 4 * c, &f, 4);
        }
    return D3D_OK;
}

// Matrix `m` (16 guest floats, row-major) into matrix element `element` of the parameter.
void putMatrix(FxParam& p, uint32_t element, reg32 m, bool transpose)
{
    const uint32_t rows = p.t.rows ? p.t.rows : 4, cols = p.t.cols ? p.t.cols : 4;
    const uint32_t base = element * rows * cols;
    // float4x4: straight copy (run 151: setMatrices took 2.2 % of the game thread through putFloat).
    std::vector<uint8_t>& v = p.v();
    if (p.t.type == TypeFloat && rows == 4 && cols == 4 && 4 * size_t(base) + 64 <= v.size())
    {
        float* to = reinterpret_cast<float*>(&v[4 * size_t(base)]);
        if (!transpose)
            memcpy(to, g_guestBase + m, 64);
        else
        {
            float in[16];
            memcpy(in, g_guestBase + m, 64);
            for (uint32_t r = 0; r < 4; ++r)
                for (uint32_t c = 0; c < 4; ++c)
                    to[r * 4 + c] = in[c * 4 + r];
        }
        return;
    }
    for (uint32_t r = 0; r < rows; ++r)
        for (uint32_t c = 0; c < cols; ++c)
        {
            const reg32 source = transpose ? m + 4 * (c * 4 + r) : m + 4 * (r * 4 + c);
            putFloat(p, base + r * cols + c, asFloat(ld(source)));
        }
}

void getMatrix(const FxParam& p, uint32_t element, reg32 m, bool transpose)
{
    const uint32_t rows = p.t.rows ? p.t.rows : 4, cols = p.t.cols ? p.t.cols : 4;
    const uint32_t base = element * rows * cols;
    for (uint32_t r = 0; r < 4; ++r)
        for (uint32_t c = 0; c < 4; ++c)
        {
            const float f = r < rows && c < cols ? getFloat(p, base + r * cols + c) : (r == c ? 1.0f : 0.0f);
            const reg32 target = transpose ? m + 4 * (c * 4 + r) : m + 4 * (r * 4 + c);
            memcpy(g_guestBase + target, &f, 4);
        }
}

reg32 setMatrices(FxEffect& fx, const x86::CPU& cpu, bool array, bool pointers, bool transpose)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), count = array ? arg(cpu, 3) : 1;
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < count; ++i)
        putMatrix(*p, i, pointers ? ld(data + 4 * i) : data + 64 * i, transpose);
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 getMatrices(FxEffect& fx, const x86::CPU& cpu, bool array, bool pointers, bool transpose)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 data = arg(cpu, 2), count = array ? arg(cpu, 3) : 1;
    if (!p || !data)
        return D3DERR_INVALIDCALL;
    for (uint32_t i = 0; i < count; ++i)
        getMatrix(*p, i, pointers ? ld(data + 4 * i) : data + 64 * i, transpose);
    return D3D_OK;
}

reg32 SetMatrix(FxEffect& fx, const x86::CPU& cpu)                 { return setMatrices(fx, cpu, false, false, false); }
reg32 GetMatrix(FxEffect& fx, const x86::CPU& cpu)                 { return getMatrices(fx, cpu, false, false, false); }
reg32 SetMatrixArray(FxEffect& fx, const x86::CPU& cpu)            { return setMatrices(fx, cpu, true, false, false); }
reg32 GetMatrixArray(FxEffect& fx, const x86::CPU& cpu)            { return getMatrices(fx, cpu, true, false, false); }
reg32 SetMatrixPointerArray(FxEffect& fx, const x86::CPU& cpu)     { return setMatrices(fx, cpu, true, true, false); }
reg32 GetMatrixPointerArray(FxEffect& fx, const x86::CPU& cpu)     { return getMatrices(fx, cpu, true, true, false); }
reg32 SetMatrixTranspose(FxEffect& fx, const x86::CPU& cpu)        { return setMatrices(fx, cpu, false, false, true); }
reg32 GetMatrixTranspose(FxEffect& fx, const x86::CPU& cpu)        { return getMatrices(fx, cpu, false, false, true); }
reg32 SetMatrixTransposeArray(FxEffect& fx, const x86::CPU& cpu)   { return setMatrices(fx, cpu, true, false, true); }
reg32 GetMatrixTransposeArray(FxEffect& fx, const x86::CPU& cpu)   { return getMatrices(fx, cpu, true, false, true); }
reg32 SetMatrixTransposePointerArray(FxEffect& fx, const x86::CPU& cpu) { return setMatrices(fx, cpu, true, true, true); }
reg32 GetMatrixTransposePointerArray(FxEffect& fx, const x86::CPU& cpu) { return getMatrices(fx, cpu, true, true, true); }

reg32 SetTexture(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    if (!p)
        return D3DERR_INVALIDCALL;
    const reg32 texture = arg(cpu, 2);
    Object* o = texture ? lookup(texture) : nullptr;
    if (texture && !o)
        return D3DERR_INVALIDCALL;
    FxEffect::setTexture(*p, o);
    fx.shareParam(*p);
    return D3D_OK;
}

reg32 GetTexture(FxEffect& fx, const x86::CPU& cpu)
{
    FxParam* p = fx.param(arg(cpu, 1));
    const reg32 out = arg(cpu, 2);
    if (!p || !out)
        return D3DERR_INVALIDCALL;
    if (p->tex())
        p->tex()->addRef();
    st(out, p->tex() ? p->tex()->guest() : 0);
    return D3D_OK;
}

reg32 GetShader(FxEffect&, const x86::CPU& cpu)
{
    if (arg(cpu, 2))
        st(arg(cpu, 2), 0);             // no IDirect3D*Shader9 objects in the port
    return D3D_OK;
}

reg32 SetArrayRange(FxEffect&, const x86::CPU&)
{
    return D3D_OK;
}

// ---- Techniques and passes ----

reg32 GetPool(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 out = arg(cpu, 1);
    if (!out)
        return D3DERR_INVALIDCALL;
    if (fx.pool())
        fx.pool()->addRef();
    st(out, fx.pool() ? fx.pool()->guest() : 0);
    return D3D_OK;
}

reg32 SetTechnique(FxEffect& fx, const x86::CPU& cpu)
{
    const int t = fx.technique(arg(cpu, 1));
    if (t < 0)
        return D3DERR_INVALIDCALL;
    fx.m_technique = uint32_t(t);
    return D3D_OK;
}

reg32 GetCurrentTechnique(FxEffect& fx, const x86::CPU&)
{
    return HandleTechnique | fx.m_technique;
}

reg32 ValidateTechnique(FxEffect& fx, const x86::CPU& cpu)
{
    return fx.technique(arg(cpu, 1)) >= 0 ? D3D_OK : D3DERR_INVALIDCALL;
}

// Every technique counts as valid; the next one after the handle (0: from the first).
reg32 FindNextValidTechnique(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 out = arg(cpu, 2);
    if (!out)
        return D3DERR_INVALIDCALL;
    const int t = arg(cpu, 1) ? fx.technique(arg(cpu, 1)) : -1;
    const uint32_t next = uint32_t(t + 1);
    st(out, next < fx.m_techniques.size() ? HandleTechnique | next : 0);
    return next < fx.m_techniques.size() ? D3D_OK : 1;     // S_FALSE
}

reg32 IsParameterUsed(FxEffect&, const x86::CPU&)
{
    return 1;
}

reg32 Begin(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 out = arg(cpu, 1);
    const uint32_t passes = fx.m_technique < fx.m_techniques.size()
                          ? uint32_t(fx.m_techniques[fx.m_technique].passes.size()) : 0;
    if (out)
        st(out, passes);
    fx.m_begun = true;
    fx.m_pass = 0xffffffff;
    return D3D_OK;
}

// ---- Pass states on the device ----
// Until the shaders are translated, a pass's draws use the fixed-function path; the pass still sets what that path
// can use: its render and texture stage states, and the textures (and sampler states) of the samplers its pixel
// shader reads. State positions are those of Wine's d3dx9 effect.c state table (as the Mac port's kit uses them).

// Positions 0..102: the D3DRENDERSTATETYPE at each.
const uint8_t FxRenderState[103] = {
    7,   8,   9,   14,  15,  16,  19,  20,  22,  23,  24,  25,  26,  27,  28,  29,
    34,  35,  36,  37,  38,  48,  52,  53,  54,  55,  56,  57,  58,  59,  60,  128,
    129, 130, 131, 132, 133, 134, 135, 198, 199, 200, 201, 202, 203, 204, 205, 136,
    137, 139, 140, 141, 142, 143, 145, 146, 147, 148, 151, 152, 154, 155, 166, 156,
    157, 158, 159, 160, 161, 162, 163, 165, 167, 168, 170, 171, 172, 173, 174, 175,
    176, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192,
    193, 194, 195, 206, 207, 208, 209,
};
// Positions 103..120: the D3DTEXTURESTAGESTATETYPE of each.
const uint8_t FxStageState[18] = { 1, 26, 2, 3, 4, 27, 5, 6, 28, 7, 8, 9, 10, 11, 22, 23, 24, 32 };
const uint32_t FxOpPixelShader = 147;
const uint32_t FxOpSamplerTexture = 164;
const uint32_t FxOpSamplerFirst = 165;
const uint32_t FxOpSamplerLast = 177;

WinApplication* s_fxApp = nullptr;
// Setting fx_commit_light (default on): CommitChanges skips the pass's constant states.
bool s_fxCommitLight = true;

// The sampler registers (register set 3) of a shader's constant table: register -> name.
std::vector<std::pair<uint32_t, std::string>> shaderSamplers(const std::vector<uint8_t>& code)
{
    std::vector<std::pair<uint32_t, std::string>> out;
    Reader r{ code.data(), code.size() };
    const uint32_t token = r.at(4);
    if (!r.ok || (token & 0xffff) != 0xfffe || r.at(8) != 0x42415443)    // comment block holding "CTAB"
        return out;
    const size_t base = 12;
    const uint32_t count = r.at(base + 12), info = r.at(base + 16);
    for (uint32_t i = 0; i < count && i < 256 && r.ok; ++i)
    {
        const size_t c = base + info + 20 * i;
        const uint32_t nameOffset = r.at(c), w = r.at(c + 4);
        if ((w & 0xffff) != 3 || !r.ok)
            continue;
        const size_t start = base + nameOffset;
        size_t end = start;
        while (end < code.size() && code[end])
            ++end;
        if (start < code.size())
            out.emplace_back(w >> 16, std::string(reinterpret_cast<const char*>(code.data()) + start, end - start));
    }
    return out;
}

// The parameter an object state names (a texture assignment), or null.
FxParam* namedParam(FxEffect& fx, const FxState& s)
{
    auto it = fx.m_objects.find(s.object());
    if (it == fx.m_objects.end() || it->second.param.empty())
        return nullptr;
    if (it->second.paramIndex == -2)
        it->second.paramIndex = fx.paramIndex(it->second.param);
    const int i = it->second.paramIndex;
    return i >= 0 ? &fx.m_params[size_t(i)] : nullptr;
}

void applyState(FxEffect& fx, Device& device, x86::CPU& cpu, const FxState& s, uint32_t stage)
{
    if (s.t.cls == ClassObject || s.value.size() < 4)
        return;
    if (!s.preshader.empty())
    {
        D3D_LOG_ONCE("d3dx9: pass states computed by preshaders are not evaluated yet");
        return;
    }
    uint32_t v;
    memcpy(&v, s.value.data(), 4);
    if (s.op < 103)
        device.SetRenderState(s_fxApp, cpu, FxRenderState[s.op], v);
    else if (s.op < 121)
    {
        if (s.index < 8)
            device.SetTextureStageState(s_fxApp, cpu, s.index, FxStageState[s.op - 103], v);
    }
    else if (s.op >= FxOpSamplerFirst && s.op <= FxOpSamplerLast)
    {
        const uint32_t to = stage != 0xffffffff ? stage : s.index;
        if (to < 16)
            device.SetSamplerState(s_fxApp, cpu, to, s.op - FxOpSamplerFirst + 1, v);
    }
    RC_USE(fx);
}

}

const char* g_fxPassName = nullptr;
const char* g_fxPassTechnique = nullptr;

namespace
{

// A family pass: its values into the effect's block, and the effect becomes the device's pass for the draws.
void applyFamily(FxEffect& fx, Device& device)
{
    g_fxPassName = fx.name().c_str();
    g_fxPassTechnique = fx.m_technique < fx.m_techniques.size() ? fx.m_techniques[fx.m_technique].name.c_str() : "?";
    if (fx.family() == FamilyNone || fx.block() == NoBlock)
        return;
    if (fx.family() == FamilyScreen)
    {
        device.setEffectPass(&fx);
        D3D_LOG_ONCE("d3dx9: Carbon screen effects draw with cscreen_v / cscreen_f");
        return;
    }
    Device* d = &device;
    const uint16_t id = fx.block();
    if (fx.family() == FamilyMask)
    {
        float m[16];
        fx.floatsOf("WorldViewProj", m, 16);
        carbonBlockStore(d, id, CarbonWvp, m, 16);
        device.setEffectPass(&fx);
        D3D_LOG_ONCE("d3dx9: Carbon FE mask effect draws with cmask_v / cmask_f");
        return;
    }
    // IDI_SKY_FX technique 0 (vs_3_0 / ps_3_0, what the PC uses: work/pctrace): its own programs; technique 1
    // (vs_1_1) stays in the world family below.
    static const bool carbonSky = pf::Config::getBool("carbon_sky", true);
    const FxName kind = fx.nameKind();
    fx.m_skyProcedural = kind == FxNameSky && fx.m_technique == 0 && carbonSky;
    if (fx.m_skyProcedural)
    {
        float wvp[16], falloff[4], fog[4], pb[4], eye[4], time[4] = { 0, 0, 0, 0 };
        fx.floatsOf("WorldViewProj", wvp, 16);
        fx.floatsOf("cfSkyFogFalloff", falloff, 4);
        fx.floatsOf("cvFogColour", fog, 4);
        fx.floatsOf("cvVertexPowerBrightness", pb, 4);
        fx.floatsOf("cvLocalEyePos", eye, 3);
        eye[3] = 1.0f;
        fx.floatsOf("cfTimeTicker", time, 1);
        carbonBlockStore(d, id, CarbonWvp, wvp, 16);
        carbonBlockStore(d, id, CarbonTextureOffset, falloff, 4);
        carbonBlockStore(d, id, CarbonFragmentScale, fog, 4);
        carbonBlockStore(d, id, CarbonPowerBrightness, pb, 4);
        carbonBlockStore(d, id, CarbonLocalEyePos, eye, 4);
        carbonBlockStore(d, id, CarbonDiffuseMin, time, 4);
        device.setEffectPass(&fx);
        D3D_LOG_ONCE("d3dx9: Carbon sky technique 0 draws with csky_v / csky_f (setting carbon_sky)");
        return;
    }
    if (fx.family() == FamilyParticle)
    {
        // Techniques 0-2 (fuzzz, no_fuzzz): particles; 5-6: flares; 3-4 (streak_flares: motion streaks drawn with
        // the previous frame matrix too) keep the fixed-function stand-in.
        static const std::string none;
        const std::string& technique = fx.m_technique < fx.m_techniques.size()
                                     ? fx.m_techniques[fx.m_technique].name : none;
        if (technique == "streak_flares")
            return;
        const bool flare = technique == "flares";
        float wvp[16], worldView[16], offset[4] = { 0, 0, 0, 0 }, mode[4] = { 0, 0, 0, 0 };
        float ambient[4] = { 1, 1, 1, 1 };
        fx.floatsOf("WorldViewProj", wvp, 16);
        fx.floatsOf("cmWorldView", worldView, 16);
        if (!flare)
        {
            fx.floatsOf("cvTextureOffset", offset, 4);
            fx.floatsOf("ambient_colour", ambient, 3);
            // ps_1_1 constant registers hold [-1, 1].
            for (int i = 0; i < 3; ++i)
                ambient[i] = ambient[i] < -1.0f ? -1.0f : ambient[i] > 1.0f ? 1.0f : ambient[i];
            ambient[3] = 1.0f;
        }
        mode[0] = flare ? 1.0e9f : 0.65f;
        mode[1] = flare ? 1.0f : 2.0f;
        carbonBlockStore(d, id, CarbonWvp, wvp, 16);
        carbonBlockStore(d, id, CarbonWorldView, worldView, 16);
        carbonBlockStore(d, id, CarbonTextureOffset, offset, 4);
        carbonBlockStore(d, id, CarbonPowerBrightness, mode, 4);
        carbonBlockStore(d, id, CarbonFragmentScale, ambient, 4);
        device.setEffectPass(&fx);
        D3D_LOG_ONCE("d3dx9: Carbon particles and flares draw with cparticle_v / cparticle_f (setting carbon_particles)");
        return;
    }
    if (fx.family() == FamilyCar)
    {
        fx.m_diffuseStage = 1;
        if (fx.m_technique < fx.m_techniques.size() && fx.m_pass < fx.m_techniques[fx.m_technique].passes.size())
            for (const std::pair<uint32_t, std::string>& s : fx.m_techniques[fx.m_technique].passes[fx.m_pass].samplers)
                if (s.second == "DIFFUSEMAP_SAMPLER" && s.first < render::MaxSamplers)
                    fx.m_diffuseStage = uint8_t(s.first);
        float m[40];
        struct { const char* name; CarbonSlot slot; uint32_t n; } const params[] = {
            { "WorldViewProj", CarbonWvp, 16 }, { "cavHarmonicCoeff", CarbonHarmonic, 40 },
            { "cvLocalEyePos", CarbonLocalEyePos, 4 }, { "cvDiffuseMin", CarbonDiffuseMin, 4 },
            { "cvDiffuseRange", CarbonDiffuseRange, 4 }, { "cvPowers", CarbonPowers, 4 },
            { "cvClampAndScales", CarbonClampAndScales, 4 }, { "cvVinylScales", CarbonVinylScales, 4 },
            { "cvFlakes", CarbonTextureOffset, 4 } };
        for (const auto& p : params)
        {
            fx.floatsOf(p.name, m, p.n);
            carbonBlockStore(d, id, p.slot, m, p.n);
        }
        device.setEffectPass(&fx);
        D3D_LOG_ONCE("d3dx9: Carbon car effect draws with ccar_v / ccar_f");
        return;
    }
    float wvp[16], offset[4], pb[4], scale[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
    fx.floatsOf("WorldViewProj", wvp, 16);
    fx.floatsOf("cvTextureOffset", offset, 4);
    if (!fx.floatsOf("cvVertexPowerBrightness", pb, 4))
    {
        // IDI_WORLDREFLECT_FX (vs_3_0): pow(colour, 1.3) * 2; IDI_WORLDPRELIT_FX: colour, then mul_x2.
        const bool reflect = kind == FxNameWorldReflect;
        pb[0] = reflect ? 1.3f : 1.0f;
        pb[1] = reflect ? 2.0f : 1.0f;
        pb[2] = pb[3] = 0.0f;
        if (kind == FxNameWorldPrelit)
            scale[0] = 2.0f;
        // IDI_SKY_FX (vs_1_1): colour = (vertex rgb, 0.5).
        if (kind == FxNameSky)
        {
            pb[2] = 1.0f;
            pb[3] = 0.5f;
        }
    }
    carbonBlockStore(d, id, CarbonWvp, wvp, 16);
    carbonBlockStore(d, id, CarbonTextureOffset, offset, 4);
    carbonBlockStore(d, id, CarbonPowerBrightness, pb, 4);
    carbonBlockStore(d, id, CarbonFragmentScale, scale, 4);
    device.setEffectPass(&fx);
    D3D_LOG_ONCE("d3dx9: Carbon world effects draw with cworld_v / cworld_f (setting carbon_shaders)");
}

// full: BeginPass, every state of the pass. Otherwise CommitChanges: as D3DX, only what depends on parameters (the
// sampler textures and the family's constants); the pass's constant states are already on the device (run 151:
// re-applying them on every commit took ~5 % of the game thread).
void applyPass(FxEffect& fx, const x86::CPU& constCpu, bool full)
{
    if (!s_fxApp || !fx.device() || fx.device()->kind() != KindDevice || fx.m_technique >= fx.m_techniques.size())
        return;
    FxTechnique& technique = fx.m_techniques[fx.m_technique];
    if (fx.m_pass >= technique.passes.size())
        return;
    FxPass& pass = technique.passes[fx.m_pass];
    Device& device = *static_cast<Device*>(fx.device());
    x86::CPU& cpu = const_cast<x86::CPU&>(constCpu);
    if (!full && pass.samplersParsed && s_fxCommitLight)
    {
        for (const FxState& s : pass.states)
            if (s.op == FxOpSamplerTexture && s.index < 16)
            {
                if (FxParam* texture = namedParam(fx, s))
                    device.SetTexture(s_fxApp, cpu, s.index, texture->tex());
            }
        for (size_t k = 0; k < pass.samplers.size(); ++k)
        {
            const int i = pass.samplerParams[k];
            const uint32_t stage = pass.samplers[k].first;
            if (i < 0 || stage >= 16)
                continue;
            Object* texture = nullptr;
            for (const FxState& ss : fx.m_params[size_t(i)].sampler)
                if (ss.op == FxOpSamplerTexture)
                {
                    if (FxParam* t = namedParam(fx, ss))
                        texture = t->tex();
                }
            device.SetTexture(s_fxApp, cpu, stage, texture);
        }
        applyFamily(fx, device);
        return;
    }
    // Carbon sets stage 0 to DISABLE for its shader draws (run 28's dump: colour and alpha op 1), which the real
    // pixel shaders ignore; the fixed-function stand-in would then draw the vertex colour only (solid blocks for the
    // text). Until the shaders are translated, a pass draws texture x vertex colour.
    enum : DWORD { TssColorOp = 1, TssColorArg1 = 2, TssColorArg2 = 3, TssAlphaOp = 4, TssAlphaArg1 = 5,
                   TssAlphaArg2 = 6, TopModulate = 4, TaDiffuse = 0, TaTexture = 2 };
    device.SetTextureStageState(s_fxApp, cpu, 0, TssColorOp, TopModulate);
    device.SetTextureStageState(s_fxApp, cpu, 0, TssColorArg1, TaTexture);
    device.SetTextureStageState(s_fxApp, cpu, 0, TssColorArg2, TaDiffuse);
    device.SetTextureStageState(s_fxApp, cpu, 0, TssAlphaOp, TopModulate);
    device.SetTextureStageState(s_fxApp, cpu, 0, TssAlphaArg1, TaTexture);
    device.SetTextureStageState(s_fxApp, cpu, 0, TssAlphaArg2, TaDiffuse);
    for (const FxState& s : pass.states)
    {
        applyState(fx, device, cpu, s, 0xffffffff);
        if (s.op == FxOpSamplerTexture && s.index < 16)
        {
            if (FxParam* texture = namedParam(fx, s))
                device.SetTexture(s_fxApp, cpu, s.index, texture->tex());
        }
        if (s.op != FxOpPixelShader || s.t.cls != ClassObject)
            continue;
        auto code = fx.m_objects.find(s.object());
        if (code == fx.m_objects.end())
            continue;
        if (!pass.samplersParsed)
        {
            pass.samplers = shaderSamplers(code->second.data);
            pass.samplerParams.clear();
            for (const std::pair<uint32_t, std::string>& sampler : pass.samplers)
                pass.samplerParams.push_back(fx.paramIndex(sampler.second));
            pass.samplersParsed = true;
        }
        for (size_t k = 0; k < pass.samplers.size(); ++k)
        {
            const std::pair<uint32_t, std::string>& sampler = pass.samplers[k];
            const int i = pass.samplerParams[k];
            if (i < 0 || sampler.first >= 16)
                continue;
            FxParam& p = fx.m_params[size_t(i)];
            Object* texture = nullptr;
            for (const FxState& ss : p.sampler)
            {
                if (ss.op == FxOpSamplerTexture)
                {
                    if (FxParam* t = namedParam(fx, ss))
                        texture = t->tex();
                }
                else
                    applyState(fx, device, cpu, ss, sampler.first);
            }
            device.SetTexture(s_fxApp, cpu, sampler.first, texture);
        }
    }
    applyFamily(fx, device);
}

reg32 BeginPass(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 pass = arg(cpu, 1);
    if (fx.m_technique >= fx.m_techniques.size() || pass >= fx.m_techniques[fx.m_technique].passes.size())
        return D3DERR_INVALIDCALL;
    fx.m_pass = pass;
    D3D_LOG_ONCE("d3dx9: effect passes begin (no shader translation yet: draws use the fixed-function path with the "
                 "passes' states and sampler textures)");
    applyPass(fx, cpu, true);
    return D3D_OK;
}

reg32 CommitChanges(FxEffect& fx, const x86::CPU& cpu)
{
    applyPass(fx, cpu, false);
    return D3D_OK;
}

reg32 EndPass(FxEffect& fx, const x86::CPU&)
{
    fx.m_pass = 0xffffffff;
    fx.leaveDevice();
    g_fxPassName = nullptr;
    return D3D_OK;
}

reg32 End(FxEffect& fx, const x86::CPU&)
{
    fx.m_pass = 0xffffffff;
    fx.leaveDevice();
    g_fxPassName = nullptr;
    fx.m_begun = false;
    return D3D_OK;
}

reg32 GetDevice(FxEffect& fx, const x86::CPU& cpu)
{
    const reg32 out = arg(cpu, 1);
    if (!out || !fx.device())
        return D3DERR_INVALIDCALL;
    fx.device()->addRef();
    st(out, fx.device()->guest());
    return D3D_OK;
}

reg32 Ok(FxEffect&, const x86::CPU&)
{
    return D3D_OK;
}

// ID3DXBaseEffect + ID3DXEffect of d3dx9_30, in vtable order (after IUnknown's three).
const FxEntry s_entries[] = {
    { "GetDesc", 2, GetDesc }, { "GetParameterDesc", 3, GetParameterDesc },
    { "GetTechniqueDesc", 3, GetTechniqueDesc }, { "GetPassDesc", 3, GetPassDesc },
    { "GetFunctionDesc", 3, nullptr }, { "GetParameter", 3, GetParameter },
    { "GetParameterByName", 3, GetParameterByName }, { "GetParameterBySemantic", 3, GetParameterBySemantic },
    { "GetParameterElement", 3, nullptr }, { "GetTechnique", 2, GetTechnique },
    { "GetTechniqueByName", 2, GetTechniqueByName }, { "GetPass", 3, GetPass }, { "GetPassByName", 3, GetPassByName },
    { "GetFunction", 2, GetFunction }, { "GetFunctionByName", 2, GetFunction },
    { "GetAnnotation", 3, GetAnnotation }, { "GetAnnotationByName", 3, GetAnnotationByName },
    { "SetValue", 4, SetValue }, { "GetValue", 4, GetValue }, { "SetBool", 3, SetBool }, { "GetBool", 3, GetBool },
    { "SetBoolArray", 4, SetBoolArray }, { "GetBoolArray", 4, GetBoolArray }, { "SetInt", 3, SetInt },
    { "GetInt", 3, GetInt }, { "SetIntArray", 4, SetIntArray }, { "GetIntArray", 4, GetIntArray },
    { "SetFloat", 3, SetFloat }, { "GetFloat", 3, GetFloat }, { "SetFloatArray", 4, SetFloatArray },
    { "GetFloatArray", 4, GetFloatArray }, { "SetVector", 3, SetVector }, { "GetVector", 3, GetVector },
    { "SetVectorArray", 4, SetVectorArray }, { "GetVectorArray", 4, GetVectorArray },
    { "SetMatrix", 3, SetMatrix }, { "GetMatrix", 3, GetMatrix }, { "SetMatrixArray", 4, SetMatrixArray },
    { "GetMatrixArray", 4, GetMatrixArray }, { "SetMatrixPointerArray", 4, SetMatrixPointerArray },
    { "GetMatrixPointerArray", 4, GetMatrixPointerArray }, { "SetMatrixTranspose", 3, SetMatrixTranspose },
    { "GetMatrixTranspose", 3, GetMatrixTranspose }, { "SetMatrixTransposeArray", 4, SetMatrixTransposeArray },
    { "GetMatrixTransposeArray", 4, GetMatrixTransposeArray },
    { "SetMatrixTransposePointerArray", 4, SetMatrixTransposePointerArray },
    { "GetMatrixTransposePointerArray", 4, GetMatrixTransposePointerArray },
    { "SetString", 3, nullptr }, { "GetString", 3, nullptr }, { "SetTexture", 3, SetTexture },
    { "GetTexture", 3, GetTexture }, { "GetPixelShader", 3, GetShader }, { "GetVertexShader", 3, GetShader },
    { "SetArrayRange", 4, SetArrayRange },
    { "GetPool", 2, GetPool }, { "SetTechnique", 2, SetTechnique }, { "GetCurrentTechnique", 1, GetCurrentTechnique },
    { "ValidateTechnique", 2, ValidateTechnique }, { "FindNextValidTechnique", 3, FindNextValidTechnique },
    { "IsParameterUsed", 3, IsParameterUsed }, { "Begin", 3, Begin }, { "BeginPass", 2, BeginPass },
    { "CommitChanges", 1, CommitChanges }, { "EndPass", 1, EndPass }, { "End", 1, End },
    { "GetDevice", 2, GetDevice }, { "OnLostDevice", 1, Ok }, { "OnResetDevice", 1, Ok },
    { "SetStateManager", 2, nullptr }, { "GetStateManager", 2, nullptr }, { "BeginParameterBlock", 1, nullptr },
    { "EndParameterBlock", 1, nullptr }, { "ApplyParameterBlock", 2, nullptr },
    { "DeleteParameterBlock", 2, nullptr }, { "CloneEffect", 3, nullptr }, { "SetRawValue", 5, nullptr },
};
const uint32_t EntryCount = sizeof(s_entries) / sizeof(s_entries[0]);
static_assert(sizeof(s_entries) / sizeof(s_entries[0]) == 76, "ID3DXEffect of d3dx9_30: 79 slots with IUnknown");

// The guest-callable method of entry I: stdcall, pops the return address and its arguments.
template< uint32_t I >
void call(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const FxEntry& e = s_entries[I];
    FxEffect* fx = self(cpu);
    if (!fx)
        cpu.eax = D3DERR_INVALIDCALL;
    else if (!e.method)
    {
        logOnce(e.name);
        cpu.eax = E_NOTIMPL_;
    }
    else
        cpu.eax = e.method(*fx, cpu);
    cpu.esp += 4 + 4 * e.args;
}

template< uint32_t... I >
struct Indices {};
template< uint32_t N, uint32_t... I >
struct MakeIndices : MakeIndices< N - 1, N - 1, I... > {};
template< uint32_t... I >
struct MakeIndices< 0, I... > { typedef Indices< I... > type; };

template< uint32_t... I >
void buildTable(WinApplication* app, Indices< I... >)
{
    static const MethodEntry table[] = {
        D3D_NAMED("QueryInterface", Object, QueryInterface), D3D_NAMED("AddRef", Object, AddRef),
        D3D_NAMED("Release", Object, Release),
        { s_entries[I].name, &call< I > }...
    };
    buildInterface(app, KindFxEffect, "ID3DXEffect", table, sizeof(table) / sizeof(table[0]));
}

}

void initFxInterfaces(WinApplication* app)
{
    s_fxApp = app;
    s_fxCommitLight = pf::Config::getBool("fx_commit_light", true);
    buildTable(app, MakeIndices< EntryCount >::type());
}

HRESULT createFxEffect(WinApplication* app, x86::CPU& cpu, reg32 device, reg32 resource, reg32 pool, reg32* effect)
{
    initInterfaces(app);
    const std::string name = resource > 0xffff ? guestString(resource, 63) : "#" + std::to_string(resource);
    // RT_RCDATA resource of NFSC.exe (the only module with effects).
    const reg32 info = kernel32::FindResourceA(app, cpu, 0, resource, 10);
    const reg32 data = info ? kernel32::LoadResource(app, cpu, 0, info) : 0;
    const reg32 size = info ? kernel32::SizeofResource(app, cpu, 0, info) : 0;
    if (!data || !size)
    {
        pf::logError("d3dx9: effect %s: no RCDATA resource of that name", name.c_str());
        return E_FAIL;
    }
    Object* d = lookup(device);
    Object* p = pool ? lookup(pool) : nullptr;
    FxEffect* fx = new FxEffect(d, p && p->kind() == KindEffectPool ? p : nullptr, name);
    if (!fx->parse(g_guestBase + data, size))
    {
        pf::logError("d3dx9: effect %s: %u bytes do not parse as fx_2_0", name.c_str(), unsigned(size));
        delete fx;
        return E_FAIL;
    }
    fx->joinPool();
    fx->describe();
    *effect = publish(app, fx);
    return D3D_OK;
}

}}
