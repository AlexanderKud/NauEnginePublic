// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "device.h"
#include "frontend_state.h"

#include "d3dformat.h"

#include "nau/dag_ioSys/dag_memIo.h"
#include "nau/image/dag_texPixel.h"
#include "nau/math/dag_adjpow2.h"
#include "nau/3d/dag_drv3d_pc.h"
#include <EASTL/tuple.h>
#include <drv3d_commonCode/basetexture.h>
#include <drv3d_commonCode/validateUpdateSubRegion.h>


#if 0
#define VERBOSE_DEBUG debug
#else
#define VERBOSE_DEBUG(...)
#endif

using namespace drv3d_dx12;

namespace
{
void free_and_reset_staging_memory(HostDeviceSharedMemoryRegion &staging_memory)
{
  get_device().getContext().freeMemory(staging_memory);
  staging_memory = HostDeviceSharedMemoryRegion{};
}

bool needs_subresource_tracking(uint32_t cflags)
{
  // In addition to RT, UA and copy target, dynamic textures also need state tracking, as they can (and will) be
  // updated multiple times during the same frame and we need to update them during GPU timeline.
  return 0 != (cflags & (TEXCF_RTARGET | TEXCF_UNORDERED | TEXCF_UPDATE_DESTINATION | TEXCF_DYNAMIC));
}

void clear_full_rt_resource_with_default_value(const ImageInfo &desc, const D3DTextures &tex)
{
  // render target always clear to zero on create.
  ImageSubresourceRange area;
  area.mipMapRange = desc.mips;
  area.arrayLayerRange = desc.arrays;
  if (desc.format.isColor())
  {
    ClearColorValue cv = {};
    get_device().getContext().clearColorImage(tex.image, area, cv);
  }
  else
  {
    ClearDepthStencilValue cv = {};
    get_device().getContext().clearDepthStencilImage(tex.image, area, cv);
  }
}

void clear_full_uav_resource(const ImageInfo &desc, const D3DTextures &tex, bool is_cube, uint32_t array_size)
{
  float clearValF[4] = {0.0f};
  unsigned clearValI[4] = {0};
  for (auto mip : desc.mips)
  {
    ImageViewState viewState;
    viewState.isCubemap = is_cube;
    viewState.isArray = array_size > 1;
    viewState.setArrayRange(desc.arrays);
    viewState.setDepthLayerRange(0, Vectormath::max(1u, desc.size.depth >> mip.index()));
    viewState.setSingleMipMapRange(mip);
    viewState.setUAV();
    viewState.setFormat(desc.format.getLinearVariant());
    if (desc.format.isFloat())
      get_device().getContext().clearUAVTexture(tex.image, viewState, clearValF);
    else
      get_device().getContext().clearUAVTexture(tex.image, viewState, clearValI);
  }
}

bool upload_initial_data_texture2d(D3DTextures &tex, ImageInfo &desc, BaseTex::ImageMem *initial_data)
{
  auto stage = tex.stagingMemory;

#if _TARGET_XBOX
  Image::TexAccessComputer *uploader = nullptr;
  if (!stage)
    uploader = tex.image->getAccessComputer();

  if (uploader)
  {
    for (auto i : desc.arrays)
    {
      for (auto j : desc.mips)
      {
        BaseTex::ImageMem &src = initial_data[desc.mips.count() * i.index() + j.index()];
        auto subResInfo = calculate_texture_mip_info(*tex.image, j);
        uploader->CopyIntoSubresource(tex.image->getMemory().asPointer(), 0,
          calculate_subresource_index(j.index(), i.index(), 0, desc.mips.count(), desc.arrays.count()), src.ptr, src.rowPitch,
          src.rowPitch * subResInfo.rowCount);
      }
    }
    // only need end access as begin is implicit after create
    get_device().getContext().endCPUTextureAccess(tex.image);
    // usually we only need this once
    tex.image->releaseAccessComputer();
    return true;
  }
#endif

  if (!stage)
  {
    auto size = calculate_texture_staging_buffer_size(*tex.image, tex.image->getSubresourceRange());
    stage = get_device().allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!stage)
    {
      return false;
    }
  }

  size_t offset = 0;
  for (auto i : desc.arrays)
  {
    size_t flushStart = offset;
    for (auto j : desc.mips)
    {
      BaseTex::ImageMem &src = initial_data[desc.mips.count() * i.index() + j.index()];
      auto subResInfo = calculate_texture_mip_info(*tex.image, j);
      NAU_ASSERT(src.rowPitch > 0);
      auto srcPtr = reinterpret_cast<const uint8_t *>(src.ptr);
      for (uint32_t r = 0; r < subResInfo.rowCount; ++r)
      {
        memcpy(&stage.pointer[offset], srcPtr, src.rowPitch);
        offset += subResInfo.footprint.RowPitch;
        srcPtr += src.rowPitch;
      }
      offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    }
    TextureMipsCopyInfo copies =
      calculate_texture_mips_copy_info(*tex.image, desc.mips.count(), i.index(), desc.arrays.count(), flushStart);
    stage.flushRegion(ValueRange<uint64_t>{flushStart, offset});
    get_device().getContext().uploadToImage(tex.image, copies.data(), desc.mips.count(), stage, DeviceQueueType::UPLOAD, false);
  }

  if (!tex.stagingMemory)
  {
    get_device().getContext().freeMemory(stage);
  }

  return true;
}

bool upload_initial_data_texture3d(D3DTextures &tex, ImageInfo &desc, BaseTex::ImageMem *initial_data)
{
#if _TARGET_XBOX
  Image::TexAccessComputer *uploader = tex.image->getAccessComputer();
  if (uploader)
  {
    for (auto j : desc.mips)
    {
      auto &src = initial_data[j.index()];
      uploader->CopyIntoSubresource(tex.image->getMemory().asPointer(), 0,
        calculate_subresource_index(j.index(), 0, 0, desc.mips.count(), 1), src.ptr, src.rowPitch, src.slicePitch);
    }
    // only need end access as begin is implicit after create
    get_device().getContext().endCPUTextureAccess(tex.image);
    // usually we only need this once
    tex.image->releaseAccessComputer();
    return true;
  }
#endif

  uint64_t size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, desc.mips.count()));
  auto stage = get_device().allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  if (!stage)
    return false;

  uint64_t offset = 0;
  for (auto j : desc.mips)
  {
    auto subResInfo = calculate_texture_mip_info(*tex.image, j);
    auto &initSource = initial_data[j.index()];
    auto srcPtr = reinterpret_cast<const uint8_t *>(initSource.ptr);
    for (uint32_t z = 0; z < subResInfo.footprint.Depth; ++z)
    {
      auto slicePtr = srcPtr;
      for (uint32_t r = 0; r < subResInfo.rowCount; ++r)
      {
        memcpy(&stage.pointer[offset], slicePtr, initSource.rowPitch);
        offset += subResInfo.footprint.RowPitch;
        slicePtr += initSource.rowPitch;
      }
      srcPtr += initSource.slicePitch;
    }
    offset = (offset + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  }
  stage.flush();

  TextureMipsCopyInfo copies = calculate_texture_mips_copy_info(*tex.image, desc.mips.count());
  get_device().getContext().uploadToImage(tex.image, copies.data(), desc.mips.count(), stage, DeviceQueueType::UPLOAD, false);
  get_device().getContext().freeMemory(stage);

  return true;
}

bool create_tex2d(D3DTextures &tex, BaseTex *bt_in, uint32_t w, uint32_t h, uint32_t levels, bool cube,
  BaseTex::ImageMem *initial_data, int array_size = 1, BaseTex *baseTexture = nullptr, bool temp_alloc = false)
{
  auto &device = get_device();

  uint32_t &flg = bt_in->cflg;
  NAU_ASSERT(!((flg & TEXCF_SAMPLECOUNT_MASK) && initial_data != nullptr));
  NAU_ASSERT(!((flg & TEXCF_LOADONCE) && (flg & (TEXCF_DYNAMIC | TEXCF_RTARGET))));

  if (flg & TEXCF_VARIABLE_RATE)
  {
    // Check rules for VRS textures
    NAU_ASSERT_RETURN(TEXFMT_R8UI == (flg & TEXFMT_MASK), false, "Variable Rate Textures can only use R8 UINT format");
    NAU_ASSERT_RETURN(0 == (flg & TEXCF_RTARGET), false, "Variable Rate Textures can not be used as render target");
    NAU_ASSERT_RETURN(0 == (flg & TEXCF_SAMPLECOUNT_MASK), false, "Variable Rate Textures can not be multisampled");
    NAU_ASSERT_RETURN(1 == array_size, false, "Variable Rate Textures can not be a arrayed texture");
    NAU_ASSERT_RETURN(false == cube, false, "Variable Rate Texture can not be a cube map");
    NAU_ASSERT_RETURN(1 == levels, false, "Variable Rate Texture can only have one mip level");
  }

  ImageInfo desc;
  desc.type = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.size.width = w;
  desc.size.height = h;
  desc.size.depth = 1;
  desc.mips = MipMapCount::make(levels);
  desc.arrays = ArrayLayerCount::make((cube ? 6 : 1) * array_size);
  desc.format = FormatStore::fromCreateFlags(flg);
  desc.memoryLayout = (flg & TEXCF_TILED_RESOURCE) ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.allocateSubresourceIDs = needs_subresource_tracking(flg);
  desc.sampleDesc.Count = get_sample_count(flg);

  flg = BaseTex::update_flags_for_linear_layout(flg, desc.format);
  desc.memoryClass = BaseTex::get_memory_class(flg);
#if _TARGET_XBOX
  if (TEXCF_LINEAR_LAYOUT & flg)
  {
    desc.memoryLayout = TEXTURE_LINEAR_LAYOUT;
  }
#endif

  tex.realMipLevels = levels;

  if (flg & TEXCF_SIMULTANEOUS_MULTI_QUEUE_USE)
  {
    NAU_ASSERT_RETURN(0 != ((TEXCF_RTARGET | TEXCF_UNORDERED) & flg), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be render targets "
      "and/or uav textures");
    NAU_ASSERT_RETURN(!desc.format.isDepth() && !desc.format.isStencil(), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be used with color "
      "texture formats");

    desc.usage |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  }

#if _TARGET_XBOXONE
  if (flg & TEXCF_RTARGET) // Fast clear doesn't work, clears only a top half of a thick textures with incorrect values, or clears
                           // areas adjacent to thin textures.
    desc.usage |= RESOURCE_FLAG_DENY_COLOR_COMPRESSION_DATA;
#endif

  const bool isDepth = desc.format.isDepth();
  const bool isRT = flg & TEXCF_RTARGET;
  const bool isUav = flg & TEXCF_UNORDERED;
  if (isRT)
  {
    reinterpret_cast<uint32_t &>(desc.usage) |=
      isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
  reinterpret_cast<uint32_t &>(desc.usage) |= (flg & TEXCF_UNORDERED) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : 0;

  NAU_ASSERT(!(isDepth && (flg & TEXCF_READABLE)));

  if (!temp_alloc)
    TEXQL_PRE_CLEAN(bt_in->ressize());

#if DX12_USE_ESRAM
  if (baseTexture)
  {
    auto baseEsram = baseTexture->cflg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    auto selfEsram = flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    if (baseEsram != (selfEsram & baseEsram))
    {
      NAU_LOG_WARNING("DX12: Alias texture has not matching ESRam flags to its base {:08x} != {:08x}", baseEsram, selfEsram);
    }
    if (baseEsram & TEXCF_MOVABLE_ESRAM)
    {
      NAU_LOG_WARNING("DX12: Aliased ESRam texture uses TEXCF_MOVABLE_ESRAM which has issues");
    }
    // when base texture is esram, than alias has to be also esram
    flg = (flg & ~(TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM)) | baseEsram;
  }
  if (flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM))
  {
    tex.image = device.createEsramBackedImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
  }
  if (!tex.image)
  {
#endif
    tex.image = device.createImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
#if DX12_USE_ESRAM
  }
  else if ((flg & TEXCF_MOVABLE_ESRAM) && tex.image->isEsramTexture())
  {
    EsramResource &resource = tex.image->getEsramResource();
    // no support for aliasing, this causes lots of problems
    resource.dramStorage = device.createImage(desc, nullptr, bt_in->getResName());
    device.registerMovableESRAMTexture(tex.image);
  }
#endif
  if (!tex.image)
  {
    if (tex.stagingMemory)
      free_and_reset_staging_memory(tex.stagingMemory);
    return false;
  }
  if (flg & TEXCF_SYSMEM)
  {
    tex.stagingMemory = BaseTex::allocate_read_write_staging_memory(tex.image, tex.image->getSubresourceRange());
    NAU_ASSERT(!isRT && !isDepth);
  }

  if ((flg & (TEXCF_READABLE | TEXCF_SYSMEM)) == TEXCF_READABLE && !tex.stagingMemory)
    tex.stagingMemory = BaseTex::allocate_read_write_staging_memory(tex.image, tex.image->getSubresourceRange());

  if (initial_data)
  {
    if (!upload_initial_data_texture2d(tex, desc, initial_data))
      return false;
  }
  else if (isRT)
    clear_full_rt_resource_with_default_value(desc, tex);
  else if (flg & TEXCF_CLEAR_ON_CREATE)
  {
    if (isUav)
    {
      clear_full_uav_resource(desc, tex, cube, array_size);
    }
    else
    {
      auto stage = tex.stagingMemory;
#if _TARGET_XBOX
      Image::TexAccessComputer *uploader = nullptr;
      if (!stage)
      {
        uploader = tex.image->getAccessComputer();
      }
      if (uploader)
      {
        memset(tex.image->getMemory().asPointer(), 0, tex.image->getMemory().size());
        device.getContext().endCPUTextureAccess(tex.image);
        tex.image->releaseAccessComputer();
      }
      else
#endif
      {
        if (!stage)
        {
          auto size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, 1));
          stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
          if (!stage)
            return 0;
        }

        BufferImageCopy copies[MAX_MIPMAPS];
        memset(stage.pointer, 0, stage.range.size());
        stage.flush();
        for (auto i : desc.arrays)
        {
          for (auto j : desc.mips)
            copies[j.index()] = calculate_texture_subresource_copy_info(*tex.image,
              calculate_subresource_index(j.index(), i.index(), 0, desc.mips.count(), desc.arrays.count()));
          device.getContext().uploadToImage(tex.image, copies, desc.mips.count(), stage, DeviceQueueType::UPLOAD, false);
        }
        if (!tex.stagingMemory)
          device.getContext().freeMemory(stage);
      }
    }
  }

  tex.memSize = bt_in->ressize();
  bt_in->updateTexName();
  TEXQL_ON_ALLOC(bt_in);
  return true;
}

