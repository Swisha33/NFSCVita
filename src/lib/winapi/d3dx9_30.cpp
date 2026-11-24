// d3dx9_30.dll for NFSC.exe. Matrices are 16 floats row by row (D3DX's row-vector convention: v * M).

#include <winapi/d3dx9_30.h>
#include <winapi/d3dx9.h>
#include <winapi/d3d9_internal.h>
#include <winapi/d3d9_native.h>
#include "crt.h"

#include <platform/log.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <cstring>
#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace win32 { namespace d3dx9_30
{

using namespace crt;

namespace
{

const HRESULT E_FAIL = 0x80004005;

void load(Ptr address, float* values, unsigned count)
{
    memcpy(values, ptr(address), 4 * count);
}

void store(Ptr address, const float* values, unsigned count)
{
    memcpy(ptr(address), values, 4 * count);
}

// r = a * b. NEON: the scalar VFP form took 2.6 % of the game thread (run 151: the game multiplies thousands of
// matrices a frame). b is loaded first and each row of a before its row of r is written, so r may be a or b.
void multiply(const float* a, const float* b, float* r)
{
#ifdef __ARM_NEON
    const float32x4_t b0 = vld1q_f32(b), b1 = vld1q_f32(b + 4), b2 = vld1q_f32(b + 8), b3 = vld1q_f32(b + 12);
    for (int i = 0; i < 4; ++i)
    {
        const float32x4_t row = vld1q_f32(a + 4 * i);
        float32x4_t v = vmulq_lane_f32(b0, vget_low_f32(row), 0);
        v = vmlaq_lane_f32(v, b1, vget_low_f32(row), 1);
        v = vmlaq_lane_f32(v, b2, vget_high_f32(row), 0);
        v = vmlaq_lane_f32(v, b3, vget_high_f32(row), 1);
        vst1q_f32(r + 4 * i, v);
    }
    return;
#endif
    float t[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            t[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
    memcpy(r, t, sizeof(t));
}

// Logs a not yet supported call once per name.
HRESULT unsupported(const char* name, HRESULT result)
{
    static std::string logged;
    if (logged.find(name) == std::string::npos)
    {
        logged += name;
        pf::logError("d3dx9_30: %s not supported yet", name);
    }
    return result;
}

}

Ptr D3DXMatrixMultiply(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr a, Ptr b)
{
    RC_USE(app);
    RC_USE(cpu);
    multiply(reinterpret_cast<const float*>(ptr(a)), reinterpret_cast<const float*>(ptr(b)),
             reinterpret_cast<float*>(ptr(out)));
    return out;
}

Ptr D3DXMatrixTranspose(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr m)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[16], r[16];
    load(m, in, 16);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r[i * 4 + j] = in[j * 4 + i];
    store(out, r, 16);
    return out;
}

// Inverse by cofactors; null when the matrix is singular (as D3DX).
Ptr D3DXMatrixInverse(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr determinant, Ptr m)
{
    RC_USE(app);
    RC_USE(cpu);
    float a[16], inv[16];
    load(m, a, 16);
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const float det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (determinant)
        store(determinant, &det, 1);
    if (det == 0.0f)
        return 0;
    for (float& v : inv)
        v /= det;
    store(out, inv, 16);
    return out;
}

Ptr D3DXMatrixPerspectiveLH(WinApplication* app, x86::CPU& cpu, Ptr out, float w, float h, float zn, float zf)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 2 * zn / w, 0, 0, 0,  0, 2 * zn / h, 0, 0,  0, 0, zf / (zf - zn), 1,
                          0, 0, zn * zf / (zn - zf), 0 };
    store(out, r, 16);
    return out;
}

Ptr D3DXMatrixOrthoLH(WinApplication* app, x86::CPU& cpu, Ptr out, float w, float h, float zn, float zf)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 2 / w, 0, 0, 0,  0, 2 / h, 0, 0,  0, 0, 1 / (zf - zn), 0,  0, 0, zn / (zn - zf), 1 };
    store(out, r, 16);
    return out;
}

Ptr D3DXMatrixTranslation(WinApplication* app, x86::CPU& cpu, Ptr out, float x, float y, float z)
{
    RC_USE(app);
    RC_USE(cpu);
    const float r[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  x, y, z, 1 };
    store(out, r, 16);
    return out;
}

