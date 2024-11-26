// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/span.h>

#include "command_list_storage.h"
#include "command_list_trace.h"
#include "command_list_trace_recorder.h"


namespace drv3d_dx12
{
struct Direct3D12Enviroment;
namespace debug
{
union Configuration;
}
} // namespace drv3d_dx12

namespace drv3d_dx12::debug::gpu_postmortem::dagor
{
class Trace
{
  struct TraceRecording
  {
    struct TraceConfig
    {
      using OperationTraceData = CommandListTraceRecorder::TraceID;
      struct EventTraceBase
      {
        constexpr CommandListTraceBase::TraceCompareResult compare(OperationTraceData) const
        {
          return CommandListTraceBase::TraceCompareResult::IgnoreForProgress;
        }
      };
      using EventTraceData = EventTraceBase;
      struct OperationTraceBase
      {
        OperationTraceData traceID;

        CommandListTraceBase::TraceCompareResult compare(OperationTraceData id) const
        {
          return traceID == id ? CommandListTraceBase::TraceCompareResult::Matching
                               : CommandListTraceBase::TraceCompareResult::Mismatching;
        }
      };
    };
    CommandListTraceRecorder traceRecodring;
    CommandListTrace<TraceConfig> traceList;

    TraceRecording() = default;
    TraceRecording(ID3D12Device3 *device) : traceRecodring{device} {}

    ~TraceRecording() = default;
    TraceRecording(TraceRecording &&) = default;
    TraceRecording &operator=(TraceRecording &&) = default;
  };
  CommandListStorage<TraceRecording> commandListTable;

  void walkBreadcumbs(call_stack::Reporter &reporter);
  static bool try_load(const Configuration &config, const Direct3D12Enviroment &d3d_env);


public:
  // Have to delete move constructor, otherwise compiler / templated stuff of variant tries to be smart and results in compile errors.
  Trace(Trace &&) = delete;
  Trace &operator=(Trace &&) = delete;
  Trace() = default;
  ~Trace()
  {
      //NAU_LOG_DEBUG("DX12: Shutting down DAGOR GPU Trace");
  }
  void configure();
  void beginCommandBuffer(ID3D12Device3 *device, ID3D12GraphicsCommandList *);
  void endCommandBuffer(ID3D12GraphicsCommandList *);
  void beginEvent(ID3D12GraphicsCommandList *, eastl::span<const char>, eastl::span<const char>);
  void endEvent(ID3D12GraphicsCommandList *, eastl::span<const char>);
  void marker(ID3D12GraphicsCommandList *, eastl::span<const char>);
  void draw(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &, const PipelineStageStateBase &,
    BasePipeline &, PipelineVariant &, uint32_t, uint32_t, uint32_t, uint32_t, D3D12_PRIMITIVE_TOPOLOGY);
  void drawIndexed(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &,
    const PipelineStageStateBase &, BasePipeline &, PipelineVariant &, uint32_t, uint32_t, uint32_t, int32_t, uint32_t,
    D3D12_PRIMITIVE_TOPOLOGY);
  void drawIndirect(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &,
    const PipelineStageStateBase &, BasePipeline &, PipelineVariant &, BufferResourceReferenceAndOffset);
  void drawIndexedIndirect(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &,
    const PipelineStageStateBase &, BasePipeline &, PipelineVariant &, BufferResourceReferenceAndOffset);
  void dispatchIndirect(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &, ComputePipeline &,
    BufferResourceReferenceAndOffset);
  void dispatch(const call_stack::CommandData &, D3DGraphicsCommandList *, const PipelineStageStateBase &, ComputePipeline &, uint32_t,
    uint32_t, uint32_t);
  void dispatchMesh(const call_stack::CommandData &debug_info, D3DGraphicsCommandList *cmd, const PipelineStageStateBase &vs,
    const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline, uint32_t x, uint32_t y, uint32_t z);
  void dispatchMeshIndirect(const call_stack::CommandData &debug_info, D3DGraphicsCommandList *cmd, const PipelineStageStateBase &vs,
    const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline, BufferResourceReferenceAndOffset args,
    BufferResourceReferenceAndOffset count, uint32_t max_count);
  void blit(const call_stack::CommandData &, D3DGraphicsCommandList *);
  void onDeviceRemoved(D3DDevice *device, HRESULT reason, call_stack::Reporter &reporter);
  bool sendGPUCrashDump(const char *, const void *, uintptr_t);
  void onDeviceShutdown();
  bool onDeviceSetup(ID3D12Device *, const Configuration &, const Direct3D12Enviroment &);

  template <typename T>
  static bool load(const Configuration &config, const Direct3D12Enviroment &d3d_env, T &target)
  {
    if (!try_load(config, d3d_env))
    {
      return false;
    }
    target.template emplace<Trace>();
    return true;
  }
};
} // namespace drv3d_dx12::debug::gpu_postmortem::dagor