bool create_tex3d(D3DTextures &tex, BaseTex *bt_in, uint32_t w, uint32_t h, uint32_t d, uint32_t flg, uint32_t levels,
  BaseTex::ImageMem *initial_data, BaseTex *baseTexture = nullptr)
{
  auto &device = get_device();
  auto &ctx = device.getContext();

  NAU_ASSERT((flg & TEXCF_SAMPLECOUNT_MASK) == 0);
  NAU_ASSERT(!((flg & TEXCF_LOADONCE) && (flg & TEXCF_DYNAMIC)));
  NAU_ASSERT_RETURN(0 == (flg & TEXCF_VARIABLE_RATE), false, "Variable Rate Texture can not be a volumetric texture");

  ImageInfo desc;
  desc.type = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
  desc.size.width = w;
  desc.size.height = h;
  desc.size.depth = d;
  desc.mips = MipMapCount::make(levels);
  desc.arrays = ArrayLayerCount::make(1);
  desc.format = FormatStore::fromCreateFlags(flg);
  desc.memoryLayout = (flg & TEXCF_TILED_RESOURCE) ? D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE : D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.allocateSubresourceIDs = needs_subresource_tracking(flg);

  flg = BaseTex::update_flags_for_linear_layout(flg, desc.format);
  desc.memoryClass = BaseTex::get_memory_class(flg);
#if _TARGET_XBOX
  if (TEXCF_LINEAR_LAYOUT & flg)
  {
    desc.memoryLayout = TEXTURE_LINEAR_LAYOUT;
  }
#endif

  tex.realMipLevels = levels;

  if (flg & TEXCF_SIMULTANEOUS_MULTI_QUEUE_USE)
  {
    NAU_ASSERT_RETURN(0 != ((TEXCF_RTARGET | TEXCF_UNORDERED) & flg), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be render targets "
      "and/or uav textures");
    NAU_ASSERT_RETURN(!desc.format.isDepth() && !desc.format.isStencil(), false,
      "Resource with TEXCF_SIMULTANIOUS_MULTI_QUEUE_USE can only be used with color "
      "texture formats");

    desc.usage |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
  }

  const bool isRT = flg & TEXCF_RTARGET;
  const bool isUav = flg & TEXCF_UNORDERED;
  if (flg & TEXCF_UNORDERED)
    reinterpret_cast<uint32_t &>(desc.usage) |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

  TEXQL_PRE_CLEAN(bt_in->ressize());

  const bool isDepth = desc.format.isDepth();
  if (isRT)
  {
    reinterpret_cast<uint32_t &>(desc.usage) |=
      isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  }
#if DX12_USE_ESRAM
  if (baseTexture)
  {
    auto baseEsram = baseTexture->cflg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    auto selfEsram = flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM);
    if (baseEsram != (selfEsram & baseEsram))
    {
      NAU_LOG_WARNING("DX12: Alias texture has not matching ESRam flags to its base {:08x} != {:08x}", baseEsram, selfEsram);
    }
    if (baseEsram & TEXCF_MOVABLE_ESRAM)
    {
      NAU_LOG_WARNING("DX12: Aliased ESRam texture uses TEXCF_MOVABLE_ESRAM which has issues");
    }
    // when base texture is esram, than alias has to be also esram
    flg = (flg & ~(TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM)) | baseEsram;
  }
  if (flg & (TEXCF_ESRAM_ONLY | TEXCF_MOVABLE_ESRAM))
  {
    tex.image = device.createEsramBackedImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
  }
  if (!tex.image)
  {
#endif
    tex.image = device.createImage(desc, baseTexture ? baseTexture->tex.image : nullptr, bt_in->getResName());
#if DX12_USE_ESRAM
  }
  else if ((flg & TEXCF_MOVABLE_ESRAM) && tex.image->isEsramTexture())
  {
    EsramResource &resource = tex.image->getEsramResource();
    // no support for aliasing, this causes lots of problems
    resource.dramStorage = device.createImage(desc, nullptr, bt_in->getResName());
    device.registerMovableESRAMTexture(tex.image);
  }
#endif
  if (!tex.image)
    return false;

  if (initial_data)
  {
    if (!upload_initial_data_texture3d(tex, desc, initial_data))
      return false;
  }
  else if (isRT)
  {
    // init render target to a known state
    clear_full_rt_resource_with_default_value(desc, tex);
  }
  else if (flg & TEXCF_CLEAR_ON_CREATE)
  {
    if (isUav)
      clear_full_uav_resource(desc, tex, false, 1);
    else
    {
#if _TARGET_XBOX
      Image::TexAccessComputer *uploader = tex.image->getAccessComputer();
      if (uploader)
      {
        memset(tex.image->getMemory().asPointer(), 0, tex.image->getMemory().size());
        device.getContext().endCPUTextureAccess(tex.image);
        tex.image->releaseAccessComputer();
      }
      else
#endif
      {
        uint64_t size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, 1));
        auto stage = device.allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (stage)
        {
          memset(stage.pointer, 0, size);
          stage.flush();

          BufferImageCopy copies[MAX_MIPMAPS];
          for (auto j : desc.mips)
            copies[j.index()] =
              calculate_texture_subresource_copy_info(*tex.image, calculate_subresource_index(j.index(), 0, 0, desc.mips.count(), 1));
          ctx.uploadToImage(tex.image, copies, levels, stage, DeviceQueueType::UPLOAD, false);
          ctx.freeMemory(stage);
        }
        else
        {
          return 0;
        }
      }
    }
  }

  tex.memSize = bt_in->ressize();
  bt_in->updateTexName();
  TEXQL_ON_ALLOC(bt_in);
  return true;
}
} // namespace

ImageViewState BaseTex::getViewInfoUav(MipMapIndex mip, ArrayLayerIndex layer, bool as_uint) const
{
  ImageViewState result;
  if (resType == RES3D_TEX)
  {
    result.isCubemap = 0;
    result.isArray = 0;
    result.setSingleArrayRange(ArrayLayerIndex::make(0));
    NAU_ASSERT(layer.index() == 0, "UAV view for layer/face {} requested, but texture was 2d and has no layers/faces", (int)layer.index()); // layer
  }
  else if (resType == RES3D_CUBETEX)
  {
    result.isCubemap = 1;
    result.isArray = 0;
    result.setArrayRange(getArrayCount().front(layer));
    NAU_ASSERT(layer < getArrayCount(),
      "UAV view for layer/face %u requested, but texture was cubemap and has only 6 "
      "faces",
      (int)layer.index()); //layer
  }
  else if (resType == RES3D_ARRTEX)
  {
    result.isArray = 1;
    result.isCubemap = isArrayCube();
    result.setArrayRange(getArrayCount().front(layer));
    NAU_ASSERT(layer < getArrayCount(), "UAV view for layer/face {} requested, but texture has only {} layers", (int)layer.index(), getArrayCount().count()); //getArrayCount()
  }
  else if (resType == RES3D_VOLTEX)
  {
    result.isArray = 0;
    result.isCubemap = 0;
    result.setDepthLayerRange(0, Vectormath::max<uint16_t>(1, depth >> mip.index()));
    NAU_ASSERT(layer.index() == 0, "UAV view for layer/face {} requested, but texture was 3d and has no layers/faces", (int)layer.index());
  }
  if (as_uint)
  {
    NAU_ASSERT(getFormat().getBytesPerPixelBlock() == 4 ||
             (getFormat().getBytesPerPixelBlock() == 8 && d3d::get_driver_desc().caps.hasShader64BitIntegerResources));
    if (getFormat().getBytesPerPixelBlock() == 4)
      result.setFormat(FormatStore::fromCreateFlags(TEXFMT_R32UI));
    else if (getFormat().getBytesPerPixelBlock() == 8)
      result.setFormat(FormatStore::fromCreateFlags(TEXFMT_R32G32UI));
  }
  else
  {
    result.setFormat(getFormat().getLinearVariant());
  }
  result.setSingleMipMapRange(mip);
  result.setUAV();
  return result;
}

