// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "nau/threading/spin_lock.h"
#include "nau/threading/lock_guard.h"

#include "descriptor_heap.h"
#include "pipeline.h"
#include "format_store.h"

#include "resource_manager/object_components.h"


namespace drv3d_dx12
{
namespace resource_manager
{

// Allocs and frees do not take a lock as for textures, view create and free is always already guarded
// by the context lock.
class TextureDescriptorProvider : public ImageObjectProvider
{
  using BaseType = ImageObjectProvider;

  DescriptorHeap<ShaderResouceViewStagingPolicy> srvHeap;
  DescriptorHeap<RenderTargetViewPolicy> rtvHeap;
  DescriptorHeap<DepthStencilViewPolicy> dsvHeap;

public:
  struct SetupInfo : BaseType::SetupInfo
  {
    ID3D12Device *device;
  };

protected:
  TextureDescriptorProvider() = default;
  ~TextureDescriptorProvider() = default;
  TextureDescriptorProvider(const TextureDescriptorProvider &) = delete;
  TextureDescriptorProvider &operator=(const TextureDescriptorProvider &) = delete;
  TextureDescriptorProvider(TextureDescriptorProvider &&) = delete;
  TextureDescriptorProvider &operator=(TextureDescriptorProvider &&) = delete;

  void shutdown()
  {
    BaseType::shutdown();
    srvHeap.shutdown();
    rtvHeap.shutdown();
    dsvHeap.shutdown();
  }

  void preRecovery()
  {
    BaseType::preRecovery();
    srvHeap.shutdown();
    rtvHeap.shutdown();
    dsvHeap.shutdown();
  }

  void setup(const SetupInfo &info)
  {
    BaseType::setup(info);

    srvHeap.init(info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
    rtvHeap.init(info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV));
    dsvHeap.init(info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV));
  }

public:
  D3D12_CPU_DESCRIPTOR_HANDLE allocateTextureSRVDescriptor(ID3D12Device *device) { return srvHeap.allocate(device); }
  void freeTextureSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { srvHeap.free(descriptor); }
  void freeTextureSRVDescriptors(eastl::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors)
  {
    for (auto &&descriptor : descriptors)
    {
      srvHeap.free(descriptor);
    }
  }
  D3D12_CPU_DESCRIPTOR_HANDLE allocateTextureRTVDescriptor(ID3D12Device *device) { return rtvHeap.allocate(device); }
  void freeTextureRTVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { rtvHeap.free(descriptor); }
  void freeTextureRTVDescriptors(eastl::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors)
  {
    for (auto &&descriptor : descriptors)
    {
      rtvHeap.free(descriptor);
    }
  }
  D3D12_CPU_DESCRIPTOR_HANDLE allocateTextureDSVDescriptor(ID3D12Device *device) { return dsvHeap.allocate(device); }
  void freeTextureDSVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { dsvHeap.free(descriptor); }
  void freeTextureDSVDescriptors(eastl::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors)
  {
    for (auto &&descriptor : descriptors)
    {
      dsvHeap.free(descriptor);
    }
  }
};

class BufferDescriptorProvider : public TextureDescriptorProvider
{
  using BaseType = TextureDescriptorProvider;

  ContainerMutexWrapper<DescriptorHeap<ShaderResouceViewStagingPolicy>, nau::threading::SpinLock> srvHeap;

protected:
  BufferDescriptorProvider() = default;
  ~BufferDescriptorProvider() = default;
  BufferDescriptorProvider(const BufferDescriptorProvider &) = delete;
  BufferDescriptorProvider &operator=(const BufferDescriptorProvider &) = delete;
  BufferDescriptorProvider(BufferDescriptorProvider &&) = delete;
  BufferDescriptorProvider &operator=(BufferDescriptorProvider &&) = delete;

  void shutdown()
  {
    BaseType::shutdown();
    srvHeap.access()->shutdown();
  }

  void preRecovery()
  {
    BaseType::preRecovery();
    srvHeap.access()->shutdown();
  }

  void setup(const SetupInfo &info)
  {
    BaseType::setup(info);
    srvHeap.access()->init(info.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV));
  }

  eastl::unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE[]> createBufferSRVs(ID3D12Device *device, ID3D12Resource *buffer, uint32_t count,
    D3D12_SHADER_RESOURCE_VIEW_DESC desc)
  {
    auto descriptors = eastl::make_unique<D3D12_CPU_DESCRIPTOR_HANDLE[]>(count);
    auto srvHeapAccess = srvHeap.access();
    for (uint32_t i = 0; i < count; ++i)
    {
      descriptors[i] = srvHeapAccess->allocate(device);
      device->CreateShaderResourceView(buffer, &desc, descriptors[i]);
      desc.Buffer.FirstElement += desc.Buffer.NumElements;
    }
    return descriptors;
  }

