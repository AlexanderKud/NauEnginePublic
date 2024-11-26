// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "basetexture.h"
#include "nau/diag/logging.h"


bool BaseTextureImpl::isSameFormat(int cflg1, int cflg2)
{
  if ((cflg1 & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == (cflg2 & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) &&
      (cflg1 & TEXFMT_MASK) == (cflg2 & TEXFMT_MASK))
  {
    return true;
  }
  return false;
}

BaseTextureImpl::BaseTextureImpl(int set_cflg, int set_type)
{
  anisotropyLevel = 1;
  addrU = TEXADDR_WRAP;
  addrV = TEXADDR_WRAP;
  addrW = TEXADDR_WRAP;

  texFilter = TEXFILTER_DEFAULT;
  mipFilter = TEXMIPMAP_DEFAULT;
  minMipLevel = 15; // 32768 texture size
  maxMipLevel = 0;
  mipLevels = 1;
  cflg = set_cflg;
  type = set_type;
  base_format = (cflg & TEXFMT_MASK);
}

int BaseTextureImpl::level_count() const { return mipLevels; }

void BaseTextureImpl::setTexName(const char8_t *name) { setResName(name); }

int BaseTextureImpl::getinfo(TextureInfo &ti, int level) const
{
  if (level >= mipLevels)
    level = mipLevels - 1;

  ti.w = Vectormath::max<uint32_t>(1u, width >> level);
  ti.h = Vectormath::max<uint32_t>(1u, height >> level);
  switch (type)
  {
    case RES3D_CUBETEX:
      ti.d = 1;
      ti.a = 6;
      break;
    case RES3D_CUBEARRTEX:
    case RES3D_ARRTEX:
      ti.d = 1;
      ti.a = depth * (isCubeArray() ? 6 : 1);
      break;
    case RES3D_VOLTEX:
      ti.d = Vectormath::max<uint32_t>(1u, depth >> level);
      ti.a = 1;
      break;
    default:
      ti.d = 1;
      ti.a = 1;
      break;
  }

  ti.mipLevels = mipLevels;
  ti.resType = type;
  ti.cflg = cflg;
  return 1;
}

uint32_t auto_mip_levels_count(uint32_t w, uint32_t h, uint32_t min_size)
{
  uint32_t lev = 1;
  uint32_t minExtend = Vectormath::min(w, h);
  while (minExtend > min_size)
  {
    lev++;
    minExtend >>= 1;
  }
  return lev;
}

int count_mips_if_needed(int w, int h, int32_t flg, int levels)
{
  if (0 == levels)
  {
    const bool rt = (TEXCF_RTARGET & flg) != 0;
    levels = auto_mip_levels_count(w, h, rt ? 1 : 4);
    NAU_LOG_DEBUG("Auto compute for texture mip levels yielded {}", levels);
  }

  return levels;
}