ImageViewState BaseTex::getViewInfoRenderTarget(MipMapIndex mip, ArrayLayerIndex layer, bool as_const) const
{
  FormatStore format = isSrgbWriteAllowed() ? getFormat() : getFormat().getLinearVariant();
  ImageViewState result;
  result.isArray = resType == RES3D_ARRTEX;
  result.isCubemap = resType == RES3D_CUBETEX;
  result.setFormat(format);
  result.setSingleMipMapRange(mip);

  if (layer.index() < d3d::RENDER_TO_WHOLE_ARRAY)
  {
    result.setSingleArrayRange(layer);
  }
  else
  {
    if (RES3D_VOLTEX == resType)
    {
      result.setDepthLayerRange(0, Vectormath::max<uint16_t>(1, depth >> mip.index()));
    }
    else
    {
      result.setArrayRange(getArrayCount());
    }
  }

  if (format.isColor())
  {
    result.setRTV();
  }
  else
  {
    result.setDSV(as_const);
  }

  return result;
}

ImageViewState BaseTex::getViewInfo() const
{
  ImageViewState result;
  result.setFormat(isSrgbReadAllowed() ? getFormat() : getFormat().getLinearVariant());
  result.isArray = resType == RES3D_ARRTEX ? 1 : 0;
  result.isCubemap = resType == RES3D_CUBETEX ? 1 : (resType == RES3D_ARRTEX ? int(isArrayCube()) : 0);
  int32_t baseMip = Vectormath::clamp<int32_t>(maxMipLevel, 0, Vectormath::max(0, (int32_t)tex.realMipLevels - 1));
  int32_t mipCount = (minMipLevel - maxMipLevel) + 1;
  if (mipCount <= 0 || baseMip + mipCount > tex.realMipLevels)
  {
    mipCount = tex.realMipLevels - baseMip;
  }
  if (isStub())
  {
    baseMip = 0;
    mipCount = 1;
  }
  result.setMipBase(MipMapIndex::make(baseMip));
  result.setMipCount(Vectormath::max(mipCount, 1));
  result.setArrayRange(getArrayCount());
  result.setSRV();
  result.sampleStencil = isSampleStencil();
  return result;
}

FormatStore BaseTex::getFormat() const { return tex.image ? tex.image->getFormat() : fmt; }

void BaseTex::updateDeviceSampler()
{
  sampler = get_device().getSampler(samplerState);
  lastSamplerState = samplerState;
}

D3D12_CPU_DESCRIPTOR_HANDLE BaseTex::getDeviceSampler()
{
  if (!sampler.ptr || samplerState != lastSamplerState)
  {
    updateDeviceSampler();
  }

  return sampler;
}

Extent3D BaseTex::getMipmapExtent(uint32_t level) const
{
  Extent3D result;
  result.width = Vectormath::max(width >> level, 1);
  result.height = Vectormath::max(height >> level, 1);
  result.depth = resType == RES3D_VOLTEX ? Vectormath::max(depth >> level, 1) : 1;
  return result;
}

void BaseTex::updateTexName()
{
  // don't propagate down to stub images
  if (isStub())
    return;
  if (tex.image)
  {
    get_device().setTexName(tex.image, getResName());
  }
}

void BaseTex::setTexName(const char8_t *name)
{
  setResName(name);
  updateTexName();
}

bool BaseTex::copySubresourcesToStaging(void **pointer, int &stride, int level, uint32_t flags, uint32_t prev_flags)
{
  if (!tex.stagingMemory)
    tex.stagingMemory = allocate_read_write_staging_memory(tex.image, SubresourceRange::make(0, mipLevels));
  if (!tex.stagingMemory)
    return false;

  auto copies = calculate_texture_mips_copy_info(*tex.image, mipLevels);

  fillLockedLevelInfo(level, copies[level].layout.Offset);

  const DeviceQueueType readBackQueue = is_swapchain_color_image(tex.image) ? DeviceQueueType::GRAPHICS : DeviceQueueType::READ_BACK;
  auto &ctx = get_device().getContext();

  // - It is required to copy full resource in case of TEXLOCK_COPY_STAGING
  //   (because with TEXLOCK_COPY_STAGING we will upload full resource later)
  // - It is also required with lockimg(nullptr, ...)
  //   (in this case we mark resource with TEX_COPIED flag and we can rely on these copies)
  // - Otherwise -- we can copy only locked mip
  if (flags & TEXLOCK_COPY_STAGING)
  {
    ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, copies.data(), mipLevels, tex.image, readBackQueue));
    tex.stagingMemory.invalidate();
    lockFlags = TEXLOCK_COPY_STAGING;
    return true;
  }
  if (!pointer)
  {
    // on some cases the requested copy is never used
    waitAndResetProgress();
    waitProgress = ctx.readBackFromImage(tex.stagingMemory, copies.data(), mipLevels, tex.image, readBackQueue);
    lockFlags = TEX_COPIED;
    stride = 0;
    return true;
  }
  if (prev_flags != TEX_COPIED)
  {
    ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, &copies[level], 1, tex.image, readBackQueue));
    tex.stagingMemory.invalidate();
    *pointer = lockMsr.ptr;
    stride = lockMsr.rowPitch;
    lockFlags = flags;
    lockedSubRes = calculate_subresource_index(level, 0, 0, mipLevels, 1);
    return true;
  }
  return false;
}

bool BaseTex::waitAndResetProgress()
{
  if (waitProgress)
  {
    get_device().getContext().waitForProgress(waitProgress);
    waitProgress = 0;
    return true;
  }
  return false;
}

void BaseTex::fillLockedLevelInfo(int level, uint64_t offset)
{
  auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(level));
  lockMsr.rowPitch = subResInfo.footprint.RowPitch;
  lockMsr.slicePitch = subResInfo.footprint.RowPitch * subResInfo.rowCount;
  lockMsr.memSize = lockMsr.slicePitch;
  lockMsr.ptr = &tex.stagingMemory.pointer[offset];
}

HostDeviceSharedMemoryRegion BaseTex::allocate_read_write_staging_memory(const Image *image, const SubresourceRange &subresource_range)
{
  uint64_t size = calculate_texture_staging_buffer_size(*image, subresource_range);
  return get_device().allocatePersistentBidirectionalMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
}