#ifdef __ARM_NEON
// v * m on guest memory (run 428: the copies through local arrays and the scalar form, 0.7 % of the game thread).
// Everything is loaded before the result is stored: out may be v or m.
static inline float32x4_t transformNeon(float32x4_t v, const float* m)
{
    float32x4_t r = vmulq_lane_f32(vld1q_f32(m), vget_low_f32(v), 0);
    r = vmlaq_lane_f32(r, vld1q_f32(m + 4), vget_low_f32(v), 1);
    r = vmlaq_lane_f32(r, vld1q_f32(m + 8), vget_high_f32(v), 0);
    return vmlaq_lane_f32(r, vld1q_f32(m + 12), vget_high_f32(v), 1);
}

static inline float32x4_t loadVec3(Ptr v, float w)
{
    const float* p = reinterpret_cast<const float*>(ptr(v));
    return vcombine_f32(vld1_f32(p), vset_lane_f32(w, vld1_dup_f32(p + 2), 1));
}
#endif

Ptr D3DXVec3Transform(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m)
{
    RC_USE(app);
    RC_USE(cpu);
#ifdef __ARM_NEON
    vst1q_f32(reinterpret_cast<float*>(ptr(out)),
              transformNeon(loadVec3(v, 1.0f), reinterpret_cast<const float*>(ptr(m))));
    return out;
#endif
    float in[4] = { 0, 0, 0, 1 }, mm[16], r[4];
    load(v, in, 3);
    load(m, mm, 16);
    d3dx9::transform(in, mm, r);
    store(out, r, 4);
    return out;
}

Ptr D3DXVec4Transform(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m)
{
    RC_USE(app);
    RC_USE(cpu);
#ifdef __ARM_NEON
    vst1q_f32(reinterpret_cast<float*>(ptr(out)), transformNeon(vld1q_f32(reinterpret_cast<const float*>(ptr(v))),
                                                                reinterpret_cast<const float*>(ptr(m))));
    return out;
#endif
    float in[4], mm[16], r[4];
    load(v, in, 4);
    load(m, mm, 16);
    d3dx9::transform(in, mm, r);
    store(out, r, 4);
    return out;
}

Ptr D3DXVec3TransformNormal(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m)
{
    RC_USE(app);
    RC_USE(cpu);
#ifdef __ARM_NEON
    const float32x4_t t = transformNeon(loadVec3(v, 0.0f), reinterpret_cast<const float*>(ptr(m)));
    float* o = reinterpret_cast<float*>(ptr(out));
    vst1_f32(o, vget_low_f32(t));
    vst1q_lane_f32(o + 2, t, 2);
    return out;
#endif
    float in[4] = { 0, 0, 0, 0 }, mm[16], r[4];
    load(v, in, 3);
    load(m, mm, 16);
    d3dx9::transform(in, mm, r);
    store(out, r, 3);
    return out;
}

// (x, y, z, 1) * m, divided by w; count vectors with their strides.
Ptr D3DXVec3TransformCoordArray(WinApplication* app, x86::CPU& cpu, Ptr out, UINT outStride, Ptr v, UINT vStride,
                                Ptr m, UINT count)
{
    RC_USE(app);
    RC_USE(cpu);
    float mm[16];
    load(m, mm, 16);
    for (UINT i = 0; i < count; ++i)
    {
        float in[4] = { 0, 0, 0, 1 }, r[4];
        load(v + i * vStride, in, 3);
        d3dx9::transform(in, mm, r);
        const float w = r[3] != 0.0f ? 1.0f / r[3] : 0.0f;
        const float coord[3] = { r[0] * w, r[1] * w, r[2] * w };
        store(out + i * outStride, coord, 3);
    }
    return out;
}

Ptr D3DXVec3Normalize(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v)
{
    RC_USE(app);
    RC_USE(cpu);
    float in[3];
    load(v, in, 3);
    const float length = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);
    const float scale = length > 0.0f ? 1.0f / length : 0.0f;
    const float r[3] = { in[0] * scale, in[1] * scale, in[2] * scale };
    store(out, r, 3);
    return out;
}

// UG1VITA's effect pool (d3dx9.cpp). Its entry pops its own stdcall frame; the import wrapper pops it again.
HRESULT D3DXCreateEffectPool(WinApplication* app, x86::CPU& cpu, Ptr pool)
{
    RC_USE(pool);
    const reg32 esp = cpu.esp;
    d3dx9::CreateEffectPool(app, cpu);
    cpu.esp = esp;
    return cpu.eax;
}

