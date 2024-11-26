// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "nau/supp/dag_comPtr.h"

#include "driver.h"
#include "d3d12_error_handling.h"
#include "resource_memory.h"

#include "resource_manager/heap_components.h"


namespace drv3d_dx12::resource_manager
{

// Basic buffer with some common stuff, like handling differences in memory model between PC and consoles.
struct BasicBuffer
{
  ComPtr<ID3D12Resource> buffer;
  ResourceMemory bufferMemory;
  // PC needs extra data, on consoles bufferMemory has everything we need
#if !_TARGET_XBOX
  D3D12_GPU_VIRTUAL_ADDRESS gpuPointer = 0;
  uint8_t *pointer = nullptr;
#endif

#if _TARGET_XBOX
  D3D12_GPU_VIRTUAL_ADDRESS getGPUPointer() const { return bufferMemory.getAddress(); }
  uint8_t *getCPUPointer() const { return bufferMemory.asPointer(); }
#else
  D3D12_GPU_VIRTUAL_ADDRESS getGPUPointer() const { return gpuPointer; }
  uint8_t *getCPUPointer() const { return pointer; }
#endif

  HRESULT create(ID3D12Device *device, const D3D12_RESOURCE_DESC &desc, ResourceMemory mem, D3D12_RESOURCE_STATES initial_state,
    bool map)
  {
    HRESULT errorCode = S_OK;
#if _TARGET_XBOX
    G_UNUSED(map);
    errorCode =
      DX12_CHECK_RESULT_NO_OOM_CHECK(xbox_create_placed_resource(device, mem.getAddress(), desc, initial_state, nullptr, buffer));
    if (DX12_CHECK_FAIL(errorCode))
    {
      return errorCode;
    }
#else
    errorCode = DX12_CHECK_RESULT_NO_OOM_CHECK(
      device->CreatePlacedResource(mem.getHeap(), mem.getOffset(), &desc, initial_state, nullptr, COM_ARGS(&buffer)));
    if (DX12_CHECK_FAIL(errorCode))
    {
      return errorCode;
    }
    gpuPointer = buffer->GetGPUVirtualAddress();
    if (map)
    {
      D3D12_RANGE emptyRange{};
      buffer->Map(0, &emptyRange, reinterpret_cast<void **>(&pointer));
    }
    else
    {
      pointer = nullptr;
    }
#endif
    bufferMemory = mem;
    return errorCode;
  }

  void reset(ResourceMemoryHeapProvider *heap)
  {
    if (bufferMemory && 0 == bufferMemory.getHeapID().isAlias)
    {
      heap->free(bufferMemory);
      bufferMemory = {};
    }
    buffer.Reset();
  }

  explicit operator bool() const { return static_cast<bool>(buffer); }
};

} // namespace drv3d_dx12::resource_manager