void BaseTex::notifySamplerChange()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_sampler(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::notifySrvChange()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::notifyTextureReplaceFinish()
{
  // if we are ending up here, we are still holding the lock of the state tracker
  // to avoid a deadlock by reentering we have to do the dirtying without a lock
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv_and_sampler_no_lock(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::dirtyBoundSrvsNoLock()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (srvBindingStages[s].any())
    {
      dirty_srv_no_lock(this, s, srvBindingStages[s]);
    }
  }
}

void BaseTex::dirtyBoundUavsNoLock()
{
  for (uint32_t s = 0; s < STAGE_MAX_EXT; ++s)
  {
    if (uavBindingStages[s].any())
    {
      dirty_uav_no_lock(this, s, uavBindingStages[s]);
    }
  }
}

void BaseTex::setUavBinding(uint32_t stage, uint32_t index, bool s)
{
  uavBindingStages[stage].set(index, s);
  stateBitSet.set(acitve_binding_was_used_offset);
}

void BaseTex::setSrvBinding(uint32_t stage, uint32_t index, bool s)
{
  srvBindingStages[stage].set(index, s);
  stateBitSet.set(acitve_binding_was_used_offset);
}

void BaseTex::setRtvBinding(uint32_t index, bool s)
{
  NAU_ASSERT(index < Driver3dRenderTarget::MAX_SIMRT);
  stateBitSet.set(active_binding_rtv_offset + index, s);
  stateBitSet.set(acitve_binding_was_used_offset);
}

void BaseTex::setDsvBinding(bool s)
{
  stateBitSet.set(active_binding_dsv_offset, s);
  stateBitSet.set(acitve_binding_was_used_offset);
}

Bitset<Driver3dRenderTarget::MAX_SIMRT> BaseTex::getRtvBinding() const
{
  Bitset<Driver3dRenderTarget::MAX_SIMRT> ret;
  ret.from_uint64((stateBitSet >> active_binding_rtv_offset).to_uint64());
  return ret;
}

void BaseTex::setParams(int w, int h, int d, int levels, const char8_t *stat_name)
{
  NAU_ASSERT(levels > 0);
  fmt = FormatStore::fromCreateFlags(cflg);
  mipLevels = levels;
  width = w;
  height = h;
  depth = d;
  maxMipLevel = 0;
  minMipLevel = levels - 1;
  setTexName(stat_name);
}

ArrayLayerCount BaseTex::getArrayCount() const
{
  if (resType == RES3D_CUBETEX)
  {
    return ArrayLayerCount::make(6);
  }
  else if (resType == RES3D_ARRTEX)
  {
    return ArrayLayerCount::make((isArrayCube() ? 6 : 1) * depth);
  }
  return ArrayLayerCount::make(1);
}

void BaseTex::setResApiName(const char8_t *name) const
{
  G_UNUSED(name);
#if DX12_DOES_SET_DEBUG_NAMES
  wchar_t stringBuf[MAX_OBJECT_NAME_LENGTH];
  DX12_SET_DEBUG_OBJ_NAME(tex.image->getHandle(), lazyToWchar(name, stringBuf, MAX_OBJECT_NAME_LENGTH));
#endif
}

BaseTex::BaseTex(int res_type, uint32_t cflg_) :
  cflg(cflg_), resType(res_type), width(0), height(0), depth(1), lockFlags(0), minMipLevel(20), maxMipLevel(0)
{
  samplerState.setBias(default_lodbias);
  samplerState.setAniso(default_aniso);
  samplerState.setW(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setV(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setU(D3D12_TEXTURE_ADDRESS_MODE_WRAP);
  samplerState.setMip(D3D12_FILTER_TYPE_LINEAR);
  samplerState.setMinFilter(D3D12_FILTER_TYPE_LINEAR);
  samplerState.setMagFilter(D3D12_FILTER_TYPE_LINEAR);

  if (RES3D_CUBETEX == resType)
  {
    samplerState.setV(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
    samplerState.setU(D3D12_TEXTURE_ADDRESS_MODE_CLAMP);
  }
}

void BaseTex::resolve(Image *dst) { get_device().getContext().resolveMultiSampleImage(tex.image, dst); }

BaseTexture *BaseTex::makeTmpTexResCopy(int w, int h, int d, int l, bool staging_tex)
{
  STORE_RETURN_ADDRESS();
  if (resType != RES3D_ARRTEX && resType != RES3D_VOLTEX && resType != RES3D_CUBEARRTEX)
    d = 1;
  BaseTex *clonedTex = get_device().newTextureObject(resType, cflg | (staging_tex ? TEXCF_SYSMEM : 0));
  if (!staging_tex)
    clonedTex->tidXored = tidXored, clonedTex->stubTexIdx = stubTexIdx;
  
  nau::string tempStr = eastl::string(staging_tex ? "stg:" : "tmp:");
  tempStr.append(getTexName());
  clonedTex->setParams(w, h, d, l, tempStr.c_str());//String::mk_str_cat(staging_tex ? "stg:" : "tmp:", getTexName()));
  clonedTex->setIsPreallocBeforeLoad(true);
  if (!clonedTex->allocateTex())
    del_d3dres(clonedTex);
  return clonedTex;
}
void BaseTex::replaceTexResObject(BaseTexture *&other_tex)
{
  {
    lock_(drv3d_dx12::get_resource_binding_guard());
    BaseTex *other = getbasetex(other_tex);
    NAU_ASSERT_RETURN(other, );

    // swap texture objects
    char tmp[sizeof(tex)];
    memcpy(tmp, &tex, sizeof(tmp));
    memcpy(&tex, &other->tex, sizeof(tmp));
    memcpy(&other->tex, tmp, sizeof(tmp));
    // swap dimensions
    eastl::swap(width, other->width);
    eastl::swap(height, other->height);
    eastl::swap(depth, other->depth);
    eastl::swap(mipLevels, other->mipLevels);
    eastl::swap(minMipLevel, other->minMipLevel);
    eastl::swap(maxMipLevel, other->maxMipLevel);

#if DAGOR_DBGLEVEL > 0
    other->setWasUsed();
#endif
  }
  del_d3dres(other_tex);
}

bool BaseTex::downSize(int new_width, int new_height, int new_depth, int new_mips, unsigned start_src_level, unsigned level_offset)
{
  auto rep = makeTmpTexResCopy(new_width, new_height, new_depth, new_mips, false);
  if (!rep)
  {
    return false;
  }

  auto repTex = getbasetex(rep);

  unsigned sourceLevel = Vectormath::max<unsigned>(level_offset, start_src_level);
  unsigned sourceLevelEnd = Vectormath::min<unsigned>(mipLevels, new_mips + level_offset);

  rep->texmiplevel(sourceLevel - level_offset, sourceLevelEnd - level_offset - 1);

  STORE_RETURN_ADDRESS();
  get_device().getContext().resizeImageMipMapTransfer(getDeviceImage(), repTex->getDeviceImage(),
    MipMapRange::make(sourceLevel, sourceLevelEnd - sourceLevel), 0, level_offset);

  replaceTexResObject(rep);
  return true;
}

bool BaseTex::upSize(int new_width, int new_height, int new_depth, int new_mips, unsigned start_src_level, unsigned level_offset)
{
  auto rep = makeTmpTexResCopy(new_width, new_height, new_depth, new_mips, false);
  if (!rep)
  {
    return false;
  }

  auto repTex = getbasetex(rep);

  unsigned destinationLevel = level_offset + start_src_level;
  unsigned destinationLevelEnd = Vectormath::min<unsigned>(mipLevels + level_offset, new_mips);

  rep->texmiplevel(destinationLevel, destinationLevelEnd - 1);

  STORE_RETURN_ADDRESS();
  get_device().getContext().resizeImageMipMapTransfer(getDeviceImage(), repTex->getDeviceImage(),
    MipMapRange::make(destinationLevel, destinationLevelEnd - destinationLevel), level_offset, 0);

  replaceTexResObject(rep);
  return true;
}

bool BaseTex::allocateTex()
{
  if (tex.image)
    return true;
  STORE_RETURN_ADDRESS();
  switch (resType)
  {
    case RES3D_VOLTEX: return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, nullptr);
    case RES3D_TEX:
    case RES3D_CUBETEX: return create_tex2d(tex, this, width, height, mipLevels, resType == RES3D_CUBETEX, nullptr, 1);
    case RES3D_CUBEARRTEX:
    case RES3D_ARRTEX: return create_tex2d(tex, this, width, height, mipLevels, resType == RES3D_CUBEARRTEX, nullptr, depth);
  }
  return false;
}

void BaseTex::discardTex()
{
  if (stubTexIdx >= 0)
  {
    releaseTex();
    recreate();
  }
}

bool BaseTex::recreate()
{
  STORE_RETURN_ADDRESS();
  if (RES3D_TEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "rt|dyn");
      return create_tex2d(tex, this, width, height, mipLevels, false, NULL, 1);
    }

    if (!isPreallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "empty");
      return create_tex2d(tex, this, width, height, mipLevels, false, NULL, 1);
    }

    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex2d(tex, this, 4, 4, 1, false, NULL, 1);
  }

  if (RES3D_CUBETEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "rt|dyn");
      return create_tex2d(tex, this, width, height, mipLevels, true, NULL, 1);
    }

    if (!isPreallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), width, height, "empty");
      return create_tex2d(tex, this, width, height, mipLevels, true, NULL, 1);
    }

    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex2d(tex, this, 4, 4, 1, true, NULL, 1);
  }

  if (RES3D_VOLTEX == resType)
  {
    if (cflg & (TEXCF_RTARGET | TEXCF_DYNAMIC))
    {
      VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "rt|dyn");
      return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, NULL);
    }

    if (!isPreallocBeforeLoad())
    {
      VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "empty");
      return create_tex3d(tex, this, width, height, depth, cflg, mipLevels, NULL);
    }

    VERBOSE_DEBUG("<%s> recreate %dx%d (%s)", getResName(), 4, 4, "placeholder");
    if (stubTexIdx >= 0)
    {
      auto stubTex = getStubTex();
      if (stubTex)
        tex.image = stubTex->tex.image;
      return 1;
    }
    return create_tex3d(tex, this, 4, 4, 1, cflg, 1, NULL);
  }

  if (RES3D_ARRTEX == resType)
  {
    VERBOSE_DEBUG("<%s> recreate %dx%dx%d (%s)", getResName(), width, height, depth, "rt|dyn");
    return create_tex2d(tex, this, width, height, mipLevels, isArrayCube(), NULL, depth);
  }

  return false;
}

uint32_t BaseTex::update_flags_for_linear_layout(uint32_t cflags, FormatStore format)
{
#if _TARGET_XBOX
#if !_TARGET_XBOXONE
  if ((TEXCF_RTARGET | TEXCF_LINEAR_LAYOUT) == (cflags & (TEXCF_RTARGET | TEXCF_LINEAR_LAYOUT)))
  {
    // can't use linear layout on render targets, GPU will just hang if we try it
    cflags &= ~(TEXCF_LINEAR_LAYOUT | TEXCF_CPU_CACHED_MEMORY);
  }
#endif
  if ((TEXCF_LINEAR_LAYOUT & cflags) != 0 && format.isBlockCompressed())
    cflags &= ~(TEXCF_LINEAR_LAYOUT | TEXCF_CPU_CACHED_MEMORY);
#endif
  G_UNUSED(format);
  return cflags;
}

DeviceMemoryClass BaseTex::get_memory_class(uint32_t cflags)
{
#if _TARGET_XBOX
  if ((TEXCF_LINEAR_LAYOUT & cflags) != 0 && (TEXCF_CPU_CACHED_MEMORY & cflags) != 0)
    return DeviceMemoryClass::HOST_RESIDENT_HOST_READ_WRITE_IMAGE;
#endif
  if (TEXCF_TILED_RESOURCE & cflags)
  {
    return DeviceMemoryClass::RESERVED_RESOURCE;
  }

  G_UNUSED(cflags);
  return DeviceMemoryClass::DEVICE_RESIDENT_IMAGE;
}

void BaseTex::destroyObject()
{
  releaseTex();
  // engine deletes textures that where just loaded...
  // if not deferred we will end up crashing when it was loaded via streaming load
  get_device().getContext().deleteTexture(this);
}

#if DAGOR_DBGLEVEL < 1
#define NAU_ASSERT_RETURN_AND_LOG(expression, rv, ...) \
  do                                                 \
  {                                                  \
    if (/*DAGOR_UNLIKELY*/(!(expression)))               \
    {                                                \
      NAU_LOG_ERROR(__VA_ARGS__);                           \
      return rv;                                     \
    }                                                \
  } while (0)