// Carbon's effects are fx_2_0 binaries in NFSC.exe's resources (IDI_*_FX): d3dx9_fx.cpp.
HRESULT D3DXCreateEffectFromResourceA(WinApplication* app, x86::CPU& cpu, x86::reg32 device, HMODULE module,
                                      Ptr resource, Ptr defines, Ptr include, DWORD flags, x86::reg32 pool,
                                      Ptr effect, Ptr errors)
{
    RC_USE(module);
    RC_USE(defines);
    RC_USE(include);
    RC_USE(flags);
    if (errors)
        st32(errors, 0);
    if (!effect)
        return d3d9::D3DERR_INVALIDCALL;
    st32(effect, 0);
    d3d9::bridge::drain();
    reg32 created = 0;
    const HRESULT result = d3d9::createFxEffect(app, cpu, device, resource, pool, &created);
    st32(effect, created);
    return result;
}

// ---- DDS files (GLOBALB.BUN chunks: 0070f6c0 five 256^2 DXT1 cube maps, 0070f670 a 32^3 A8R8G8B8 volume) ----------

namespace
{

struct Dds
{
    uint32_t width = 0, height = 0, depth = 1, levels = 1, format = 0, caps2 = 0;
    const uint8_t* data = nullptr;
    uint32_t size = 0;
};

enum : uint32_t
{
    DdsCubemap = 0x200, DdsVolume = 0x200000
};

uint32_t blockBytes(uint32_t format)
{
    switch (format)
    {
    case d3d9::D3DFMT_DXT1: return 8;
    case d3d9::D3DFMT_DXT3:
    case d3d9::D3DFMT_DXT5: return 16;
    default:                return 0;
    }
}

uint32_t texelBytes(uint32_t format)
{
    switch (format)
    {
    case d3d9::D3DFMT_A8R8G8B8:
    case d3d9::D3DFMT_X8R8G8B8: return 4;
    case d3d9::D3DFMT_A8:
    case d3d9::D3DFMT_L8:       return 1;
    default:                    return 0;
    }
}

// Bytes of one row (of blocks for DXT) of a level edge, as a DDS file packs it, and the number of rows.
uint32_t ddsRowBytes(uint32_t format, uint32_t width)
{
    if (const uint32_t b = blockBytes(format))
        return ((width + 3) / 4 ? (width + 3) / 4 : 1) * b;
    return width * texelBytes(format);
}

uint32_t ddsRows(uint32_t format, uint32_t height)
{
    return blockBytes(format) ? ((height + 3) / 4 ? (height + 3) / 4 : 1) : height;
}

bool parseDds(const uint8_t* p, uint32_t n, Dds& o)
{
    auto u32 = [p](uint32_t at) { uint32_t v; memcpy(&v, p + at, 4); return v; };
    if (n < 128 || memcmp(p, "DDS ", 4) != 0 || u32(4) != 124)
        return false;
    const uint32_t flags = u32(8), pfFlags = u32(80), fourcc = u32(84), bits = u32(88), rmask = u32(92),
                   amask = u32(104);
    o.height = u32(12);
    o.width = u32(16);
    o.depth = (flags & 0x800000) && u32(24) ? u32(24) : 1;     // DDSD_DEPTH
    o.levels = (flags & 0x20000) && u32(28) ? u32(28) : 1;     // DDSD_MIPMAPCOUNT
    o.caps2 = u32(112);
    if (pfFlags & 0x4)                                          // DDPF_FOURCC
    {
        if (!blockBytes(fourcc))
            return false;
        o.format = fourcc;
    }
    else if ((pfFlags & 0x40) && bits == 32 && rmask == 0x00ff0000u)   // DDPF_RGB
        o.format = (pfFlags & 0x1) && amask == 0xff000000u ? d3d9::D3DFMT_A8R8G8B8 : d3d9::D3DFMT_X8R8G8B8;
    else if (bits == 8 && (pfFlags & 0x2))                      // DDPF_ALPHA
        o.format = d3d9::D3DFMT_A8;
    else if (bits == 8 && (pfFlags & 0x20000))                  // DDPF_LUMINANCE
        o.format = d3d9::D3DFMT_L8;
    else
        return false;
    if (!o.width || !o.height || !o.depth || o.levels > 16)
        return false;
    o.data = p + 128;
    o.size = n - 128;
    return true;
}

d3d9::Device* deviceOf(x86::reg32 device)
{
    d3d9::Object* o = d3d9::lookup(device);
    return o && o->kind() == d3d9::KindDevice ? static_cast<d3d9::Device*>(o) : nullptr;
}

}

