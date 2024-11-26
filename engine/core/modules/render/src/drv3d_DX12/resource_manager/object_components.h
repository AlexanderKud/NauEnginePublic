// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

//#include <dag/dag_vector.h>
#include "nau/supp/dag_comPtr.h"
#include "nau/threading/spin_lock.h"
#include "nau/threading/lock_guard.h"
#include "nau/math/dag_adjpow2.h"
#include "nau/generic/dag_objectPool.h"

#include "texture.h"
#include "extents.h"
#include "format_store.h"
#include "device_memory_class.h"
#include "image_view_state.h"
#include "buffer.h"
#include "resource_memory.h"
#include "texture_subresource_util.h"

#include "resource_manager/basic_components.h"
#include "resource_manager/image.h"


namespace drv3d_dx12
{

inline uint64_t calculate_texture_alignment(uint64_t width, uint32_t height, uint32_t depth, uint32_t samples,
  D3D12_TEXTURE_LAYOUT layout, D3D12_RESOURCE_FLAGS flags, drv3d_dx12::FormatStore format)
{
  if (D3D12_TEXTURE_LAYOUT_UNKNOWN != layout)
  {
    if (samples > 1)
    {
      return D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    else
    {
      return D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
  }

  if ((1 == samples) && ((D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) & flags))
  {
    return D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
  }

  uint32_t blockSizeX = 1, blockSizeY = 1;
  auto bytesPerBlock = format.getBytesPerPixelBlock(&blockSizeX, &blockSizeY);
  const uint32_t textureWidthInBlocks = (width + blockSizeX - 1) / blockSizeX;
  const uint32_t textureHeightInBlocks = (height + blockSizeY - 1) / blockSizeY;

  const uint32_t TILE_MEM_SIZE = 4 * 1024;
  const uint32_t blocksInTile = TILE_MEM_SIZE / bytesPerBlock;
  // MSDN documentation says about "near-equilateral" size for the tile
  const uint32_t blocksInTileX = nau::math::get_bigger_pow2(sqrt(blocksInTile));
  const uint32_t blocksInTileY = nau::math::get_bigger_pow2(blocksInTile / blocksInTileX);
  const uint32_t MAX_TILES_COUNT_FOR_SMALL_RES = 16;
  const uint32_t tilesCount = ((textureWidthInBlocks + blocksInTileX - 1) / blocksInTileX) *
                              ((textureHeightInBlocks + blocksInTileY - 1) / blocksInTileY) * depth;
  // This check is neccessary according to debug layer and dx12 documentation:
  // https://docs.microsoft.com/en-us/windows/win32/api/d3d12/ns-d3d12-d3d12_resource_desc#alignment
  const bool smallAmountOfTiles = tilesCount <= MAX_TILES_COUNT_FOR_SMALL_RES;

  if (samples > 1)
  {
    if (smallAmountOfTiles)
    {
      return D3D12_SMALL_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    else
    {
      return D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
    }
  }
  else
  {
    if (smallAmountOfTiles)
    {
      return D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT;
    }
    else
    {
      return D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    }
  }
}

// size is not important here
struct ImageInfo
{
  D3D12_RESOURCE_DIMENSION type = D3D12_RESOURCE_DIMENSION_UNKNOWN;
  D3D12_RESOURCE_FLAGS usage = D3D12_RESOURCE_FLAG_NONE;
  Extent3D size = {};
  ArrayLayerCount arrays;
  MipMapCount mips;
  FormatStore format;
  D3D12_TEXTURE_LAYOUT memoryLayout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  DeviceMemoryClass memoryClass = DeviceMemoryClass::INVALID;
  bool allocateSubresourceIDs = false;
  DXGI_SAMPLE_DESC sampleDesc = {1, 0};

  SubresourceCount getSubResourceCount() const { return SubresourcePerFormatPlaneCount::make(mips, arrays) * format.getPlanes(); }

  D3D12_RESOURCE_DESC asDesc() const
  {
    D3D12_RESOURCE_DESC desc;
    desc.SampleDesc = sampleDesc;
    desc.Layout = memoryLayout;
    desc.Flags = usage;
    desc.Format = format.asDxGiTextureCreateFormat();
    desc.Dimension = type;
    desc.Width = size.width;
    desc.MipLevels = mips.count();
    desc.Alignment = calculate_texture_alignment(size.width, size.height, size.depth, 1, memoryLayout, usage, format);
    switch (type)
    {
      default:
      case D3D12_RESOURCE_DIMENSION_UNKNOWN:
      case D3D12_RESOURCE_DIMENSION_BUFFER: NAU_FAILURE("DX12: Invalid texture dimension"); return desc;
      case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        desc.Height = 1;
        desc.DepthOrArraySize = arrays.count();
        break;
      case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        desc.Height = size.height;
        desc.DepthOrArraySize = arrays.count();
        break;
      case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        desc.Height = size.height;
        desc.DepthOrArraySize = size.depth;
        break;
    }
    return desc;
  }
};

struct ImageCreateResult
{
  Image *image;
  D3D12_RESOURCE_STATES state;
};
namespace resource_manager
{
class BufferObjectProvider : public GlobalSubresourceIdProvider
{
  using BaseType = GlobalSubresourceIdProvider;

protected:
  nau::threading::SpinLock bufferPoolGuard;
  ObjectPool<GenericBufferInterface> bufferPool;

  void shutdown()
  {
    lock_(bufferPoolGuard);
    auto sz = bufferPool.size();
    NAU_ASSERT(0 == sz, "DX12: Shutdown without destroying all buffers, there are still {} buffers alive!", sz);
#if DAGOR_DBGLEVEL > 0
    bufferPool.iterateAllocated([](auto buffer) { NAU_ASSERT(false, "DX12: Buffer <{}> still alive!", buffer->getBufName()); });
#endif
    G_UNUSED(sz);
    bufferPool.freeAll();

    BaseType::shutdown();
  }

public:
  template <typename... Args>
  GenericBufferInterface *newBufferObject(Args &&...args)
  {
    void *memory = nullptr;
    {
      lock_(bufferPoolGuard);
      memory = bufferPool.acquire();
    }
    // The constructor may kick off additional buffer allocations or frees so it can not be done
    // under the locked bufferPoolGuard
    auto buffer = ::new (memory) GenericBufferInterface(eastl::forward<Args>(args)...);
    if (!buffer->getDeviceBuffer() && !buffer->isStreamBuffer())
    {
      buffer->destroy();
      buffer = nullptr;
    }
    return buffer;
  }

  void deleteBufferObject(GenericBufferInterface *buffer)
  {
    // Have to destruct here to prevent possible recursive locking.
    buffer->~GenericBufferInterface();
    lock_(bufferPoolGuard);
    bufferPool.release(buffer);
  }

  template <typename T>
  void visitBufferObjects(T clb)
  {
    lock_(bufferPoolGuard);
    bufferPool.iterateAllocated(clb);
  }

  void reserveBufferObjects(size_t size)
  {
    lock_(bufferPoolGuard);
    bufferPool.reserve(size);
  }

  size_t getBufferObjectCapacity()
  {
    lock_(bufferPoolGuard);
    return bufferPool.capacity();
  }

  size_t getActiveBufferObjectCount()
  {
    lock_(bufferPoolGuard);
    return bufferPool.size();
  }
};

class TextureObjectProvider : public BufferObjectProvider
{
  using BaseType = BufferObjectProvider;

protected:
  struct PendingForCompletedFrameData : BaseType::PendingForCompletedFrameData
  {
    eastl::vector<TextureInterfaceBase *> freedTextureObjects;
  };

  ContainerMutexWrapper<ObjectPool<TextureInterfaceBase>, nau::threading::SpinLock> texturePool;

  void shutdown()
  {
    auto poolAccess = texturePool.access();
    auto sz = poolAccess->size();
    NAU_ASSERT(0 == sz, "DX12: Shutdown without destroying all textures, there are still {} textures alive!", sz);
#if DAGOR_DBGLEVEL > 0
    poolAccess->iterateAllocated([](auto tex) { NAU_ASSERT(false, "DX12: Texture <{}> still alive!", tex->getResName()); });
#endif
    G_UNUSED(sz);
    poolAccess->freeAll();

    BaseType::shutdown();
  }

  void completeFrameExecution(const CompletedFrameExecutionInfo &info, PendingForCompletedFrameData &data)
  {
    if (!data.freedTextureObjects.empty())
    {
      eastl::sort(begin(data.freedTextureObjects), end(data.freedTextureObjects));
      texturePool.access()->free(begin(data.freedTextureObjects), end(data.freedTextureObjects));
      data.freedTextureObjects.clear();
    }
    BaseType::completeFrameExecution(info, data);
  }

public:
  template <typename... Args>
  TextureInterfaceBase *newTextureObject(Args &&...args)
  {
    return texturePool.access()->allocate(eastl::forward<Args>(args)...);
  }

  template <typename T>
  void visitTextureObjects(T clb)
  {
    texturePool.access()->iterateAllocated(clb);
  }

  void deleteTextureObjectOnFrameCompletion(TextureInterfaceBase *texture)
  {
    accessRecodingPendingFrameCompletion<PendingForCompletedFrameData>(
      [=](auto &data) { data.freedTextureObjects.push_back(texture); });
  }

  void reserveTextureObjects(size_t count) { texturePool.access()->reserve(count); }

  size_t getTextureObjectCapacity() { return texturePool.access()->capacity(); }

  size_t getActiveTextureObjectCount() { return texturePool.access()->size(); }
};

class ImageObjectProvider : public TextureObjectProvider
{
  using BaseType = TextureObjectProvider;

protected:
  ContainerMutexWrapper<ObjectPool<Image>, nau::threading::SpinLock> imageObjectPool;

  ImageObjectProvider() = default;
  ~ImageObjectProvider() = default;
  ImageObjectProvider(const ImageObjectProvider &) = delete;
  ImageObjectProvider &operator=(const ImageObjectProvider &) = delete;
  ImageObjectProvider(ImageObjectProvider &&) = delete;
  ImageObjectProvider &operator=(ImageObjectProvider &&) = delete;

  void shutdown()
  {
    imageObjectPool.access()->freeAll();
    BaseType::shutdown();
  }

  void preRecovery()
  {
    imageObjectPool.access()->freeAll();
    BaseType::preRecovery();
  }

  template <typename... Args>
  Image *newImageObject(Args &&...args)
  {
    return imageObjectPool.access()->allocate(eastl::forward<Args>(args)...);
  }

  void deleteImageObject(Image *image) { imageObjectPool.access()->free(image); }

  void deleteImageObjects(eastl::span<Image *> images)
  {
    auto imageObjectPoolAccess = imageObjectPool.access();
    for (auto image : images)
    {
      imageObjectPoolAccess->free(image);
    }
  }

public:
  template <typename T>
  void visitImageObjects(T &&clb)
  {
    imageObjectPool.access()->iterateAllocated(eastl::forward<T>(clb));
  }

  bool isImageAlive(Image *img) { return imageObjectPool.access()->isAllocated(img); }
};

} // namespace resource_manager
} // namespace drv3d_dx12
