// IDirect3DVolumeTexture9: levels in guest memory, no GPU copy yet (GXM has no 3D textures). Carbon creates one
// for its colour-grading LUT (0073c1a0, filled by D3DXFillVolumeTexture with callback 0073c100) and loads a 32^3
// DDS volume from GLOBALB.BUN (0070f670). The visual treatment needs the LUT as a 2D atlas later.

#include <winapi/d3d9_internal.h>
#include <platform/log.h>

namespace win32 { namespace d3d9
{

uint32_t VolumeTexture::pixelBytes(DWORD format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:   return 4;
    case D3DFMT_A8:
    case D3DFMT_L8:         return 1;
    default:                return 0;
    }
}

uint32_t VolumeTexture::fullChain(UINT width, UINT height, UINT depth)
{
    UINT largest = width > height ? width : height;
    largest = largest > depth ? largest : depth;
    uint32_t levels = 1;
    while (largest > 1)
    {
        largest >>= 1;
        ++levels;
    }
    return levels;
}

VolumeTexture::VolumeTexture(Device* device, UINT width, UINT height, UINT depth, UINT levels, DWORD format,
                             DWORD usage, DWORD pool)
    :   Object(KindVolumeTexture), m_device(device), m_width(width), m_height(height), m_depth(depth),
        m_levels(levels), m_format(format), m_usage(usage), m_pool(pool)
{
    uint32_t bytes = 0;
    for (uint32_t l = 0; l < m_levels; ++l)
    {
        m_offsets.push_back(bytes);
        bytes += slicePitch(l) * this->depth(l);
    }
    m_locked.assign(m_levels, false);
    m_memory = MemMap::tryCreate(bytes, MemMap::Low, pf::perf::Alloc(0));
    if (m_memory)
        memset(g_guestBase + m_memory->getBlockStart(), 0, bytes);
}

VolumeTexture::~VolumeTexture()
{
    delete m_memory;
}

x86::reg32 VolumeTexture::levelAddress(uint32_t level) const
{
    return m_memory->getBlockStart() + m_offsets[level];
}

HRESULT VolumeTexture::GetDevice(WinApplication* app, x86::CPU& cpu, x86::reg32* device)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!device)
        return D3DERR_INVALIDCALL;
    m_device->addRef();
    *device = m_device->guest();
    return D3D_OK;
}

DWORD VolumeTexture::SetPriority(WinApplication* app, x86::CPU& cpu, DWORD priority)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(priority);
    return 0;
}

DWORD VolumeTexture::GetPriority(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

void VolumeTexture::PreLoad(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
}

DWORD VolumeTexture::GetType(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return D3DRTYPE_VOLUMETEXTURE;
}

DWORD VolumeTexture::SetLOD(WinApplication* app, x86::CPU& cpu, DWORD lod)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lod);
    return 0;
}

DWORD VolumeTexture::GetLOD(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

DWORD VolumeTexture::GetLevelCount(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return m_levels;
}

HRESULT VolumeTexture::GetLevelDesc(WinApplication* app, x86::CPU& cpu, UINT level, D3DVOLUME_DESC* desc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (level >= m_levels || !desc)
        return D3DERR_INVALIDCALL;
    desc->Format = m_format;
    desc->Type = 2;             // D3DRTYPE_VOLUME
    desc->Usage = m_usage;
    desc->Pool = m_pool;
    desc->Width = width(level);
    desc->Height = height(level);
    desc->Depth = depth(level);
    return D3D_OK;
}

// No IDirect3DVolume9 objects: Carbon never asks for one (logged if it does).
HRESULT VolumeTexture::GetVolumeLevel(WinApplication* app, x86::CPU& cpu, UINT level, x86::reg32* volume)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::logError("d3d9: IDirect3DVolumeTexture9::GetVolumeLevel(%u) not supported", level);
    if (volume)
        *volume = 0;
    return D3DERR_NOTAVAILABLE;
}

HRESULT VolumeTexture::LockBox(WinApplication* app, x86::CPU& cpu, UINT level, D3DLOCKED_BOX* locked,
                               const D3DBOX* box, DWORD flags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(flags);
    if (level >= m_levels || !locked || m_locked[level])
        return D3DERR_INVALIDCALL;
    x86::reg32 bits = levelAddress(level);
    if (box)
    {
        if (box->Left >= box->Right || box->Right > width(level) || box->Top >= box->Bottom
            || box->Bottom > height(level) || box->Front >= box->Back || box->Back > depth(level))
            return D3DERR_INVALIDCALL;
        bits += box->Front * slicePitch(level) + box->Top * rowPitch(level) + box->Left * pixelBytes(m_format);
    }
    m_locked[level] = true;
    locked->RowPitch = LONG(rowPitch(level));
    locked->SlicePitch = LONG(slicePitch(level));
    locked->pBits = bits;
    return D3D_OK;
}

HRESULT VolumeTexture::UnlockBox(WinApplication* app, x86::CPU& cpu, UINT level)
{
    RC_USE(app);
    RC_USE(cpu);
    if (level >= m_levels || !m_locked[level])
        return D3DERR_INVALIDCALL;
    m_locked[level] = false;
    return D3D_OK;
}

HRESULT VolumeTexture::AddDirtyBox(WinApplication* app, x86::CPU& cpu, const D3DBOX* box)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(box);
    return D3D_OK;
}

HRESULT Device::CreateVolumeTexture(WinApplication* app, x86::CPU& cpu, UINT width, UINT height, UINT depth,
                                    UINT levels, DWORD usage, DWORD format, DWORD pool, x86::reg32* texture,
                                    x86::reg32 sharedHandle)
{
    RC_USE(cpu);
    RC_USE(sharedHandle);
    if (!texture || !width || !height || !depth || !VolumeTexture::pixelBytes(format)
        || (usage & D3DUSAGE_RENDERTARGET))
    {
        pf::logError("d3d9: CreateVolumeTexture %ux%ux%u format 0x%x usage 0x%x not supported", width, height, depth,
                     format, usage);
        return D3DERR_INVALIDCALL;
    }
    const uint32_t chain = VolumeTexture::fullChain(width, height, depth);
    VolumeTexture* t = new VolumeTexture(this, width, height, depth, levels == 0 || levels > chain ? chain : levels,
                                         format, usage, pool);
    if (!t->valid())
    {
        pf::logError("d3d9: out of guest memory for volume %ux%ux%u", width, height, depth);
        delete t;
        return D3DERR_OUTOFVIDEOMEMORY;
    }
    D3D_LOG_ONCE("d3d9: volume textures kept in guest memory, not sampled yet (first %ux%ux%u, %u levels, "
                 "format %u)", width, height, depth, t->levels(), format);
    *texture = publish(app, t);
    return D3D_OK;
}

}}