HRESULT D3DXCreateCubeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, Ptr data,
                                              UINT size, Ptr texture)
{
    if (!texture)
        return d3d9::D3DERR_INVALIDCALL;
    st32(texture, 0);
    d3d9::bridge::drain();
    d3d9::Device* d = deviceOf(device);
    Dds dds;
    if (!d || !data || !parseDds(ptr<const uint8_t>(data), size, dds) || !(dds.caps2 & DdsCubemap)
        || dds.width != dds.height)
    {
        pf::logError("d3dx9_30: cube DDS not loaded (%u bytes: not a supported full cube)", size);
        return d3d9::D3DERR_INVALIDCALL;
    }
    reg32 created = 0;
    HRESULT result = d->CreateCubeTexture(app, cpu, dds.width, dds.levels, 0, dds.format, 1, &created, 0);
    if (result != d3d9::D3D_OK)
        return result;
    d3d9::Texture* t = static_cast<d3d9::Texture*>(d3d9::lookup(created));
    const uint32_t kept = t->desc().levels;
    uint32_t at = 0;
    for (uint32_t face = 0; face < 6; ++face)                  // +X -X +Y -Y +Z -Z, each with its mips
        for (uint32_t level = 0; level < dds.levels; ++level)
        {
            const uint32_t edge = dds.width >> level ? dds.width >> level : 1;
            const uint32_t row = ddsRowBytes(dds.format, edge), rows = ddsRows(dds.format, edge);
            if (at + row * rows > dds.size)
            {
                pf::logError("d3dx9_30: cube DDS truncated at face %u level %u", face, level);
                t->release();
                return d3d9::D3DERR_INVALIDCALL;
            }
            if (level < kept)
            {
                d3d9::D3DLOCKED_RECT locked;
                if (t->LockRectCube(app, cpu, face, level, &locked, nullptr, 0) != d3d9::D3D_OK)
                {
                    t->release();
                    return d3d9::D3DERR_INVALIDCALL;
                }
                const uint32_t copy = row < uint32_t(locked.Pitch) ? row : uint32_t(locked.Pitch);
                for (uint32_t r = 0; r < rows; ++r)
                    memcpy(ptr(locked.pBits + r * locked.Pitch), dds.data + at + r * row, copy);
                t->UnlockRectCube(app, cpu, face, level);
            }
            at += row * rows;
        }
    D3D_LOG_ONCE("d3dx9_30: cube DDS loaded (first: %u^2, %u levels, format 0x%x)", dds.width, dds.levels,
                 dds.format);
    st32(texture, created);
    return d3d9::D3D_OK;
}

HRESULT D3DXCreateVolumeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, Ptr data,
                                                UINT size, Ptr texture)
{
    if (!texture)
        return d3d9::D3DERR_INVALIDCALL;
    st32(texture, 0);
    d3d9::bridge::drain();
    d3d9::Device* d = deviceOf(device);
    Dds dds;
    if (!d || !data || !parseDds(ptr<const uint8_t>(data), size, dds) || !(dds.caps2 & DdsVolume)
        || !texelBytes(dds.format))
    {
        pf::logError("d3dx9_30: volume DDS not loaded (%u bytes: unsupported format or not a volume)", size);
        return d3d9::D3DERR_INVALIDCALL;
    }
    reg32 created = 0;
    HRESULT result = d->CreateVolumeTexture(app, cpu, dds.width, dds.height, dds.depth, dds.levels, 0, dds.format, 1,
                                            &created, 0);
    if (result != d3d9::D3D_OK)
        return result;
    d3d9::VolumeTexture* v = static_cast<d3d9::VolumeTexture*>(d3d9::lookup(created));
    uint32_t at = 0;
    for (uint32_t level = 0; level < v->levels(); ++level)
    {
        const uint32_t bytes = v->slicePitch(level) * v->depth(level);     // packed in the file as in the texture
        if (at + bytes > dds.size)
        {
            pf::logError("d3dx9_30: volume DDS truncated at level %u", level);
            v->release();
            return d3d9::D3DERR_INVALIDCALL;
        }
        memcpy(ptr(v->levelAddress(level)), dds.data + at, bytes);
        at += bytes;
    }
    pf::logInfo("d3dx9_30: volume DDS loaded: %ux%ux%u, %u levels, format %u", dds.width, dds.height, dds.depth,
                v->levels(), dds.format);
    st32(texture, created);
    return d3d9::D3D_OK;
}

