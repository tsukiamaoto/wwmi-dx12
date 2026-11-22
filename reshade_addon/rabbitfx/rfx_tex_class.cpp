// The texture classes RabbitFX's SetTextures shader works by. See rfx_tex_class.h.
#include "rfx_tex_class.h"

int RfxTexClass(uint32_t fmt, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr)
{
    (void)arr;
    const bool square = (w == h);
    const bool bc7 = (fmt >= 94 && fmt <= 99);
    const bool bgra = (fmt >= 87 && fmt <= 91);
    if (bc7 && square && h > 1 && mips > 10) return 3; // [TextureOverrideDiffuseNormalTexture] (1718.3)
    if (bgra && square && h < 512 && mips > 1) return 3; // [TextureOverride<Name>SpecialTexture] (1718.3)
    if (fmt == 61 && square && h > 1 && mips > 1) return 4; // [TextureOverrideMaterialmap] (1718.4)
    if (bc7 && square && mips == 1) return 4; // [TextureOverrideMaterialmapHair] (1718.4, overrides LOD when mips==1)
    if (bc7 && square && h > 1 && h < 513 && mips > 1 && mips < 11) return 6; // [TextureOverrideDiffuseNormalTextureLOD] (1718.6)
    if (bgra && square && h == 1 && mips == 1) return 5; // [TextureOverrideMagicDot] (1718.5)
    return 0;
}

bool RfxSetTexturesDecision(const int cls[4], bool have_d, bool have_n, bool have_l, bool have_m, uint8_t& d_mask,
    uint8_t& n_mask, uint8_t& l_mask, uint8_t& m_mask, char flags_out[8])
{
    d_mask = n_mask = l_mask = m_mask = 0;
    // Matches RabbitFX [CommandListSetTextures]: only treat 1718.6 (LOD) textures as Diffuse/Normal
    // when t0 or t1 is itself a 1718.6 LOD texture; otherwise 256x256/512x512 BC7 helper maps (e.g.
    // hair mask at t2) stay distinct from 1718.3 full-res Diffuse/Normal maps.
    const int target = (cls[0] == 6 || cls[1] == 6) ? 6 : 3;
    const bool x = cls[0] == target;
    bool y = cls[1] == target;
    bool z = cls[2] == target;
    const bool w = !z && cls[2] == 4;
    if (cls[1] == 5) {
        if (cls[2] == target)
            y = true;
        else if (cls[3] == target)
            z = true;
    }
    const bool more = ((int)x + (int)y + (int)z + (int)w) > 1; // treg3.x: the flags sum past 1
    int f = 0;
    if (x) flags_out[f++] = 'x';
    if (y) flags_out[f++] = 'y';
    if (z) flags_out[f++] = 'z';
    if (w) flags_out[f++] = 'w';
    flags_out[f] = 0;
    // 1st sample (t0)
    if (x) {
        if (more) {
            if (have_n) n_mask |= 1;
        } else if (have_d)
            d_mask |= 1;
    } else if (have_l)
        l_mask |= 1;
    // 2nd sample (t1)
    if (y) {
        if (have_d) d_mask |= 2;
    } else if (have_l)
        l_mask |= 2;
    // 3rd sample (t2)
    if (z) {
        if (have_d) d_mask |= 4;
    } else if (w) {
        if (have_m)
            m_mask |= 4;
        else if (have_l)
            l_mask |= 4;
    }
    if (w) {
        if (more) {
            if (have_d) d_mask |= 8;
        } else if (have_m)
            m_mask |= 8;
    } else if (more && !x) {
        if (have_n) n_mask |= 8;
    }
    return (d_mask | n_mask | l_mask | m_mask) != 0;
}