#else
#define NAU_ASSERT_RETURN_AND_LOG(expr, rv, ...)               \
  do                                                         \
  {                                                          \
    const bool NAU_ASSERT_result_do_ = !!(expr);               \
    NAU_ASSERT_EX(NAU_ASSERT_result_do_, #expr, ##__VA_ARGS__); \
    if (/*DAGOR_UNLIKELY*/(!NAU_ASSERT_result_do_))                \
      return rv;                                             \
  } while (0)
#endif

namespace
{
bool can_be_copy_updated(uint32_t cflg) { return 0 != (cflg & (TEXCF_UPDATE_DESTINATION | TEXCF_RTARGET | TEXCF_UNORDERED)); }
} // namespace

int BaseTex::update(BaseTexture *src)
{
  if (!can_be_copy_updated(cflg))
  {
    NAU_LOG_ERROR("DX12: used update method on texture <{}> that does not support it, the texture needs either TEXCF_UPDATE_DESTINATION, "
           "TEXCF_RTARGET or TEXCF_UNORDERED create flags specified",
      getResName());
    return 0;
  }
  BaseTex *sTex = getbasetex(src);
  if (!sTex)
  {
    NAU_LOG_ERROR("DX12: BaseTex::update for <{}> called with null as source", getTexName());
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (isStub())
  {
    NAU_LOG_ERROR("DX12: BaseTex::update for <{}> is in stub state: stubTexIdx={}", getTexName(), stubTexIdx);
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (sTex->isStub())
  {
    NAU_LOG_ERROR("DX12: BaseTex::update for <{}>, source <{}> is in stub state: stubTexIdx={}", getTexName(), sTex->getTexName(),
      sTex->stubTexIdx);
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if ((RES3D_TEX != resType) && (RES3D_CUBETEX != resType) && (RES3D_VOLTEX != resType))
  {
    NAU_LOG_ERROR("DX12: BaseTex::update for <{}> with invalid resType {}", getTexName(), resType);
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  auto srcImage = sTex->tex.image;
  auto dstImage = tex.image;

  if (!srcImage || !dstImage)
  {
    NAU_LOG_ERROR("DX12: BaseTex::update for <{}> and <{}> at least one of the image objects ({:p}, {:p}) was "
           "null",
      getTexName(), sTex->getTexName(), (void*)dstImage, (void*)srcImage);
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (srcImage->getMipLevelRange() != dstImage->getMipLevelRange())
  {
    NAU_LOG_ERROR("DX12: BaseTex::update source <{}> mipmaps {} does not match dest <{}> mipmaps {}", sTex->getTexName(),
                    srcImage->getMipLevelRange().count(), getTexName(), dstImage->getMipLevelRange().count());
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  if (srcImage->getArrayLayers() != dstImage->getArrayLayers())
  {
    NAU_LOG_ERROR("DX12: BaseTex::update source <{}> array layers {} does not match dst <{}> "
           "array layers {}",
          sTex->getTexName(), srcImage->getArrayLayers().count(), getTexName(), dstImage->getArrayLayers().count());
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

  const auto srcFmt = srcImage->getFormat();
  const auto dstFmt = dstImage->getFormat();

  NAU_ASSERT_RETURN_AND_LOG(srcFmt.isCopyConvertible(dstFmt), 0,
    "DX12: BaseTex::update source <%s> format %s can not be copied to dest <%s> "
    "format %s",
    sTex->getTexName(), srcFmt.getNameString(), getTexName(), dstFmt.getNameString());

  auto sExt = srcImage->getBaseExtent();
  auto dExt = dstImage->getBaseExtent();

  uint32_t sBlockX, sBlockY, dBlockX, dBlockY;
  srcFmt.getBytesPerPixelBlock(&sBlockX, &sBlockY);
  dstFmt.getBytesPerPixelBlock(&dBlockX, &dBlockY);

  sExt.width *= sBlockX;
  sExt.height *= sBlockY;
  dExt.width *= dBlockX;
  dExt.height *= dBlockY;

  if (sExt != dExt)
  {
    NAU_LOG_ERROR("DX12: BaseTex::update source <{}> extent {} {} {} does not match dest <{}> extent {} "
           "{} {}",
      sTex->getTexName(), sExt.width, sExt.height, sExt.depth, getTexName(), dExt.width, dExt.height, dExt.depth);
    NAU_ASSERT_RETURN(false, 0, "DX12: Error in BaseTex::update, see error log for details");
  }

#if DAGOR_DBGLEVEL > 0
  sTex->setWasUsed();
#endif
  STORE_RETURN_ADDRESS();
  ScopedCommitLock ctxLock{get_device().getContext()};
  if (sTex->isMultisampled())
  {
    sTex->resolve(tex.image);
  }
  else
  {
    // passing no regions is fast whole resource copy
    get_device().getContext().copyImage(sTex->tex.image, tex.image, make_whole_resource_copy_info());
  }
  return 1;
}

int BaseTex::updateSubRegion(BaseTexture *src, int src_subres_idx, int src_x, int src_y, int src_z, int src_w, int src_h, int src_d,
  int dest_subres_idx, int dest_x, int dest_y, int dest_z)
{
  if (!can_be_copy_updated(cflg))
  {
    NAU_LOG_ERROR("DX12: used updateSubRegion method on texture <{}> that does not support it, the texture needs either "
           "TEXCF_UPDATE_DESTINATION, TEXCF_RTARGET or TEXCF_UNORDERED create flags specified",
      getResName());
    return 0;
  }
  STORE_RETURN_ADDRESS();
  if (isStub())
  {
    NAU_LOG_ERROR("updateSubRegion() called for tex=<{}> in stub state: stubTexIdx={}", getTexName(), stubTexIdx);
    return 0;
  }

  BaseTex *stex = getbasetex(src);

  if (stex == nullptr)
    return 0;

  if (stex->isStub())
  {

    NAU_LOG_ERROR("updateSubRegion() called with src tex=<{}> in stub state: stubTexIdx={}", src->getTexName(), stex->stubTexIdx);
    return 0;
  }

  if ((RES3D_TEX != resType) && (RES3D_CUBETEX != resType) && (RES3D_VOLTEX != resType) && (RES3D_ARRTEX != resType))
    return 0;

  if (stex->tex.image == nullptr)
    return 0;

  if (!validate_update_sub_region_params(src, src_subres_idx, src_x, src_y, src_z, src_w, src_h, src_d, this, dest_subres_idx, dest_x,
        dest_y, dest_z))
    return 0;

  // we have to check if the copy offsets and sizes are multiples of the block size
  // of the texture format and round them up.
  // If we don't do that, those copies will reset the device.
  auto sfmt = stex->getFormat();
  auto dfmt = getFormat();

  NAU_ASSERT_RETURN_AND_LOG(sfmt.isCopyConvertible(dfmt), 0,
    "DX12: BaseTex::updateSubRegion source <%s> format %s can not be copied "
    "to dest <%s> format %s",
    stex->getTexName(), sfmt.getNameString(), getTexName(), dfmt.getNameString());

  uint32_t sbx, sby, dbx, dby;
  sfmt.getBytesPerPixelBlock(&sbx, &sby);
  dfmt.getBytesPerPixelBlock(&dbx, &dby);

  auto fix_block_size = [](auto value, auto block, auto name) {
    if (auto delta = value % block)
    {
      decltype(value) adjusted = value + block - delta;
      NAU_LOG_DEBUG("DX12: {} = {} is not multiples of {}, adjusting to {}", name, value, block, adjusted);
      return adjusted;
    }
    return value;
  };

  src_x = fix_block_size(src_x, sbx, "src_x");
  src_y = fix_block_size(src_y, sby, "src_y");

  src_w = fix_block_size(src_w, sbx, "src_w");
  src_h = fix_block_size(src_h, sby, "src_h");

  dest_x = fix_block_size(dest_x, dbx, "dest_x");
  dest_y = fix_block_size(dest_y, dby, "dest_y");

  ImageCopy region;
  region.srcSubresource = SubresourceIndex::make(src_subres_idx);
  region.dstSubresource = SubresourceIndex::make(dest_subres_idx);
  region.dstOffset.x = dest_x;
  region.dstOffset.y = dest_y;
  region.dstOffset.z = dest_z;
  region.srcBox.left = src_x;
  region.srcBox.top = src_y;
  region.srcBox.front = src_z;
  region.srcBox.right = src_x + src_w;
  region.srcBox.bottom = src_y + src_h;
  region.srcBox.back = src_z + src_d;

  ScopedCommitLock ctxLock{get_device().getContext()};
  get_device().getContext().copyImage(stex->tex.image, tex.image, region);
#if DAGOR_DBGLEVEL > 0
  stex->setWasUsed();
#endif

  return 1;
}

void BaseTex::destroy()
{
  release();
#if DAGOR_DBGLEVEL > 1
  if (!wasUsed())
    NAU_LOG_WARNING("texture {:p}, of size {}x{}x{} total={}bytes, name={} was destroyed but was never used "
            "in rendering",
      this, width, height, depth, tex.memSize, getResName());
#elif DAGOR_DBGLEVEL > 0
  if (!wasUsed())
    NAU_LOG_DEBUG("texture {:p}, of size {}x{}x{} total={}bytes, name={} was destroyed but was never used in "
           "rendering",
      this, width, height, depth, tex.memSize, getResName());
#endif
  destroyObject();
}

int BaseTex::generateMips()
{
  STORE_RETURN_ADDRESS();
  if (mipLevels > 1)
  {
    get_device().getContext().generateMipmaps(tex.image);
  }
  return 1;
}

bool BaseTex::setReloadCallback(IReloadData *_rld)
{
  rld.reset(_rld);
  return true;
}

void D3DTextures::release(uint64_t progress)
{
  auto &device = get_device();
  auto &ctx = device.getContext();

  if (stagingMemory)
  {
    ctx.freeMemory(stagingMemory);
    stagingMemory = HostDeviceSharedMemoryRegion{};
  }

  if (progress)
  {
    ctx.waitForProgress(progress);
  }

  if (image)
  {
    ctx.destroyImage(image);
    image = nullptr;
  }
}

void BaseTex::resetTex()
{
  sampler.ptr = 0;
  if (!isStub() && tex.image)
    TEXQL_ON_RELEASE(this);
  tex.stagingMemory = HostDeviceSharedMemoryRegion{};
  waitProgress = 0;
  tex.image = nullptr;
}

void BaseTex::releaseTex()
{
  STORE_RETURN_ADDRESS();
  notify_delete(this, srvBindingStages, uavBindingStages, getRtvBinding(), getDsvBinding());
  sampler.ptr = 0;
  if (isStub())
    tex.image = nullptr;
  else if (tex.image)
    TEXQL_ON_RELEASE(this);
  tex.release(waitProgress);
  waitProgress = 0;
}

#if _TARGET_XBOX
void BaseTex::lockimgXboxLinearLayout(void **pointer, int &stride, int level, int face, uint32_t flags)
{
  auto &device = get_device();
  auto &ctx = device.getContext();

  if (!pointer)
  {
    ctx.beginCPUTextureAccess(tex.image);

    if (flags & TEXLOCK_READ)
      waitProgress = ctx.getRecordingFenceProgress();

    return;
  }

  if ((flags & TEXLOCK_READ) && (cflg & (TEXCF_RTARGET | TEXCF_UNORDERED)))
  {
    if (!waitAndResetProgress())
    {
      NAU_LOG_DEBUG("DX12: Blocking read back of <{}>", getTexName());
      ctx.beginCPUTextureAccess(tex.image);
      // not previously waited on, need to block
      ctx.wait();
    }
  }
  else
    ctx.beginCPUTextureAccess(tex.image);

  auto mem = tex.image->getMemory();
  auto memAccessComputer = tex.image->getAccessComputer();
  auto offset = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, face, 0);
  auto offset2 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 1, face, 0);
  stride = offset2 - offset;
  *pointer = mem.asPointer() + offset;
  lockFlags = flags;
}
#endif

int BaseTex::lockimg(void **pointer, int &stride, int level, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_TEX != resType)
    return 0;

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    lockimgXboxLinearLayout(pointer, stride, level, 0, flags);
    return 1;
  }
#endif

  if (!tex.image && (flags & TEXLOCK_WRITE) && !(flags & TEXLOCK_READ) &&
      !create_tex2d(tex, this, width, height, mipLevels, false, nullptr))
  {
    NAU_LOG_ERROR("failed to auto-create tex.tex2D on lockImg");
    return 0;
  }
  stride = 0;

  uint32_t prevFlags = lockFlags;
  if (cflg & (TEXCF_RTARGET | TEXCF_UNORDERED))
  {
#if DAGOR_DBGLEVEL > 0
    setWasUsed();
#endif
    lockFlags = 0;
    if ((getFormat().isDepth()) && !(flags & TEXLOCK_COPY_STAGING))
    {
      NAU_LOG_ERROR("can't lock depth format");
      return 0;
    }

    if (copySubresourcesToStaging(pointer, stride, level, flags, prevFlags))
      return 1;

    if (!pointer)
    {
      lockFlags = TEX_COPIED;
      stride = 0;
      return 1;
    }
  }
  else
  {
    lockFlags = flags;
    if (pointer != nullptr)
      *pointer = nullptr;
    else if (flags & TEXLOCK_RWMASK)
    {
      NAU_LOG_ERROR("nullptr in lockimg");
      return 0;
    }
  }

  if (flags & TEXLOCK_RWMASK)
  {
    NAU_ASSERT_RETURN(tex.image, 0);
    lockedSubRes = calculate_subresource_index(level, 0, 0, mipLevels, 1);

    const uint64_t offset = prepareReadWriteStagingMemoryAndGetOffset(flags);

    if (!tex.stagingMemory)
      return 0;

    fillLockedLevelInfo(level, offset);

    // fast check is ok here
    if (waitProgress)
    {
      if (flags & TEXLOCK_NOSYSLOCK && waitProgress > get_device().getContext().getCompletedFenceProgress())
      {
        if (prevFlags == TEX_COPIED)
          lockFlags = TEX_COPIED;
        return 0;
      }
      else
        waitAndResetProgress();
    }

    *pointer = lockMsr.ptr;
    stride = lockMsr.rowPitch;
  }

  lockFlags = flags;
  return 1;
}

uint64_t BaseTex::prepareReadWriteStagingMemoryAndGetOffset(uint32_t flags)
{
  if (flags & TEXLOCK_DISCARD)
  {
    if (tex.stagingMemory)
      free_and_reset_staging_memory(tex.stagingMemory);
    auto size = calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(lockedSubRes, 1)); // -V522 12 line above
                                                                                                            // tex.image checked
    tex.stagingMemory = get_device().allocateTemporaryUploadMemory(size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    return 0;
  }

  if (!tex.stagingMemory)
    tex.stagingMemory = allocate_read_write_staging_memory(tex.image, tex.image->getSubresourceRange());

  return calculate_texture_staging_buffer_size(*tex.image, SubresourceRange::make(0, lockedSubRes));
}

int BaseTex::unlockimg()
{
  STORE_RETURN_ADDRESS();
  auto &ctx = get_device().getContext();
#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    ctx.endCPUTextureAccess(tex.image);
    lockFlags = 0;
    return 1;
  }
#endif

  if (RES3D_TEX == resType)
  {
    unlockimgRes3d();
    return 1;
  }

  if (RES3D_CUBETEX != resType)
    return 0;

  if (tex.stagingMemory)
  {
    if (lockFlags & TEXLOCK_WRITE)
    {
      BufferImageCopy copy = calculate_texture_subresource_copy_info(*tex.image, lockedSubRes);

      tex.stagingMemory.flush();
      // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
      // execute the upload on the upload queue, it will move it to the graphics queue.
      ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, 0 != (lockFlags & TEXLOCK_DISCARD));
    }
    free_and_reset_staging_memory(tex.stagingMemory);
  }

  lockFlags = 0;
  return 1;
}

void BaseTex::unlockimgRes3d()
{
  auto &ctx = get_device().getContext();

  // only ever used when texture is RTV or UAV and staging is assumed to be fully written
  if (lockFlags == TEXLOCK_COPY_STAGING && tex.image != nullptr)
  {
    tex.stagingMemory.flush();
    TextureMipsCopyInfo copies = calculate_texture_mips_copy_info(*tex.image, mipLevels);
    ctx.uploadToImage(tex.image, copies.data(), mipLevels, tex.stagingMemory, DeviceQueueType::UPLOAD, false);
    return;
  }

  if ((cflg & (TEXCF_RTARGET | TEXCF_UNORDERED)) && (lockFlags == TEX_COPIED))
    return;

  ddsx::Header *hdr = (ddsx::Header *)texCopy.data();
  if ((cflg & TEXCF_SYSTEXCOPY) && lockMsr.ptr && hdr && !hdr->compressionType())
  {
    auto rpitch = hdr->getSurfacePitch(lockedSubRes); // for normal tex subres id and mip level are the same
    auto h = hdr->getSurfaceScanlines(lockedSubRes);
    uint8_t *src = (uint8_t *)lockMsr.ptr;
    uint8_t *dest = texCopy.data() + sizeof(ddsx::Header);

    for (uint32_t i = 0; i < lockedSubRes; i++)
      dest += hdr->getSurfacePitch(i) * hdr->getSurfaceScanlines(i);
    NAU_ASSERT(dest < texCopy.data() + sizeof(ddsx::Header) + hdr->memSz);

    NAU_ASSERT(rpitch <= lockMsr.rowPitch, "{}x{}: tex.pitch={} copy.pitch={}, level={}", width, height, lockMsr.rowPitch, rpitch,
      lockedSubRes);
    for (int y = 0; y < h; y++, dest += rpitch)
      memcpy(dest, src + y * lockMsr.rowPitch, rpitch);
    VERBOSE_DEBUG("%s %dx%d updated DDSx for TEXCF_SYSTEXCOPY", getResName(), hdr->w, hdr->h, data_size(texCopy));
  }

  if (tex.stagingMemory && tex.image)
  {
    if (lockFlags & TEXLOCK_DISCARD)
    {
      // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
      // execute the upload on the upload queue, it will move it to the graphics queue.
      tex.stagingMemory.flush();
      auto copy = calculate_texture_subresource_copy_info(*tex.image, lockedSubRes);
      ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, true);

      free_and_reset_staging_memory(tex.stagingMemory);
    }
    else if ((lockFlags & TEXLOCK_DONOTUPDATEON9EXBYDEFAULT) != 0)
      stateBitSet.set(unlock_image_is_upload_skipped, true);
    else if ((lockFlags & (TEXLOCK_RWMASK | TEXLOCK_UPDATEFROMSYSTEX)) != 0 && !(cflg & TEXCF_RTARGET))
    {
      // Sometimes we use TEXLOCK_DONOTUPDATEON9EXBYDEFAULT flag and don't copy locked subresource on unlock.
      // Copy all mips is required after that action.
      tex.stagingMemory.flush();
      auto copies = calculate_texture_mips_copy_info(*tex.image, mipLevels);
      const eastl::span<BufferImageCopy> fullResource{copies};
      const eastl::span<BufferImageCopy> oneSubresource{&copies[calculate_mip_slice_from_index(lockedSubRes, mipLevels)], 1};
      const auto uploadRegions = stateBitSet.test(unlock_image_is_upload_skipped) ? fullResource : oneSubresource;
      ctx.uploadToImage(tex.image, uploadRegions.data(), uploadRegions.size(), tex.stagingMemory, DeviceQueueType::UPLOAD, false);
      stateBitSet.set(unlock_image_is_upload_skipped, false);
    }
  }

  if (tex.stagingMemory && (lockFlags & TEXLOCK_DELSYSMEMCOPY) && !(cflg & TEXCF_DYNAMIC))
  {
    NAU_ASSERT(!stateBitSet.test(unlock_image_is_upload_skipped));
    free_and_reset_staging_memory(tex.stagingMemory);
  }

  lockFlags = 0;
  return;
}

int BaseTex::lockimg(void **pointer, int &stride, int face, int level, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_CUBETEX != resType)
    return 0;

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    lockimgXboxLinearLayout(pointer, stride, level, face, flags);
    return 1;
  }
