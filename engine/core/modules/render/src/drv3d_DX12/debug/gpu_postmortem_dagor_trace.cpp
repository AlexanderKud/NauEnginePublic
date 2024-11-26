// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "drv3d_DX12/device.h"

using namespace drv3d_dx12;

void debug::gpu_postmortem::dagor::Trace::walkBreadcumbs(call_stack::Reporter &reporter)
{
  CommandListTraceBase::printLegend();

  commandListTable.visitAll([&reporter](auto cmd, auto &cmdList) {
    if (cmdList.traceRecodring.isCompleted())
    {
      NAU_LOG_DEBUG("DX12: Command Buffer was executed without error");
      NAU_LOG_DEBUG("DX12: Command Buffer: {:p}", (int)cmd);
    }
    else
    {
      uint32_t count = cmdList.traceRecodring.completedCount();

      if (0 == count)
      {
        NAU_LOG_DEBUG("DX12: Command Buffer execution was likely not started yet");
        NAU_LOG_DEBUG("DX12: Command Buffer: {:p}", (int)cmd);
        NAU_LOG_DEBUG("DX12: Breadcrumb count: {}", count);
        NAU_LOG_DEBUG("DX12: Last breadcrumb value: {}", cmdList.traceRecodring.indexToTraceID(0));
        auto vistorContext = cmdList.traceList.beginVisitation();
        cmdList.traceList.reportEverythingAsNotCompleted(vistorContext, cmdList.traceRecodring.indexToTraceID(1), reporter);
      }
      else
      {
        auto completedTraceID = cmdList.traceRecodring.indexToTraceID(count);
        auto vistorContext = cmdList.traceList.beginVisitation();

        NAU_LOG_DEBUG("DX12: Command Buffer execution incomplete");
        NAU_LOG_DEBUG("DX12: Command Buffer: {:p}", (int)cmd);
        NAU_LOG_DEBUG("DX12: Breadcrumb count: {}", count);
        NAU_LOG_DEBUG("DX12: Last breadcrumb value: {}", completedTraceID);

        // Report everything until the trace it as regular completed
        cmdList.traceList.reportEverythingUntilAsCompleted(vistorContext, completedTraceID, reporter);

        // Report everything that matches the trace as last completed
        NAU_LOG_DEBUG("DX12: ~Last known good command~~");
        cmdList.traceList.reportEverythingMatchingAsLastCompleted(vistorContext, completedTraceID, reporter);

        NAU_LOG_DEBUG("DX12: ~First may be bad command~");
        // Will print full dump for all remaining trace entries
        cmdList.traceList.reportEverythingAsNoCompleted(vistorContext, reporter);
      }
    }
  });
}

bool debug::gpu_postmortem::dagor::Trace::try_load(const Configuration &config, const Direct3D12Enviroment &d3d_env)
{
  if (!config.enableDagorGPUTrace)
  {
    NAU_LOG_DEBUG("DX12: Dagor GPU trace is disabled by configuration");
    return false;
  }

  NAU_LOG_DEBUG("DX12: Dagor GPU trace is enabled...");

  // when page fault should be tracked we try to use DRED, if it works nice, if not, no loss
  if (!config.trackPageFaults)
  {
    NAU_LOG_DEBUG("DX12: ...page fault tracking is disabled by configuration");
    return true;
  }

  NAU_LOG_DEBUG("DX12: Trying to capture page faults with DRED, may not work...");
  NAU_LOG_DEBUG("DX12: ...loading debug interface query...");
  PFN_D3D12_GET_DEBUG_INTERFACE D3D12GetDebugInterface = nullptr;
  reinterpret_cast<FARPROC &>(D3D12GetDebugInterface) = d3d_env.getD3DProcAddress("D3D12GetDebugInterface");
  if (!D3D12GetDebugInterface)
  {
    NAU_LOG_DEBUG("DX12: ...D3D12GetDebugInterface not found in direct dx runtime library");
    return true;
  }

  ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredConfig;
  if (FAILED(D3D12GetDebugInterface(COM_ARGS(&dredConfig))))
  {
    NAU_LOG_DEBUG("DX12: ...unable to acquire DRED config interface");
    return false;
  }

  dredConfig->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
  NAU_LOG_DEBUG("DX12: ...enabled page fault tracking of DRED");
  return true;
}