// ---- D3DXLoadSurfaceFromSurface --------------------------------------------------------------------------------------
// Whole-surface copies between texture levels: decode the source to RGBA8, nearest-neighbour scale, colour key,
// encode the destination. Carbon's car vinyl skin is painted on the CPU into a 1024x1024 A8R8G8B8 texture and copied
// so into the car's DXT1 skin (007d7580 -> 00732ca0), transparent outside the vinyl (the Mac port's notes: DXT1 blocks
// with transparent pixels are written in 3-colour mode, as D3DX does). Unsupported, the skin kept garbage: the
// blue/black swirl over the car (run 94).
namespace
{

struct Rgba { uint8_t r, g, b, a; };

Rgba from565(uint16_t c)
{
    const uint8_t r = uint8_t((c >> 11) & 31), g = uint8_t((c >> 5) & 63), b = uint8_t(c & 31);
    return { uint8_t(r << 3 | r >> 2), uint8_t(g << 2 | g >> 4), uint8_t(b << 3 | b >> 2), 255 };
}

uint16_t to565(int r, int g, int b)
{
    auto clamp = [](int v) { return v < 0 ? 0 : v > 255 ? 255 : v; };
    r = clamp(r), g = clamp(g), b = clamp(b);
    return uint16_t(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

// Colours of a DXT colour block (4-colour when c0 > c1 or forced, as DXT3/DXT5 do).
void palette(const uint8_t* block, Rgba out[4], bool force4)
{
    const uint16_t c0 = uint16_t(block[0] | block[1] << 8), c1 = uint16_t(block[2] | block[3] << 8);
    out[0] = from565(c0);
    out[1] = from565(c1);
    if (c0 > c1 || force4)
    {
        out[2] = { uint8_t((2 * out[0].r + out[1].r) / 3), uint8_t((2 * out[0].g + out[1].g) / 3),
                   uint8_t((2 * out[0].b + out[1].b) / 3), 255 };
        out[3] = { uint8_t((out[0].r + 2 * out[1].r) / 3), uint8_t((out[0].g + 2 * out[1].g) / 3),
                   uint8_t((out[0].b + 2 * out[1].b) / 3), 255 };
    }
    else
    {
        out[2] = { uint8_t((out[0].r + out[1].r) / 2), uint8_t((out[0].g + out[1].g) / 2),
                   uint8_t((out[0].b + out[1].b) / 2), 255 };
        out[3] = { 0, 0, 0, 0 };
    }
}

bool decode(DWORD format, const uint8_t* bits, uint32_t pitch, uint32_t w, uint32_t h, std::vector<Rgba>& out)
{
    out.assign(size_t(w) * h, Rgba{ 0, 0, 0, 0 });
    switch (format)
    {
    case d3d9::D3DFMT_A8R8G8B8:
    case d3d9::D3DFMT_X8R8G8B8:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t* p = bits + size_t(y) * pitch + 4 * x;
                out[size_t(y) * w + x] = { p[2], p[1], p[0],
                                           format == d3d9::D3DFMT_A8R8G8B8 ? p[3] : uint8_t(255) };
            }
        return true;
    case d3d9::D3DFMT_A8:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                out[size_t(y) * w + x] = { 0, 0, 0, bits[size_t(y) * pitch + x] };
        return true;
    case d3d9::D3DFMT_L8:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint8_t l = bits[size_t(y) * pitch + x];
                out[size_t(y) * w + x] = { l, l, l, 255 };
            }
        return true;
    case d3d9::D3DFMT_DXT1:
    case d3d9::D3DFMT_DXT3:
    case d3d9::D3DFMT_DXT5:
    {
        const uint32_t blockBytes = format == d3d9::D3DFMT_DXT1 ? 8 : 16;
        for (uint32_t by = 0; by < (h + 3) / 4; ++by)
            for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
            {
                const uint8_t* block = bits + size_t(by) * pitch + size_t(bx) * blockBytes;
                const uint8_t* colour = format == d3d9::D3DFMT_DXT1 ? block : block + 8;
                Rgba pal[4];
                palette(colour, pal, format != d3d9::D3DFMT_DXT1);
                uint8_t alphas[8] = {};
                if (format == d3d9::D3DFMT_DXT5)
                {
                    alphas[0] = block[0], alphas[1] = block[1];
                    for (int i = 2; i < 8; ++i)
                        alphas[i] = alphas[0] > alphas[1]
                                  ? uint8_t(((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7)
                                  : i < 6 ? uint8_t(((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5)
                                  : i == 6 ? 0 : 255;
                }
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint32_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                    if (x >= w || y >= h)
                        continue;
                    Rgba c = pal[(colour[4 + i / 4] >> (2 * (i % 4))) & 3];
                    if (format == d3d9::D3DFMT_DXT3)
                        c.a = uint8_t(((block[i / 2] >> (4 * (i % 2))) & 15) * 17);
                    else if (format == d3d9::D3DFMT_DXT5)
                    {
                        const uint32_t bit = 3 * i;
                        const uint64_t indices = uint64_t(block[2]) | uint64_t(block[3]) << 8 |
                                                 uint64_t(block[4]) << 16 | uint64_t(block[5]) << 24 |
                                                 uint64_t(block[6]) << 32 | uint64_t(block[7]) << 40;
                        c.a = alphas[(indices >> bit) & 7];
                    }
                    out[size_t(y) * w + x] = c;
                }
            }
        return true;
    }
    default:
        return false;
    }
}

// One DXT1 colour block from 16 texels: endpoints are the extremes of the opaque texels along their largest colour
// spread; 3-colour mode with index 3 transparent when a texel is under half alpha.
void encodeDxt1(const Rgba texels[16], bool allowAlpha, uint8_t* block)
{
    bool transparent = false;
    int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 }, opaque = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (allowAlpha && texels[i].a < 128)
        {
            transparent = true;
            continue;
        }
        const int c[3] = { texels[i].r, texels[i].g, texels[i].b };
        for (int k = 0; k < 3; ++k)
            lo[k] = std::min(lo[k], c[k]), hi[k] = std::max(hi[k], c[k]);
        ++opaque;
    }
    if (!opaque)
    {
        // All transparent: c0 <= c1 and every index 3.
        block[0] = block[1] = block[2] = block[3] = 0;
        block[4] = block[5] = block[6] = block[7] = 0xff;
        return;
    }
    // Axis: the channel spread; endpoints: the opaque texels with the smallest and largest projection.
    const int axis[3] = { hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2] };
    int minDot = 1 << 30, maxDot = -(1 << 30), minI = 0, maxI = 0;
    for (int i = 0; i < 16; ++i)
    {
        if (allowAlpha && texels[i].a < 128)
            continue;
        const int d = texels[i].r * axis[0] + texels[i].g * axis[1] + texels[i].b * axis[2];
        if (d < minDot)
            minDot = d, minI = i;
        if (d > maxDot)
            maxDot = d, maxI = i;
    }
    uint16_t a = to565(texels[maxI].r, texels[maxI].g, texels[maxI].b);
    uint16_t b = to565(texels[minI].r, texels[minI].g, texels[minI].b);
    // 4-colour mode needs c0 > c1, 3-colour mode c0 <= c1.
    if (transparent ? a > b : a < b)
        std::swap(a, b);
    if (!transparent && a == b)
    {
        // One colour: 4-colour mode is impossible with equal endpoints; 3-colour mode shows it with index 0.
        block[0] = uint8_t(a), block[1] = uint8_t(a >> 8), block[2] = uint8_t(b), block[3] = uint8_t(b >> 8);
        block[4] = block[5] = block[6] = block[7] = 0;
        return;
    }
    block[0] = uint8_t(a), block[1] = uint8_t(a >> 8), block[2] = uint8_t(b), block[3] = uint8_t(b >> 8);
    Rgba pal[4];
    palette(block, pal, false);
    for (int row = 0; row < 4; ++row)
    {
        uint8_t bitsOut = 0;
        for (int col = 0; col < 4; ++col)
        {
            const Rgba& t = texels[row * 4 + col];
            int best = 0;
            if (transparent && t.a < 128)
                best = 3;
            else
            {
                int bestErr = 1 << 30;
                for (int k = 0; k < (transparent ? 3 : 4); ++k)
                {
                    const int dr = t.r - pal[k].r, dg = t.g - pal[k].g, db = t.b - pal[k].b;
                    const int err = dr * dr + dg * dg + db * db;
                    if (err < bestErr)
                        bestErr = err, best = k;
                }
            }
            bitsOut |= uint8_t(best << (2 * col));
        }
        block[4 + row] = bitsOut;
    }
}

bool encode(DWORD format, const std::vector<Rgba>& in, uint32_t w, uint32_t h, uint8_t* bits, uint32_t pitch,
            uint32_t* transparentBlocks)
{
    switch (format)
    {
    case d3d9::D3DFMT_A8R8G8B8:
    case d3d9::D3DFMT_X8R8G8B8:
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const Rgba& c = in[size_t(y) * w + x];
                uint8_t* p = bits + size_t(y) * pitch + 4 * x;
                p[0] = c.b, p[1] = c.g, p[2] = c.r, p[3] = format == d3d9::D3DFMT_A8R8G8B8 ? c.a : 255;
            }
        return true;
    case d3d9::D3DFMT_DXT1:
        for (uint32_t by = 0; by < (h + 3) / 4; ++by)
            for (uint32_t bx = 0; bx < (w + 3) / 4; ++bx)
            {
                Rgba texels[16];
                bool transparent = false;
                for (uint32_t i = 0; i < 16; ++i)
                {
                    const uint32_t x = std::min(bx * 4 + i % 4, w - 1), y = std::min(by * 4 + i / 4, h - 1);
                    texels[i] = in[size_t(y) * w + x];
                    transparent = transparent || texels[i].a < 128;
                }
                if (transparent)
                    ++*transparentBlocks;
                encodeDxt1(texels, true, bits + size_t(by) * pitch + size_t(bx) * 8);
            }
        return true;
    default:
        return false;
    }
}