  eastl::unique_ptr<D3D12_CPU_DESCRIPTOR_HANDLE[]> createBufferUAVs(ID3D12Device *device, ID3D12Resource *buffer, uint32_t count,
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc)
  {
    auto descriptors = eastl::make_unique<D3D12_CPU_DESCRIPTOR_HANDLE[]>(count);
    auto srvHeapAccess = srvHeap.access();
    for (uint32_t i = 0; i < count; ++i)
    {
      descriptors[i] = srvHeapAccess->allocate(device);
      device->CreateUnorderedAccessView(buffer, nullptr, &desc, descriptors[i]);
      desc.Buffer.FirstElement += desc.Buffer.NumElements;
    }
    return descriptors;
  }

public:
  D3D12_CPU_DESCRIPTOR_HANDLE allocateBufferSRVDescriptor(ID3D12Device *device) { return srvHeap.access()->allocate(device); }
  void freeBufferSRVDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE descriptor) { srvHeap.access()->free(descriptor); }
  void freeBufferSRVDescriptors(eastl::span<const D3D12_CPU_DESCRIPTOR_HANDLE> descriptors)
  {
    for (auto &&descriptor : descriptors)
    {
      srvHeap.access()->free(descriptor);
    }
  }

  void createBufferTextureSRV(ID3D12Device *device, BufferState &buffer, FormatStore format)
  {
    NAU_ASSERT(0 == (buffer.offset % format.getBytesPerPixelBlock()), "DX12: Offset {} has to be multiples of element size {}",
      buffer.offset, format.getBytesPerPixelBlock());
    D3D12_SHADER_RESOURCE_VIEW_DESC desc;
    desc.Format = format.asDxGiFormat();
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / format.getBytesPerPixelBlock();
    desc.Buffer.NumElements = buffer.size / format.getBytesPerPixelBlock();
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    buffer.srvs = createBufferSRVs(device, buffer.buffer, buffer.discardCount, desc);
  }

  void createBufferStructureSRV(ID3D12Device *device, BufferState &buffer, uint32_t struct_size)
  {
    NAU_ASSERT(0 == (buffer.offset % struct_size), "DX12: Offset {} has to be multiples of element size {}", buffer.offset,
      struct_size);
    auto srvs = eastl::make_unique<D3D12_CPU_DESCRIPTOR_HANDLE[]>(buffer.discardCount);
    D3D12_SHADER_RESOURCE_VIEW_DESC desc;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / struct_size;
    desc.Buffer.NumElements = buffer.size / struct_size;
    desc.Buffer.StructureByteStride = struct_size;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;

    buffer.srvs = createBufferSRVs(device, buffer.buffer, buffer.discardCount, desc);
  }

  void createBufferRawSRV(ID3D12Device *device, BufferState &buffer)
  {
    // RAW has a 16 byte offset alignment rule
    if (buffer.discardCount > 1)
    {
      NAU_ASSERT(0 == (buffer.size % 16), "DX12: Buffer size {} has to be multiples of 16", buffer.size);
    }
    NAU_ASSERT(0 == (buffer.offset % 16), "DX12: Offset {} has to be multiples of 16", buffer.offset);
    auto srvs = eastl::make_unique<D3D12_CPU_DESCRIPTOR_HANDLE[]>(buffer.discardCount);
    D3D12_SHADER_RESOURCE_VIEW_DESC desc;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    desc.Buffer.FirstElement = buffer.offset / 4;
    desc.Buffer.NumElements = buffer.size / 4;
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

    buffer.srvs = createBufferSRVs(device, buffer.buffer, buffer.discardCount, desc);
  }

  void createBufferTextureUAV(ID3D12Device *device, BufferState &buffer, FormatStore format)
  {
    NAU_ASSERT(0 == buffer.offset, "DX12: Buffers with offsets can't have UAVs");
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
    desc.Format = format.asDxGiFormat();
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = buffer.size / format.getBytesPerPixelBlock();
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.CounterOffsetInBytes = 0;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    buffer.uavs = createBufferUAVs(device, buffer.buffer, buffer.discardCount, desc);

    buffer.uavForClear.reset();
  }

  void createBufferStructureUAV(ID3D12Device *device, BufferState &buffer, uint32_t struct_size)
  {
    NAU_ASSERT(0 == buffer.offset, "DX12: Buffers with offsets can't have UAVs");
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
    desc.Format = DXGI_FORMAT_UNKNOWN;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = buffer.size / struct_size;
    desc.Buffer.StructureByteStride = struct_size;
    desc.Buffer.CounterOffsetInBytes = 0;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;

    buffer.uavs = createBufferUAVs(device, buffer.buffer, buffer.discardCount, desc);

    // need extra views for clearing that are formatted, DX12 does not allow clearing of
    // structured views
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Buffer.FirstElement = 0; // -V1048
    desc.Buffer.NumElements = buffer.size / sizeof(uint32_t);
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.CounterOffsetInBytes = 0;           // -V1048
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE; // -V1048

    buffer.uavForClear = createBufferUAVs(device, buffer.buffer, buffer.discardCount, desc);
  }

  void createBufferRawUAV(ID3D12Device *device, BufferState &buffer)
  {
    NAU_ASSERT(0 == buffer.offset, "DX12: Buffers with offsets can't have UAVs");
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
    desc.Format = DXGI_FORMAT_R32_TYPELESS;
    desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = buffer.size / 4;
    desc.Buffer.StructureByteStride = 0;
    desc.Buffer.CounterOffsetInBytes = 0;
    desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

    buffer.uavs = createBufferUAVs(device, buffer.buffer, buffer.discardCount, desc);
  }
};

} // namespace resource_manager
} // namespace drv3d_dx12