void debug::gpu_postmortem::dagor::Trace::configure() {}

void debug::gpu_postmortem::dagor::Trace::beginCommandBuffer(ID3D12Device3 *device, ID3D12GraphicsCommandList *cmd)
{
  auto &list = commandListTable.beginList(cmd, device);
  list.traceRecodring.beginRecording();
  list.traceList.beginTrace();
}

void debug::gpu_postmortem::dagor::Trace::endCommandBuffer(ID3D12GraphicsCommandList *cmd) { commandListTable.endList(cmd); }

void debug::gpu_postmortem::dagor::Trace::beginEvent(ID3D12GraphicsCommandList *cmd, eastl::span<const char> text,
  eastl::span<const char> full_path)
{
  auto &list = commandListTable.getList(cmd);
  list.traceList.beginEvent({}, text, full_path);
}

void debug::gpu_postmortem::dagor::Trace::endEvent(ID3D12GraphicsCommandList *cmd, eastl::span<const char> full_path)
{
  auto &list = commandListTable.getList(cmd);
  list.traceList.endEvent({}, full_path);
}

void debug::gpu_postmortem::dagor::Trace::marker(ID3D12GraphicsCommandList *cmd, eastl::span<const char> text)
{
  auto &list = commandListTable.getList(cmd);
  list.traceList.marker({}, text);
}

void debug::gpu_postmortem::dagor::Trace::draw(const call_stack::CommandData &debug_info, ID3D12GraphicsCommandList2 *cmd,
  const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline,
  uint32_t count, uint32_t instance_count, uint32_t start, uint32_t first_instance, D3D12_PRIMITIVE_TOPOLOGY topology)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.draw(id, debug_info, vs, ps, pipeline_base, pipeline, count, instance_count, start, first_instance, topology);
}

void debug::gpu_postmortem::dagor::Trace::drawIndexed(const call_stack::CommandData &debug_info, ID3D12GraphicsCommandList2 *cmd,
  const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline,
  uint32_t count, uint32_t instance_count, uint32_t index_start, int32_t vertex_base, uint32_t first_instance,
  D3D12_PRIMITIVE_TOPOLOGY topology)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.drawIndexed(id, debug_info, vs, ps, pipeline_base, pipeline, count, instance_count, index_start, vertex_base,
    first_instance, topology);
}

void debug::gpu_postmortem::dagor::Trace::drawIndirect(const call_stack::CommandData &debug_info, ID3D12GraphicsCommandList2 *cmd,
  const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline,
  BufferResourceReferenceAndOffset buffer)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.drawIndirect(id, debug_info, vs, ps, pipeline_base, pipeline, buffer);
}

void debug::gpu_postmortem::dagor::Trace::drawIndexedIndirect(const call_stack::CommandData &debug_info,
  ID3D12GraphicsCommandList2 *cmd, const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base,
  PipelineVariant &pipeline, BufferResourceReferenceAndOffset buffer)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.drawIndexedIndirect(id, debug_info, vs, ps, pipeline_base, pipeline, buffer);
}

void debug::gpu_postmortem::dagor::Trace::dispatchIndirect(const call_stack::CommandData &debug_info, ID3D12GraphicsCommandList2 *cmd,
  const PipelineStageStateBase &state, ComputePipeline &pipeline, BufferResourceReferenceAndOffset buffer)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.dispatchIndirect(id, debug_info, state, pipeline, buffer);
}