d3d9::Surface* levelSurface(x86::reg32 guest)
{
    d3d9::Object* o = d3d9::lookup(guest);
    if (!o || o->kind() != d3d9::KindSurface)
        return nullptr;
    d3d9::Surface* s = static_cast<d3d9::Surface*>(o);
    return s->container() && !s->container()->desc().target ? s : nullptr;
}

}

HRESULT D3DXLoadSurfaceFromSurface(WinApplication* app, x86::CPU& cpu, x86::reg32 destination, Ptr destinationPalette,
                                   Ptr destinationRect, x86::reg32 source, Ptr sourcePalette, Ptr sourceRect,
                                   DWORD filter, DWORD colorKey)
{
    RC_USE(destinationPalette);
    RC_USE(sourcePalette);
    RC_USE(filter);
    if (destinationRect || sourceRect)
        return unsupported("D3DXLoadSurfaceFromSurface with rectangles", E_FAIL);
    d3d9::Surface* dst = levelSurface(destination);
    d3d9::Surface* src = levelSurface(source);
    if (!dst || !src)
        return unsupported("D3DXLoadSurfaceFromSurface of a render target or non-texture surface", E_FAIL);
    d3d9::bridge::drain();
    const DWORD srcFormat = src->container()->format(), dstFormat = dst->container()->format();
    const uint32_t sw = src->width(), sh = src->height(), dw = dst->width(), dh = dst->height();
    d3d9::D3DLOCKED_RECT in = {}, out = {};
    if (src->LockRect(app, cpu, &in, nullptr, d3d9::D3DLOCK_READONLY) != d3d9::D3D_OK)
        return E_FAIL;
    std::vector<Rgba> texels;
    const bool decoded = decode(srcFormat, reinterpret_cast<const uint8_t*>(ptr(in.pBits)), uint32_t(in.Pitch), sw, sh,
                                texels);
    src->UnlockRect(app, cpu);
    if (!decoded)
    {
        pf::logError("d3dx9_30: D3DXLoadSurfaceFromSurface: source format 0x%08x not supported", unsigned(srcFormat));
        return E_FAIL;
    }
    if (sw != dw || sh != dh)
    {
        std::vector<Rgba> scaled(size_t(dw) * dh);
        for (uint32_t y = 0; y < dh; ++y)
            for (uint32_t x = 0; x < dw; ++x)
                scaled[size_t(y) * dw + x] = texels[size_t(y * sh / dh) * sw + x * sw / dw];
        texels.swap(scaled);
    }
    if (colorKey)
    {
        const Rgba key = { uint8_t(colorKey >> 16), uint8_t(colorKey >> 8), uint8_t(colorKey), uint8_t(colorKey >> 24) };
        for (Rgba& t : texels)
            if (t.r == key.r && t.g == key.g && t.b == key.b && t.a == key.a)
                t = Rgba{ 0, 0, 0, 0 };
    }
    if (dst->LockRect(app, cpu, &out, nullptr, 0) != d3d9::D3D_OK)
        return E_FAIL;
    uint32_t transparentBlocks = 0;
    const bool encoded = encode(dstFormat, texels, dw, dh, reinterpret_cast<uint8_t*>(ptr(out.pBits)), uint32_t(out.Pitch),
                                &transparentBlocks);
    dst->UnlockRect(app, cpu);
    if (!encoded)
    {
        pf::logError("d3dx9_30: D3DXLoadSurfaceFromSurface: destination format 0x%08x not supported",
                     unsigned(dstFormat));
        return E_FAIL;
    }
    static uint32_t logged = 0;
    if (logged++ < 8)
        pf::logInfo("d3dx9_30: D3DXLoadSurfaceFromSurface %ux%u 0x%08x -> %ux%u 0x%08x (%u DXT1 blocks with "
                    "transparent texels, colour key 0x%08x)", unsigned(sw), unsigned(sh), unsigned(srcFormat),
                    unsigned(dw), unsigned(dh), unsigned(dstFormat), unsigned(transparentBlocks), unsigned(colorKey));
    return d3d9::D3D_OK;
}