#endif

  NAU_ASSERT(!(cflg & TEXCF_SYSTEXCOPY), "cube texture with system copy not implemented yet");
  NAU_ASSERT(!(flags & TEXLOCK_DISCARD), "Discard for cube texture is not implemented yet");
  NAU_ASSERT(!((flags & TEXCF_RTARGET) && (flags & TEXLOCK_WRITE)), "you can not write to a render "
                                                                   "target");

  if ((flags & TEXLOCK_RWMASK) == 0)
    return 0;

  lockFlags = flags;

  if (tex.stagingMemory)
    return 1;

  lockedSubRes = calculate_subresource_index(level, face, 0, mipLevels, 6);
  auto subResInfo = calculate_texture_mip_info(*tex.image, MipMapIndex::make(level));

  tex.stagingMemory =
    get_device().allocatePersistentBidirectionalMemory(subResInfo.totalByteSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
  if (!tex.stagingMemory)
    return 0;

  auto &ctx = get_device().getContext();

  if (flags & TEXLOCK_READ)
  {
    BufferImageCopy copy{};
    copy.layout.Footprint = subResInfo.footprint;
    copy.subresourceIndex = lockedSubRes;
    waitProgress = ctx.readBackFromImage(tex.stagingMemory, &copy, 1, tex.image, DeviceQueueType::READ_BACK);
    if (pointer)
      NAU_LOG_WARNING("DX12: blocking texture readback issued");
  }

  if (pointer)
  {
    waitAndResetProgress();
    tex.stagingMemory.invalidate();
    *pointer = tex.stagingMemory.pointer;
    stride = subResInfo.footprint.RowPitch;
  }

  return 1;
}

int BaseTex::lockbox(void **data, int &row_pitch, int &slice_pitch, int level, unsigned flags)
{
  STORE_RETURN_ADDRESS();
  if (RES3D_VOLTEX != resType)
  {
    NAU_LOG_WARNING("DX12: called lockbox on a non volume texture");
    return 0;
  }

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    if (data)
    {
      if (flags & TEXLOCK_READ)
      {
        if (!waitAndResetProgress())
        {
          NAU_LOG_DEBUG("DX12: Blocking read back of <{}>", getTexName());
          ctx.beginCPUTextureAccess(tex.image);
          // not previously waited on, need to block
          ctx.wait();
        }
      }
      else
      {
        ctx.beginCPUTextureAccess(tex.image);
      }
      auto mem = tex.image->getMemory();
      auto memAccessComputer = tex.image->getAccessComputer();
      auto offset = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, 0, 0);
      auto offset2 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 1, 0, 0);
      auto offset3 = get_texel_element_offset_bytes(memAccessComputer, 0, level, 0, 0, 1, 0);
      row_pitch = offset2 - offset;
      slice_pitch = offset3 - offset;
      *data = mem.asPointer() + offset;
      lockFlags = flags;
    }
    else
    {
      ctx.beginCPUTextureAccess(tex.image);

      if (flags & TEXLOCK_READ)
      {
        waitProgress = ctx.getRecordingFenceProgress();
      }
    }
    return 1;
  }
#endif

  NAU_ASSERT(!(flags & TEXLOCK_DISCARD), "Discard for volume texture is not implemented yet");
  NAU_ASSERT(data != nullptr, "for lockbox you need to provide a output pointer");
  NAU_ASSERT(!((cflg & TEXCF_RTARGET) && (flags & TEXLOCK_WRITE)), "you can not write to a render "
                                                                  "target");

  if ((flags & TEXLOCK_RWMASK) && data)
  {
    lockedSubRes = level;
    lockFlags = flags;

    NAU_ASSERT(!tex.stagingMemory);

    auto subResInfo = calculate_texture_subresource_info(*tex.image, SubresourceIndex::make(lockedSubRes));
    // only get a buffer large enough to hold the locked level
    tex.stagingMemory = device.allocatePersistentBidirectionalMemory(subResInfo.totalByteSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    if (!tex.stagingMemory)
      return 0;

    if (flags & TEXLOCK_READ)
    {
      BufferImageCopy copy{};
      copy.layout.Footprint = subResInfo.footprint;
      copy.subresourceIndex = lockedSubRes;
      NAU_LOG_WARNING("DX12: blocking texture readback issued");
      ctx.waitForProgress(ctx.readBackFromImage(tex.stagingMemory, &copy, 1, tex.image, DeviceQueueType::READ_BACK));
      tex.stagingMemory.invalidate();
    }
    *data = lockMsr.ptr = tex.stagingMemory.pointer;
    row_pitch = lockMsr.rowPitch = subResInfo.footprint.RowPitch;
    slice_pitch = lockMsr.slicePitch = subResInfo.footprint.RowPitch * subResInfo.rowCount;
    lockMsr.memSize = static_cast<uint32_t>(subResInfo.totalByteSize);
    return 1;
  }
  else
  {
    NAU_LOG_WARNING("DX12: lockbox called with no effective action (either no read/write flag or null "
            "pointer passed)");
    return 0;
  }
}