void debug::gpu_postmortem::dagor::Trace::dispatch(const call_stack::CommandData &debug_info, ID3D12GraphicsCommandList2 *cmd,
  const PipelineStageStateBase &state, ComputePipeline &pipeline, uint32_t x, uint32_t y, uint32_t z)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.dispatch(id, debug_info, state, pipeline, x, y, z);
}

void debug::gpu_postmortem::dagor::Trace::dispatchMesh(const call_stack::CommandData &debug_info, D3DGraphicsCommandList *cmd,
  const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline,
  uint32_t x, uint32_t y, uint32_t z)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.dispatchMesh(id, debug_info, vs, ps, pipeline_base, pipeline, x, y, z);
}

void debug::gpu_postmortem::dagor::Trace::dispatchMeshIndirect(const call_stack::CommandData &debug_info, D3DGraphicsCommandList *cmd,
  const PipelineStageStateBase &vs, const PipelineStageStateBase &ps, BasePipeline &pipeline_base, PipelineVariant &pipeline,
  BufferResourceReferenceAndOffset args, BufferResourceReferenceAndOffset count, uint32_t max_count)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.dispatchMeshIndirect(id, debug_info, vs, ps, pipeline_base, pipeline, args, count, max_count);
}

void debug::gpu_postmortem::dagor::Trace::blit(const call_stack::CommandData &call_stack, ID3D12GraphicsCommandList2 *cmd)
{
  auto &list = commandListTable.getList(cmd);
  auto id = list.traceRecodring.record(cmd);
  list.traceList.blit(id, call_stack);
}

void debug::gpu_postmortem::dagor::Trace::onDeviceRemoved(D3DDevice *device, HRESULT reason, call_stack::Reporter &reporter)
{
  if (DXGI_ERROR_INVALID_CALL == reason)
  {
    // Data is not useful when the runtime detected a invalid call.
    return;
  }

  // For possible page fault information we fall back to DRED
  NAU_LOG_DEBUG("DX12: Acquiring DRED interface...");
  ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(COM_ARGS(&dred))))
  {
    NAU_LOG_DEBUG("DX12: ...failed, no DRED information available");
    return;
  }

  walkBreadcumbs(reporter);
  report_page_fault(dred.Get());
}

bool debug::gpu_postmortem::dagor::Trace::sendGPUCrashDump(const char *, const void *, uintptr_t) { return false; }

void debug::gpu_postmortem::dagor::Trace::onDeviceShutdown() { commandListTable.reset(); }

bool debug::gpu_postmortem::dagor::Trace::onDeviceSetup(ID3D12Device *device, const Configuration &, const Direct3D12Enviroment &)
{
  // have to check if hw does support the necessary features
  D3D12_FEATURE_DATA_D3D12_OPTIONS3 level3Options = {};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS3, &level3Options, sizeof(level3Options))))
  {
    NAU_LOG_DEBUG("DX12: CheckFeatureSupport for OPTIONS3 failed, assuming no WriteBufferImmediate support");
    return false;
  }
  if (0 == (level3Options.WriteBufferImmediateSupportFlags & D3D12_COMMAND_LIST_SUPPORT_FLAG_DIRECT))
  {
    NAU_LOG_DEBUG("DX12: No support for WriteBufferImmediate on direct queue");
    return false;
  }

  // NOTE this is not really required as committed resource seems to work fine after crash, but is probably not entirely reliable
  // right now we require it though
  D3D12_FEATURE_DATA_EXISTING_HEAPS existingHeap = {};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_EXISTING_HEAPS, &existingHeap, sizeof(existingHeap))))
  {
    NAU_LOG_DEBUG("DX12: CheckFeatureSupport for EXISTING_HEAPS failed, assuming no existing heap support");
    return false;
  }
  if (FALSE == existingHeap.Supported)
  {
    NAU_LOG_DEBUG("DX12: No support for existing heaps");
    return false;
  }
  return true;
}