// Every texel of every level from the guest's LPD3DXFILL3D (stdcall: out float4, texel-centre coordinate, texel
// size, user data). Carbon builds its colour-grading LUT so (0073c1a0 -> 0073c100); unfilled, it is black.
HRESULT D3DXFillVolumeTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 texture, x86::reg32 function, Ptr data)
{
    d3d9::bridge::drain();
    d3d9::Object* o = d3d9::lookup(texture);
    if (!o || o->kind() != d3d9::KindVolumeTexture || !function)
    {
        pf::logError("d3dx9_30: D3DXFillVolumeTexture(0x%08x, 0x%08x): not a volume texture", texture, function);
        return d3d9::D3DERR_INVALIDCALL;
    }
    d3d9::VolumeTexture* v = static_cast<d3d9::VolumeTexture*>(o);
    const reg32 scratch = allocate(48, true);
    if (!scratch)
        return E_FAIL;
    const reg32 out = scratch, coord = scratch + 16, texel = scratch + 32;
    auto u8 = [](float f) { f = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f; return uint32_t(f * 255.0f + 0.5f); };
    const DWORD format = v->format();
    const uint32_t bpp = d3d9::VolumeTexture::pixelBytes(format);
    for (uint32_t level = 0; level < v->levels(); ++level)
    {
        const uint32_t w = v->width(level), h = v->height(level), d = v->depth(level);
        const float size[3] = { 1.0f / float(w), 1.0f / float(h), 1.0f / float(d) };
        store(texel, size, 3);
        uint8_t* base = ptr<uint8_t>(v->levelAddress(level));
        for (uint32_t z = 0; z < d; ++z)
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const float c[3] = { (float(x) + 0.5f) / float(w), (float(y) + 0.5f) / float(h),
                                         (float(z) + 0.5f) / float(d) };
                    const float zero[4] = { 0, 0, 0, 0 };
                    store(coord, c, 3);
                    store(out, zero, 4);
                    callGuest(app, cpu, function, { out, coord, texel, data });
                    float rgba[4];
                    load(out, rgba, 4);
                    uint8_t* px = base + (size_t(z) * h + y) * v->rowPitch(level) + size_t(x) * bpp;
                    if (bpp == 4)
                    {
                        const uint32_t a = format == d3d9::D3DFMT_X8R8G8B8 ? 0xffu : u8(rgba[3]);
                        const uint32_t argb = a << 24 | u8(rgba[0]) << 16 | u8(rgba[1]) << 8 | u8(rgba[2]);
                        memcpy(px, &argb, 4);
                    }
                    else
                        *px = uint8_t(format == d3d9::D3DFMT_A8 ? u8(rgba[3]) : u8(rgba[0]));
                }
    }
    release(scratch);
    pf::logInfo("d3dx9_30: D3DXFillVolumeTexture filled %ux%ux%u (%u levels) with guest 0x%08x", v->width(0),
                v->height(0), v->depth(0), v->levels(), function);
    return d3d9::D3D_OK;
}

// Screenshots: not saved.
HRESULT D3DXSaveSurfaceToFileA(WinApplication* app, x86::CPU& cpu, Ptr file, DWORD format, x86::reg32 surface,
                               Ptr palette, Ptr rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(file);
    RC_USE(format);
    RC_USE(surface);
    RC_USE(palette);
    RC_USE(rect);
    return unsupported("D3DXSaveSurfaceToFileA", E_FAIL);
}

HRESULT D3DXSaveSurfaceToFileInMemory(WinApplication* app, x86::CPU& cpu, Ptr buffer, DWORD format,
                                      x86::reg32 surface, Ptr palette, Ptr rect)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(format);
    RC_USE(surface);
    RC_USE(palette);
    RC_USE(rect);
    if (buffer)
        st32(buffer, 0);
    return unsupported("D3DXSaveSurfaceToFileInMemory", E_FAIL);
}

}}