int BaseTex::unlockbox()
{
  STORE_RETURN_ADDRESS();
  if (RES3D_VOLTEX != resType)
    return 0;

  NAU_ASSERT(lockFlags != 0, "Unlock without any lock before?");

  auto &device = get_device();
  auto &ctx = device.getContext();

#if _TARGET_XBOX
  if (cflg & TEXCF_LINEAR_LAYOUT)
  {
    ctx.endCPUTextureAccess(tex.image);
    lockFlags = 0;
    return 1;
  }
#endif

  if ((cflg & TEXCF_SYSTEXCOPY) && data_size(texCopy) && lockMsr.ptr)
  {
    ddsx::Header &hdr = *(ddsx::Header *)texCopy.data();
    NAU_ASSERT(!hdr.compressionType());

    auto rpitch = hdr.getSurfacePitch(lockedSubRes); // for vol tex sub res index is the same as mip level
    auto h = hdr.getSurfaceScanlines(lockedSubRes);
    auto d = Vectormath::max<uint32_t>(depth >> lockedSubRes, 1);
    uint8_t *src = (uint8_t *)lockMsr.ptr;
    uint8_t *dest = texCopy.data() + sizeof(ddsx::Header);

    for (int i = 0; i < lockedSubRes; i++)
      dest += hdr.getSurfacePitch(i) * hdr.getSurfaceScanlines(i) * Vectormath::max(depth >> i, 1);
    NAU_ASSERT(dest < texCopy.data() + sizeof(ddsx::Header) + hdr.memSz);

    NAU_ASSERT(rpitch <= lockMsr.rowPitch && rpitch * h <= lockMsr.slicePitch, "{}x{}x{}: tex.pitch={},{} copy.pitch={},{}, level={}",
      width, height, depth, lockMsr.rowPitch, lockMsr.slicePitch, rpitch, rpitch * h, lockedSubRes);
    for (int di = 0; di < d; di++, src += lockMsr.slicePitch)
      for (int y = 0; y < h; y++, dest += rpitch)
        memcpy(dest, src + y * lockMsr.rowPitch, rpitch);
    VERBOSE_DEBUG("%s %dx%dx%d updated DDSx for TEXCF_SYSTEXCOPY", getResName(), hdr.w, hdr.h, hdr.depth, data_size(texCopy));
  }
  lockMsr.ptr = nullptr;

  if (tex.stagingMemory)
  {
    if (lockFlags & TEXLOCK_WRITE)
    {
      tex.stagingMemory.flush();
      BufferImageCopy copy = calculate_texture_subresource_copy_info(*tex.image, lockedSubRes);
      // Allow upload happen on the upload queue as a discard upload. If the driver can not safely
      // execute the upload on the upload queue, it will move it to the graphics queue.
      ctx.uploadToImage(tex.image, &copy, 1, tex.stagingMemory, DeviceQueueType::UPLOAD, 0 != (lockFlags & TEXLOCK_DISCARD));
    }

    free_and_reset_staging_memory(tex.stagingMemory);
  }

  lockFlags = 0;
  return 1;
}

int BaseTex::ressize() const
{
  if ((cflg & TEXCF_TILED_RESOURCE) != 0)
    return 0;
  if (tex.image && tex.image->isAliased())
    return 0;
  if (isStub())
    return 0;

  Extent3D ext{width, height, resType == RES3D_VOLTEX ? depth : 1u};
  return static_cast<int>(
    calculate_texture_staging_buffer_size(ext, MipMapCount::make(mipLevels), fmt, SubresourceRange::make(0, mipLevels)) *
    getArrayCount().count());
}

int BaseTex::getinfo(TextureInfo &ti, int level) const
{
  level = Vectormath::clamp<int>(level, 0, mipLevels - 1);

  ti.w = Vectormath::max<uint32_t>(1u, width >> level);
  ti.h = Vectormath::max<uint32_t>(1u, height >> level);
  switch (resType)
  {
    case RES3D_CUBETEX:
      ti.d = 1;
      ti.a = 6;
      break;
    case RES3D_CUBEARRTEX:
    case RES3D_ARRTEX:
      ti.d = 1;
      ti.a = getArrayCount().count();
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
  ti.resType = resType;
  ti.cflg = cflg;
  return 1;
}

int BaseTex::texaddr(int a)
{
  samplerState.setW(translate_texture_address_mode_to_dx12(a));
  samplerState.setV(translate_texture_address_mode_to_dx12(a));
  samplerState.setU(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddru(int a)
{
  samplerState.setU(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddrv(int a)
{
  samplerState.setV(translate_texture_address_mode_to_dx12(a));
  notifySamplerChange();
  return 1;
}

int BaseTex::texaddrw(int a)
{
  if (RES3D_VOLTEX == resType)
  {
    samplerState.setW(translate_texture_address_mode_to_dx12(a));
    notifySamplerChange();
    return 1;
  }
  return 0;
}

int BaseTex::texbordercolor(nau::math::E3DCOLOR c)
{
  samplerState.setBorder(c);
  notifySamplerChange();
  return 1;
}

int BaseTex::texfilter(int m)
{
  samplerState.isCompare = m == TEXFILTER_COMPARE;
  samplerState.setMinFilter(translate_filter_type_to_dx12(m));
  samplerState.setMagFilter(translate_filter_type_to_dx12(m));
  notifySamplerChange();
  return 1;
}

int BaseTex::texmipmap(int m)
{
  samplerState.setMip(translate_mip_filter_type_to_dx12(m));
  notifySamplerChange();
  return 1;
}

int BaseTex::texlod(float mipmaplod)
{
  samplerState.setBias(mipmaplod);
  notifySamplerChange();
  return 1;
}

int BaseTex::texmiplevel(int minlevel, int maxlevel)
{
  maxMipLevel = (minlevel >= 0) ? minlevel : 0;
  minMipLevel = (maxlevel >= 0) ? maxlevel : (mipLevels - 1);
  notifySrvChange();
  return 1;
}

int BaseTex::setAnisotropy(int level)
{
  samplerState.setAniso(Vectormath::clamp<int>(level, 1, 16));
  notifySamplerChange();
  return 1;
}

static Texture *create_tex_internal(TexImage32 *img, int w, int h, int flg, int levels, const char8_t *stat_name, Texture *baseTexture)
{
  NAU_ASSERT_RETURN(d3d::check_texformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    NAU_LOG_ERROR("create_tex: can not create dynamic render target");
    return nullptr;
  }
  if (img)
  {
    w = img->w;
    h = img->h;
  }

  const Driver3dDesc &dd = d3d::get_driver_desc();
  w = Vectormath::clamp<int>(w, dd.mintexw, dd.maxtexw);
  h = Vectormath::clamp<int>(h, dd.mintexh, dd.maxtexh);

  levels = count_mips_if_needed(w, h, flg, levels);

  if (img)
  {
    levels = 1;

    if (0 == w && 0 == h)
    {
      w = img->w;
      h = img->h;
    }

    if ((w != img->w) || (h != img->h))
    {
      NAU_LOG_ERROR("create_tex: image size differs from texture size ({}x{} != {}x{})", img->w, img->h, w, h);
      img = nullptr; // abort copying
    }

    if (FormatStore::fromCreateFlags(flg).getBytesPerPixelBlock() != 4)
      img = nullptr;
  }

  // TODO: check for preallocated RT (with requested, not adjusted tex dimensions)

  auto tex = get_device().newTextureObject(RES3D_TEX, flg);

  tex->setParams(w, h, 1, levels, stat_name);
  if (tex->cflg & TEXCF_SYSTEXCOPY)
  {
    if (img)
    {
      uint32_t memSz = w * h * 4;
      clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + memSz);

      ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
      memset(&hdr, 0, sizeof(hdr));
      hdr.label = _MAKE4C('DDSx');
      hdr.d3dFormat = D3DFMT_A8R8G8B8;
      hdr.flags |= ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0) ? hdr.FLG_GAMMA_EQ_1 : 0;
      hdr.w = w;
      hdr.h = h;
      hdr.levels = 1;
      hdr.bitsPerPixel = 32;
      hdr.memSz = memSz;
      /*sysCopyQualityId*/ hdr.hqPartLevels = 0;

      memcpy(tex->texCopy.data() + sizeof(hdr), img + 1, memSz);
      VERBOSE_DEBUG("%s %dx%d stored DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
    }
    else if (tex->cflg & TEXCF_LOADONCE)
    {
      uint32_t memSz = tex->ressize();
      clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + memSz);
      mem_set_0(tex->texCopy);

      ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
      hdr.label = _MAKE4C('DDSx');
      hdr.d3dFormat = texfmt_to_d3dformat(tex->cflg & TEXFMT_MASK);
      hdr.flags |= ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0) ? hdr.FLG_GAMMA_EQ_1 : 0;
      hdr.w = w;
      hdr.h = h;
      hdr.levels = levels;
      hdr.bitsPerPixel = tex->getFormat().getBytesPerPixelBlock();
      if (hdr.d3dFormat == D3DFMT_DXT1)
        hdr.dxtShift = 3;
      else if (hdr.d3dFormat == D3DFMT_DXT3 || hdr.d3dFormat == D3DFMT_DXT5)
        hdr.dxtShift = 4;
      if (hdr.dxtShift)
        hdr.bitsPerPixel = 0;
      hdr.memSz = memSz;
      /*sysCopyQualityId*/ hdr.hqPartLevels = 0;

      VERBOSE_DEBUG("%s %dx%d reserved DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
    }
    else
      tex->cflg &= ~TEXCF_SYSTEXCOPY;
  }

  // only 1 mip
  BaseTex::ImageMem idata;
  if (img)
  {
    idata.ptr = img + 1;
    idata.rowPitch = w * 4;
    idata.slicePitch = 0; // -V1048
    idata.memSize = w * h * 4;
  }

  if (!create_tex2d(tex->tex, tex, w, h, levels, false, img ? &idata : nullptr, 1, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

Texture *d3d::create_tex(TexImage32 *img, int w, int h, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_tex_internal(img, w, h, flg, levels, stat_name, nullptr);
}

static CubeTexture *create_cubetex_internal(int size, int flg, int levels, const char8_t *stat_name, CubeTexture *baseTexture)
{
  NAU_ASSERT_RETURN(d3d::check_cubetexformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    NAU_LOG_ERROR("create_cubtex: can not create dynamic render target");
    return nullptr;
  }

  const Driver3dDesc &dd = d3d::get_driver_desc();
  size = nau::math::get_bigger_pow2(Vectormath::clamp<int>(size, dd.mincubesize, dd.maxcubesize));

  levels = count_mips_if_needed(size, size, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_CUBETEX, flg);
  tex->setParams(size, size, 1, levels, stat_name);

  if (!create_tex2d(tex->tex, tex, size, size, levels, true, nullptr, 1, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

CubeTexture *d3d::create_cubetex(int size, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cubetex_internal(size, flg, levels, stat_name, nullptr);
}

static VolTexture *create_voltex_internal(int w, int h, int d, int flg, int levels, const char8_t *stat_name, VolTexture *baseTexture)
{
  NAU_ASSERT_RETURN(d3d::check_voltexformat(flg), nullptr);

  if ((flg & (TEXCF_RTARGET | TEXCF_DYNAMIC)) == (TEXCF_RTARGET | TEXCF_DYNAMIC))
  {
    NAU_LOG_ERROR("create_voltex: can not create dynamic render target");
    return nullptr;
  }

  levels = count_mips_if_needed(w, h, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_VOLTEX, flg);
  tex->setParams(w, h, d, levels, stat_name);

  if (!create_tex3d(tex->tex, tex, w, h, d, flg, levels, nullptr, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();
  if (tex->cflg & TEXCF_SYSTEXCOPY)
  {
    clear_and_resize(tex->texCopy, sizeof(ddsx::Header) + tex->tex.memSize);
    mem_set_0(tex->texCopy);

    ddsx::Header &hdr = *(ddsx::Header *)tex->texCopy.data();
    hdr.label = _MAKE4C('DDSx');
    hdr.d3dFormat = texfmt_to_d3dformat(tex->cflg & TEXFMT_MASK);
    hdr.w = w;
    hdr.h = h;
    hdr.depth = d;
    hdr.levels = levels;
    hdr.bitsPerPixel = tex->getFormat().getBytesPerPixelBlock();
    if (hdr.d3dFormat == D3DFMT_DXT1)
      hdr.dxtShift = 3;
    else if (hdr.d3dFormat == D3DFMT_DXT3 || hdr.d3dFormat == D3DFMT_DXT5)
      hdr.dxtShift = 4;
    if (hdr.dxtShift)
      hdr.bitsPerPixel = 0;
    hdr.flags = hdr.FLG_VOLTEX;
    if ((tex->cflg & (TEXCF_SRGBREAD | TEXCF_SRGBWRITE)) == 0)
      hdr.flags |= hdr.FLG_GAMMA_EQ_1;
    hdr.memSz = tex->tex.memSize;
    /*sysCopyQualityId*/ hdr.hqPartLevels = 0;
    VERBOSE_DEBUG("%s %dx%d reserved DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(tex->texCopy));
  }

  return tex;
}

VolTexture *d3d::create_voltex(int w, int h, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_voltex_internal(w, h, d, flg, levels, stat_name, nullptr);
}

static ArrayTexture *create_array_tex_internal(int w, int h, int d, int flg, int levels, const char8_t *stat_name,
  ArrayTexture *baseTexture)
{
  NAU_ASSERT_RETURN(d3d::check_texformat(flg), nullptr);

  levels = count_mips_if_needed(w, h, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_ARRTEX, flg);
  tex->setParams(w, h, d, levels, stat_name);

  if (!create_tex2d(tex->tex, tex, w, h, levels, false, nullptr, d, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

ArrayTexture *d3d::create_array_tex(int w, int h, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_array_tex_internal(w, h, d, flg, levels, stat_name, nullptr);
}

static ArrayTexture *create_cube_array_tex_internal(int side, int d, int flg, int levels, const char8_t *stat_name,
  ArrayTexture *baseTexture)
{
  NAU_ASSERT_RETURN(d3d::check_cubetexformat(flg), nullptr);

  levels = count_mips_if_needed(side, side, flg, levels);

  auto tex = get_device().newTextureObject(RES3D_ARRTEX, flg);
  tex->setParams(side, side, d, levels, stat_name);
  tex->setIsArrayCube(true);

  if (!create_tex2d(tex->tex, tex, side, side, levels, true, nullptr, d, static_cast<BaseTex *>(baseTexture)))
  {
    del_d3dres(tex);
    return nullptr;
  }

  tex->tex.memSize = tex->ressize();

  return tex;
}

ArrayTexture *d3d::create_cube_array_tex(int side, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cube_array_tex_internal(side, d, flg, levels, stat_name, nullptr);
}

// load compressed texture
BaseTexture *d3d::create_ddsx_tex(nau::iosys::IGenLoad &crd, int flg, int quality_id, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  ddsx::Header hdr;
  if (!crd.readExact(&hdr, sizeof(hdr)) || !hdr.checkLabel())
  {
    NAU_LOG_DEBUG("invalid DDSx format");
    return nullptr;
  }

  BaseTexture *tex = alloc_ddsx_tex(hdr, flg, quality_id, levels, stat_name);
  if (tex)
  {
    BaseTex *bt = (BaseTex *)tex;
    int st_pos = crd.tell();

    //NAU_ASSERT_AND_DO(hdr.hqPartLevels == 0, bt->cflg &= ~TEXCF_SYSTEXCOPY, "cannot use TEXCF_SYSTEXCOPY with base part of split texture!");
    NAU_ASSERT(hdr.hqPartLevels == 0, "cannot use TEXCF_SYSTEXCOPY with base part of split texture!");
    bt->cflg &= ~TEXCF_SYSTEXCOPY;

    if (bt->cflg & TEXCF_SYSTEXCOPY)
    {
      auto data_sz = hdr.packedSz ? hdr.packedSz : hdr.memSz;
      clear_and_resize(bt->texCopy, sizeof(hdr) + data_sz);
      memcpy(bt->texCopy.data(), &hdr, sizeof(hdr));
      /*sysCopyQualityId*/ ((ddsx::Header *)bt->texCopy.data())->hqPartLevels = quality_id;
      if (!crd.readExact(bt->texCopy.data() + sizeof(hdr), data_sz))
      {
        //NAU_LOG_ERROR_ctx("inconsistent input tex data, data_sz={} tex={}", data_sz, stat_name);
          NAU_LOG_ERROR("inconsistent input tex data, data_sz={} tex={}", data_sz, stat_name);
        del_d3dres(tex);
        return NULL;
      }
      VERBOSE_DEBUG("%s %dx%d stored DDSx (%d bytes) for TEXCF_SYSTEXCOPY", stat_name, hdr.w, hdr.h, data_size(bt->texCopy));
      nau::iosys::InPlaceMemLoadCB mcrd(bt->texCopy.data() + sizeof(hdr), data_sz);
      if (load_ddsx_tex_contents(tex, hdr, mcrd, quality_id))
        return tex;
    }
    else if (load_ddsx_tex_contents(tex, hdr, crd, quality_id))
      return tex;

    if (!nau::hal::is_main_thread())
    {
      for (unsigned attempt = 0, tries = 5, f = dagor_frame_no(); attempt < tries;)
        if (dagor_frame_no() < f + 1)
            nau::hal::sleep_msec(1);
        else
        {
          crd.seekto(st_pos);
          if (load_ddsx_tex_contents(tex, hdr, crd, quality_id))
          {
            NAU_LOG_DEBUG("finally loaded {} (attempt={})", stat_name, attempt + 1);
            return tex;
          }
          f = dagor_frame_no();
          attempt++;
        }
      return tex;
    }
    del_d3dres(tex);
  }
  return nullptr;
}

BaseTexture *d3d::alloc_ddsx_tex(const ddsx::Header &hdr, int flg, int q_id, int levels, const char8_t *stat_name, int stub_tex_idx)
{
  flg = implant_d3dformat(flg, hdr.d3dFormat);
  if (hdr.d3dFormat == D3DFMT_A4R4G4B4 || hdr.d3dFormat == D3DFMT_X4R4G4B4 || hdr.d3dFormat == D3DFMT_R5G6B5)
    flg = implant_d3dformat(flg, D3DFMT_A8R8G8B8);
  NAU_ASSERT((flg & TEXCF_RTARGET) == 0);
  flg |= (hdr.flags & hdr.FLG_GAMMA_EQ_1) ? 0 : TEXCF_SRGBREAD;

  if (levels <= 0)
    levels = hdr.levels;

  int resType;
  if (hdr.flags & ddsx::Header::FLG_CUBTEX)
    resType = RES3D_CUBETEX;
  else if (hdr.flags & ddsx::Header::FLG_VOLTEX)
    resType = RES3D_VOLTEX;
  else if (hdr.flags & ddsx::Header::FLG_ARRTEX)
    resType = RES3D_ARRTEX;
  else
    resType = RES3D_TEX;

  auto bt = drv3d_dx12::get_device().newTextureObject(resType, flg);

  int skip_levels = hdr.getSkipLevels(hdr.getSkipLevelsFromQ(q_id), levels);
  int w = Vectormath::max(hdr.w >> skip_levels, 1), h = Vectormath::max(hdr.h >> skip_levels, 1), d = Vectormath::max(hdr.depth >> skip_levels, 1);
  if (!(hdr.flags & hdr.FLG_VOLTEX))
    d = (hdr.flags & hdr.FLG_ARRTEX) ? hdr.depth : 1;

  bt->setParams(w, h, d, levels, stat_name);
  bt->stubTexIdx = stub_tex_idx;
  bt->setIsPreallocBeforeLoad(true);

  if (stub_tex_idx >= 0)
  {
    // static analysis says this could be null
    auto subtex = bt->getStubTex();
    if (subtex)
      bt->tex.image = subtex->tex.image;
  }

  return bt;
}

#if _TARGET_PC_WIN
unsigned d3d::pcwin32::get_texture_format(BaseTexture *tex)
{
  auto bt = getbasetex(tex);
  if (!bt)
    return 0;
  return bt->getFormat();
}
const char *d3d::pcwin32::get_texture_format_str(BaseTexture *tex)
{
  auto bt = getbasetex(tex);
  if (!bt)
    return nullptr;
  return bt->getFormat().getNameString();
}
void *d3d::pcwin32::get_native_surface(BaseTexture *)
{
  return nullptr; // TODO:: ((BaseTex*)tex)->tex.tex2D;
}
#endif

bool d3d::set_tex_usage_hint(int, int, int, const char *, unsigned int)
{
  //debug_ctx("n/a");
    NAU_LOG_DEBUG("n/a");
  return true;
}


Texture *d3d::alias_tex(Texture *baseTexture, TexImage32 *img, int w, int h, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_tex_internal(img, w, h, flg, levels, stat_name, static_cast<BaseTex *>(baseTexture));
}

CubeTexture *d3d::alias_cubetex(CubeTexture *baseTexture, int size, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cubetex_internal(size, flg, levels, stat_name, baseTexture);
}

VolTexture *d3d::alias_voltex(VolTexture *baseTexture, int w, int h, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_voltex_internal(w, h, d, flg, levels, stat_name, baseTexture);
}

ArrayTexture *d3d::alias_array_tex(ArrayTexture *baseTexture, int w, int h, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_array_tex_internal(w, h, d, flg, levels, stat_name, baseTexture);
}

ArrayTexture *d3d::alias_cube_array_tex(ArrayTexture *baseTexture, int side, int d, int flg, int levels, const char8_t *stat_name)
{
  STORE_RETURN_ADDRESS();
  return create_cube_array_tex_internal(side, d, flg, levels, stat_name, baseTexture);
}
