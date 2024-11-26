// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once
#ifndef __DRV_DX12_DEVICE_H__
#define __DRV_DX12_DEVICE_H__

/////////////// TEMP ///////////////
#ifndef DAGOR_NOINLINE
#if defined(__GNUC__)
#define DAGOR_NOINLINE __attribute__((noinline))
#elif _MSC_VER >= 1300
#define DAGOR_NOINLINE __declspec(noinline)
#else
#define DAGOR_NOINLINE
#endif
#endif

////////////////////////////////////


#include "nau/generic/dag_objectPool.h"

#include "driver.h"
#include "platform.h"
#include "device_queue.h"
#include "shader.h"
#include "pipeline_cache.h"
#include "descriptor_heap.h"
#include "swapchain.h"
#include "pipeline.h"
#include "bindless.h"
#include "device_context.h"
#include "query_manager.h"
#include "tagged_handles.h"
#include "pipeline/blk_cache.h"

//#include <debug/dag_debug.h>
#include <atomic>
#include <mutex>
#include "nau/3d/dag_drv3d.h"
#include "nau/diag/logging.h"


#include "nau/supp/dag_comPtr.h"
#include "nau/osApiWrappers/dag_miscApi.h"

#include "nau/util/common.h"

template <>
struct fmt::formatter<D3D12_RESOURCE_STATES> : fmt::formatter<const char*>
{
    auto format(const D3D12_RESOURCE_STATES state, format_context& ctx) const
    {
        switch(state)
        {
            case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_COMMON:
                return formatter<const char*>::format("D3D12_RESOURCE_STATE_COMMON", ctx);
                break;
            case D3D12_RESOURCE_STATES::D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER:
                return formatter<const char*>::format("D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER", ctx);
                break;
            default:
                return formatter<const char*>::format("TODO: add more cases for D3D12_RESOURCE_STATES", ctx);
                break;
        };
        return formatter<const char*>::format("unknown log level", ctx);
    }
};

template <>
struct fmt::formatter<D3D12_VIEW_INSTANCING_TIER> : fmt::formatter<const char*>
{
    auto format(const D3D12_VIEW_INSTANCING_TIER state, format_context& ctx) const
    {
        switch(state)
        {
            case D3D12_VIEW_INSTANCING_TIER::D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED:
                return formatter<const char*>::format("D3D12_VIEW_INSTANCING_TIER_NOT_SUPPORTED", ctx);
                break;
            case D3D12_VIEW_INSTANCING_TIER::D3D12_VIEW_INSTANCING_TIER_1:
                return formatter<const char*>::format("D3D12_VIEW_INSTANCING_TIER_1", ctx);
                break;
            default:
                return formatter<const char*>::format("TODO: add more cases for D3D12_VIEW_INSTANCING_TIER", ctx);
                break;
        };
        return formatter<const char*>::format("unknown log level", ctx);
    }
};

template <>
struct fmt::formatter<DXGI_FORMAT> : fmt::formatter<const char*>
{
    auto format(const DXGI_FORMAT state, format_context& ctx) const
    {
        switch(state)
        {
            case DXGI_FORMAT::DXGI_FORMAT_UNKNOWN:
                return formatter<const char*>::format("DXGI_FORMAT_UNKNOWN", ctx);
                break;
            default:
                return formatter<const char*>::format("TODO: add more cases for DXGI_FORMAT", ctx);
                break;
        };
        return formatter<const char*>::format("unknown log level", ctx);
    }
};



namespace drv3d_dx12
{
class Device;

struct TextureSubresourceInfo
{
  D3D12_SUBRESOURCE_FOOTPRINT footprint{};
  uint32_t rowCount{};
  // basically footprint.RowPitch not aligned to D3D12_TEXTURE_DATA_PITCH_ALIGNMENT
  uint32_t rowByteSize{};
  // RowPitch * rowCount * Depth * Arrays aligned to D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
  uint32_t totalByteSize{};
};

TextureSubresourceInfo calculate_texture_region_info(Extent3D ext, ArrayLayerCount arrays, FormatStore fmt);
inline TextureSubresourceInfo calculate_texture_mip_info(const Image &texture, MipMapIndex mip_level)
{
  return calculate_texture_region_info(texture.getMipExtents(mip_level), texture.getArrayLayers(), texture.getFormat());
}
inline TextureSubresourceInfo calculate_texture_subresource_info(const Image &texture, SubresourceIndex subres_index)
{
  return calculate_texture_mip_info(texture, texture.stateIndexToMipIndex(subres_index));
}
uint64_t calculate_texture_staging_buffer_size(Extent3D size, MipMapCount mips, FormatStore format,
  SubresourceRange subresource_range);
inline uint64_t calculate_texture_staging_buffer_size(const Image &texture, SubresourceRange subresource_range)
{
  return calculate_texture_staging_buffer_size(texture.getBaseExtent(), texture.getMipLevelRange(), texture.getFormat(),
    subresource_range);
}

#if D3D_HAS_RAY_TRACING

D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS toAccelerationStructureBuildFlags(RaytraceBuildFlags flags);
eastl::pair<D3D12_RAYTRACING_GEOMETRY_DESC, RaytraceGeometryDescriptionBufferResourceReferenceSet>
raytraceGeometryDescriptionToGeometryDesc(const RaytraceGeometryDescription &desc);
#endif

enum class BufferViewType
{
  SRV,
  UAV
};

enum class BufferViewFormating
{
  FORMATED,
  STRUCTURED,
  RAW
};

struct VirtualAllocFree
{
  void operator()(void *ptr) { VirtualFree(ptr, 0, MEM_RELEASE); }
};

#if _TARGET_PC_WIN
// min requirement is device3 right now, 5 is optional to raytracing
typedef VersionedComPtr<D3DDevice, ID3D12Device5> AnyDeviceComPtr;
#else
typedef VersionedComPtr<D3DDevice> AnyDeviceComPtr;
#endif

#if _TARGET_PC_WIN
class DeviceErrroState
{
  enum class Health
  {
    HEALTHY,
    ILL,
    RECOVERING,
    DEAD
  };

  // will be set when a device removal event occurs
  std::atomic<Health> contextHealth{Health::HEALTHY};

protected:
  void enterErrorState()
  {
    Health expected = Health::HEALTHY;
    if (contextHealth.compare_exchange_strong(expected, Health::ILL))
    {
      NAU_LOG_DEBUG("DX12: Device was in healthy state, entering ill state...");
    }
    else if (Health::ILL == expected)
    {
      // nothing to do, this case if we set ILL state manually and then the error reporter
      // sets it again
      NAU_LOG_DEBUG("DX12: Error state reported while already in ill state...");
    }
    else if (Health::RECOVERING == expected)
    {
      NAU_LOG_ERROR("DX12: Device was in recovering state, entering dead state...");
      contextHealth.compare_exchange_strong(expected, Health::DEAD);
    }
    else
    {
      NAU_LOG_ERROR("DX12: Device was in unexpected state {}", static_cast<uint32_t>(expected));
    }
    notify_all(contextHealth);
  }

  void enterRecoveringState()
  {
    contextHealth.store(Health::RECOVERING);
    notify_all(contextHealth);
  }

  bool enterHealthyState()
  {
    Health expected = Health::RECOVERING;
    if (!contextHealth.compare_exchange_strong(expected, Health::HEALTHY))
    {
      NAU_LOG_DEBUG("DX12: Failed to recover device after critical error");
      return false;
    }
    else
    {
      notify_all(contextHealth);
      NAU_LOG_DEBUG("DX12: Device was successfully recovered after critical error");
      return true;
    }
  }

public:
  bool isHealthyOrRecovering() const
  {
    auto v = contextHealth.load(std::memory_order_relaxed);
    return (Health::HEALTHY == v) || (Health::RECOVERING == v);
  }
  bool isHealthy() const { return Health::HEALTHY == contextHealth.load(std::memory_order_relaxed); }
  bool isIll() const { return Health::ILL == contextHealth.load(std::memory_order_relaxed); }
  bool isRecovering() const { return Health::RECOVERING == contextHealth.load(std::memory_order_relaxed); }
  bool isDead() const { return Health::DEAD == contextHealth.load(std::memory_order_relaxed); }

  bool isInErrorState() const
  {
    auto v = contextHealth.load(std::memory_order_relaxed);
    return (Health::ILL == v) || (Health::DEAD == v);
  }

  // Simple method that will do the right thing when device is in a state that
  // prevents creation of resources. From the time a error was detected and
  // until the reset process is started, this will block. Should be a fatal error
  // be detected this will return false. In any other case it will return true.
  bool checkResourceCreateState(const char8_t *what = u8"checkResourceCreateState", const char8_t *name = u8"<unknown>")
  {
    for (;;)
    {
      auto v = contextHealth.load(std::memory_order_seq_cst);
      // On healthy and recovering state we can create resources without issues
      if ((Health::HEALTHY == v) || (Health::RECOVERING == v))
      {
        return true;
      }
      // On error while recovering we stop creation of resources as we are
      // about to wind down and terminate.
      if (Health::DEAD == v)
      {
        NAU_LOG_ERROR("DX12: Trying to {} for <{}> while device encountered an error during reset process", what, name);
        return false;
      }

      NAU_LOG_WARNING("DX12: Trying to {} for <{}> after a error was detected and recovery was not started yet", what, name);
      // eg v == Health::ILL
      // On ill state we have to wait until the recover process has started
      // until then we have to wait or results are undefined.
      wait(contextHealth, v);
    }
  }

  // Returns true if resources can safely deleted without checking if they are still valid,
  // on false resources have to be checked if they are still valid and only be deleted if they are.
  bool checkResourceDeleteState(const char *what = "checkResizrceDeleteState", const char8_t *name = u8"<unknown>")
  {
    bool hasToCheck = false;
    for (;;)
    {
      auto v = contextHealth.load(std::memory_order_seq_cst);
      // On healthy state we can safely delete resources
      if (Health::HEALTHY == v)
      {
        return !hasToCheck;
      }

      // If either ill or in dead state, we have to check if a resource is still valid.
      if (Health::RECOVERING != v)
      {
        NAU_LOG_WARNING("DX12: Trying to {} for <{}> after a error was detected and recovery was not "
                "started yet (or recovery failed)",
          what, name);
        return false;
      }

      NAU_LOG_WARNING("DX12: Trying to {} for <{}> after a error was detected and recovery was not "
              "completed yet",
        what, name);
      wait(contextHealth, v);
      hasToCheck = true;
    }
  }
};

template <typename D>
class DeviceErrorObserver
{
  struct ObserverThread : public DaThread
  {
    ObserverThread(D *target, ComPtr<ID3D12Fence> detector) :
      DaThread{"DX12 Error Watch"}, detector{eastl::move(detector)}, target{target}
    {}
    void execute() override
    {
      // Change priority as the priority param of DaThread gets clamped, but this thread needs to be
      // on this priority to better capture reset info before any other thread can do anything else.
      SetThreadPriority(GetCurrentThread(), 15);
      NAU_LOG_DEBUG("DX12: Device error observer started...");
      // Could wait without event here, but some tools will deadlock, like PIX.
      // Those block other threads until each API call has returned, as calling
      // SetEventOnCompletion with null as event will wait until the fence is
      // signaled, it blocks all other API calls of all other threads and so
      // dead locks.
      EventPointer trigger{CreateEvent(nullptr, FALSE, FALSE, nullptr)};
      // On error all fences will be signaled with ~0, or when we exit we set this too
      NAU_LOG_DEBUG("DX12: Device error observer trigger set, waiting...");
      if (DX12_CHECK_OK(detector->SetEventOnCompletion(~uint64_t(0), trigger.get())))
      {
        WaitForSingleObject(trigger.get(), INFINITE);
        NAU_LOG_DEBUG("DX12: Device error observer trigger was signaled...");
      }

      detector.Reset();
      target->signalDeviceError();
      NAU_LOG_DEBUG("DX12: Device error observer finished execution, exiting...");
    }
    ComPtr<ID3D12Fence> detector;
    D *target;
  };

  eastl::unique_ptr<ObserverThread> observer;
  ComPtr<ID3D12Fence> detector;

protected:
  void startDeviceErrorObserver(D3DDevice *device)
  {
    if (DX12_CHECK_FAIL(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, COM_ARGS(&detector))))
    {
      NAU_LOG_ERROR("DX12: Unable to setup device error observer, failed to create detector fence");
      return;
    }

    observer = eastl::make_unique<ObserverThread>(static_cast<D *>(this), detector);
    if (!observer->start())
    {
      NAU_LOG_ERROR("DX12: Unable to setup device error observer, failed to start observer thread");
      detector.Reset();
    }
  }
  using ErrorObserverShutdownToken = ComPtr<ID3D12Fence>;
  ErrorObserverShutdownToken enterDeviceErrorObserverInShutdownMode()
  {
    auto result = eastl::move(detector);
    return result;
  }
  void stopDeviceErrorObserver(ErrorObserverShutdownToken &&token)
  {
    if (token)
    {
      DX12_CHECK_RESULT(token->Signal(~uint64_t(0)));
    }
    if (observer)
    {
      observer->terminate(true);
      observer.reset();
    }
  }

  // if our detector pointer is null we are shutting down
  bool observerIsShuttingDown() { return !static_cast<bool>(detector); }
};
#else
class DeviceErrroState
{
protected:
  void enterErrorState() {}

  void enterRecoveringState() {}

  bool enterHealthyState() { return true; }

public:
  constexpr bool isHealthyOrRecovering() const { return true; }
  constexpr bool isHealthy() const { return true; }
  constexpr bool isIll() const { return false; }
  constexpr bool isRecovering() const { return false; }
  constexpr bool isDead() const { return false; }
  constexpr bool isInErrorState() const { return false; }
  constexpr bool checkResourceCreateState(const char *, const char *) const { return true; }
  constexpr bool checkResourceDeleteState(const char *, const char *) const { return true; }
};
template <typename D>
class DeviceErrorObserver
{
  // for consoles we do nothing
};
#endif

// This is from the D3D12 Agility SDK. We need to figure out how to
// make use of its headers and libraries. For now, the needed stuff
// is declared here.
#if _TARGET_PC_WIN
#define D3D12_FEATURE_D3D12_OPTIONS8 ((D3D12_FEATURE)36)

typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS8
{
  _Out_ BOOL UnalignedBlockTexturesSupported;
} D3D12_FEATURE_DATA_D3D12_OPTIONS8;

#define D3D12_FEATURE_D3D12_OPTIONS9 ((D3D12_FEATURE)37)

typedef struct D3D12_FEATURE_DATA_D3D12_OPTIONS9
{
  _Out_ BOOL MeshShaderPipelineStatsSupported;
  _Out_ BOOL MeshShaderSupportsFullRangeRenderTargetArrayIndex;
  _Out_ BOOL AtomicInt64OnTypedResourceSupported;
  _Out_ BOOL AtomicInt64OnGroupSharedSupported;
  _Out_ BOOL DerivativesInMeshAndAmplificationShadersSupported;
  _Out_ INT WaveMMATier;
} D3D12_FEATURE_DATA_D3D12_OPTIONS9;
#endif

template <typename T>
struct FeatureID;

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS1>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS1;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS2>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS2;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS3>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS3;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS4>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS4;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS5>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS5;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS6>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS6;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS7>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS7;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS8>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS8;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_D3D12_OPTIONS9>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_D3D12_OPTIONS9;
};

template <>
struct FeatureID<D3D12_FEATURE_DATA_SHADER_MODEL>
{
  static constexpr D3D12_FEATURE value = D3D12_FEATURE_SHADER_MODEL;
};

class Device : public DeviceErrroState, public DeviceErrorObserver<Device>, protected debug::DeviceState
{
  friend class DeviceContext;
  friend class backend::Swapchain;
  friend class backend::AdditionalSwapchain;
  friend class frontend::Swapchain;
  friend class TempBufferManager;
  friend class ResourceMemoryHeap;

public:
  struct Config
  {
    DeviceFeaturesConfig::Type features;
    ResourceMemoryHeap::SetupInfo memorySetup;
  };

#if _TARGET_PC_WIN
  struct AdapterInfo
  {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 info;
    bool integrated;
  };
#endif

private:
  struct Caps
  {
    enum
    {
      DEPTH_BOUNDS_TEST,
#if D3D_HAS_RAY_TRACING
      RAY_TRACING,
      RAY_TRACING_T1_1,
#endif
#if !_TARGET_XBOXONE
      SHADING_RATE_T1,
      SHADING_RATE_T2,
#endif

      MAX
    };
    typedef eastl::bitset<MAX> Type;
  };
#if _TARGET_PC_WIN
  ComPtr<DXGIAdapter> adapter;
#endif
  AnyDeviceComPtr device;
  DeviceQueueGroup queues;
  Caps::Type caps;
  Config config; //-V730_NOINIT
  eastl::vector<DeviceResetEventHandler *> deviceResetEventHandlers;
  RenderStateSystem renderStateSystem;
  PipelineManager pipeMan;
  PipelineCache pipelineCache;
  FrontendQueryManager frontendQueryManager;
  uint32_t lastAllocationCount = 0;
  uint32_t lastFreeCount = 0;
  DeviceContext context;
  ResourceMemoryHeap resources;
  NullResourceTable nullResourceTable = {};
  D3D12_CPU_DESCRIPTOR_HANDLE defaultSampler{};
  D3D12_CPU_DESCRIPTOR_HANDLE defaultCmpSampler{};
#if _TARGET_PC_WIN
  DriverVersion driverVersion{};
#endif
  frontend::BindlessManager bindlessManager;
  BufferState nullBuffer;

  D3D12_CPU_DESCRIPTOR_HANDLE getNonRecentImageViews(Image *img, ImageViewState state); // checks oldViews, update LRU entry if found
  // context lock has to be held to ensure data consistency
  D3D12_CPU_DESCRIPTOR_HANDLE getImageView(Image *img, ImageViewState state)
  {
    if (DAGOR_LIKELY(img->getRecentView().state == state))
      return img->getRecentView().handle;
    return getNonRecentImageViews(img, state);
  }

  Device(const Device &);
  Device &operator=(const Device &);
  // TODO rename and move null resource table gen into it
  void setupNullViews();
  void configureFeatureCaps();

  // creates new image, also allocates id range to it, however it does no id range
  // register at the resource tracker in any way.
  Image *createImageNoContextLock(const ImageInfo &ii, const char8_t *name);

#if _TARGET_PC_WIN
  DXGIAdapter *getDXGIAdapter() { return adapter.Get(); }
#else
  // on console we don't need this, but to make code a bit cleaner we have this return nullptr
  DXGIAdapter *getDXGIAdapter() { return nullptr; }
#endif
  // Extends DeviceErrroState::checkResourceCreateState to delay the completion until a device is not null
  // otherwise it could exit when error recovery was started but no device was created yet.
  bool checkResourceCreateState(const char8_t *what = u8"checkResourceCreateState", const char8_t *name = u8"<unknown>")
  {
    if (!DeviceErrroState::checkResourceCreateState(what, name))
    {
      return false;
    }
    while (!device)
    {
      // Ensure we don't infinite loop on error during recovering.
      if (isDead())
      {
        return false;
      }
      nau::hal::sleep_msec(1);
    }
    return true;
  }

  // Pulls the D3D12_FEATURE value from FeatureID<T>::value
  // This makes it very simple to query feature, just auto name = checkFeatureSupport<type>(<optional init>);
  template <typename T, typename... As>
  T checkFeatureSupport(As &&...as)
  {
    T result{eastl::forward<As>(as)...};
    if (S_OK != device->CheckFeatureSupport(FeatureID<T>::value, &result, sizeof(result)))
    {
      result = {};
    }
    return result;
  }

public:
  using debug::DeviceState::processDebugLog;



  SWAPID createAdditionalSwachain(void* hwnd, DXGIFactory *factory);
  void removeAdditionalSwapchain(SWAPID swapID);


  Device() : context(*this) {}
  ~Device();
#if _TARGET_PC_WIN
  bool init(DXGIFactory *factory, AdapterInfo &&adapterInfo, D3D_FEATURE_LEVEL feature_level, const Direct3D12Enviroment &d3d_env,
    SwapchainCreateInfo swapchain_create_info, debug::GlobalState &debug_state, const Config &cfg, const nau::DataBlock *dxCfg,
    bool stereo_render);
#elif _TARGET_XBOX
  bool init(SwapchainCreateInfo swapchain_create_info, const Config &cfg);
#endif
  bool isInitialized() const;
  void shutdown(const DeviceCapsAndShaderModel &deatures);
  void adjustCaps(Driver3dDesc &);
#if D3D_HAS_RAY_TRACING
  bool hasRaytraceSupport() const { return caps.test(Caps::RAY_TRACING); }
#endif
  uint64_t getGpuTimestampFrequency();
  int getGpuClockCalibration(uint64_t *gpu, uint64_t *cpu, int *cpu_freq_type);
  Image *createImage(const ImageInfo &ii, Image *base_image, const char8_t *name);
#if DX12_USE_ESRAM
  Image *createEsramBackedImage(const ImageInfo &ii, Image *base_image, const char *name);
#endif
  Texture *wrapD3DTex(ID3D12Resource *tex_res, ResourceBarrier current_state, const char8_t *name, int flg);
  BufferState createBuffer(uint32_t size, uint32_t structure_size, uint32_t discard_count, DeviceMemoryClass memory_class,
    D3D12_RESOURCE_FLAGS flags, uint32_t cflags, const char8_t *name);
  BufferState createDedicatedBuffer(uint32_t size, uint32_t structure_size, uint32_t discard_count, DeviceMemoryClass memory_class,
    D3D12_RESOURCE_FLAGS flags, uint32_t cflags, const char8_t *name);
  void addBufferView(BufferState &buffer, BufferViewType view_type, BufferViewFormating formating, FormatStore format,
    uint32_t struct_size);
  d3d::SamplerHandle createSampler(SamplerState state) { return resources.createSampler(device.get(), state); }
  void deleteSampler(d3d::SamplerHandle handle) { resources.deleteSampler(handle); }
  D3D12_CPU_DESCRIPTOR_HANDLE getSampler(d3d::SamplerHandle handle) { return resources.getSampler(handle); }
  D3D12_CPU_DESCRIPTOR_HANDLE getSampler(SamplerState state) { return resources.getSampler(device.get(), state); }
  int createPredicate();
  void deletePredicate(int name);
  void setTexName(Image *img, const char8_t *name);
  bool hasDepthBoundsTest() const { return caps.test(Caps::DEPTH_BOUNDS_TEST); }

  ID3D12CommandQueue *getGraphicsCommandQueue() const;
  D3DDevice *getDevice();

  DeviceContext &getContext() { return context; }
  FrontendQueryManager &getQueryManager() { return frontendQueryManager; }
  bool getGpuMemUsageStats(uint64_t additionalVramUsageInBytes, uint32_t *out_mem_size, uint32_t *out_free_mem_kb,
    uint32_t *out_used_mem_kb);
#if D3D_HAS_RAY_TRACING
  RaytraceAccelerationStructure *createRaytraceAccelerationStructure(RaytraceGeometryDescription *desc, uint32_t count,
    RaytraceBuildFlags flags);
  RaytraceAccelerationStructure *createRaytraceAccelerationStructure(uint32_t elements, RaytraceBuildFlags flags);

  BufferResourceReferenceAndAddress getRaytraceScratchBuffer() { return resources.getRaytraceScratchBuffer(); }
#endif
  void flushMappedMemory(BufferState &buffer, uint32_t offset, uint32_t size)
  {
    if (hasCoherentMemory(buffer))
      return;
    buffer.flushMappedMemory(offset, size);
  }
  void invalidateMappedMemory(BufferState &buffer, uint32_t offset, uint32_t size)
  {
    if (hasCoherentMemory(buffer))
      return;
    buffer.invalidateMappedMemory(offset, size);
  }
  bool hasCoherentMemory(BufferState &buffer)
  {
    // right now we use default DX12 stuff and assume nothing is coherent
    G_UNUSED(buffer);
    return false;
  }

#if _TARGET_PC_WIN
  AdapterInfo getAdapterInfo();
  uint64_t getAdapterLuid();
  void enumerateDisplayModes(eastl::vector<nau::string> &list);
  void enumerateDisplayModesFromOutput(IDXGIOutput *dxgi_output, eastl::vector<nau::string> &list);
  void enumerateActiveMonitors(eastl::vector<nau::string> &result);
  ComPtr<IDXGIOutput> getOutputMonitorByNameOrDefault(const char *monitorName);
  HRESULT findClosestMatchingMode(DXGI_MODE_DESC *out_desc);
#endif

  bool isSamplesCountSupported(DXGI_FORMAT format, int32_t samples_count);

  D3D12_FEATURE_DATA_FORMAT_SUPPORT getFormatFeatures(FormatStore fmt);
  ImageGlobalSubresouceId getSwapchainColorGlobalId() const { return resources.getSwapchainColorGlobalId(); }
  ImageGlobalSubresouceId getSwapchainSecondaryColorGlobalId() const { return resources.getSwapchainSecondaryColorGlobalId(); }

#if _TARGET_PC_WIN
  void signalDeviceError()
  {
    STORE_RETURN_ADDRESS();

    if (observerIsShuttingDown())
    {
      NAU_LOG_DEBUG("DX12: Device error observer shutdown");
      return;
    }
    // report everything into error log, so we have complete overview by just looking at that log
    NAU_LOG_ERROR("DX12: Detected device lost...");
    auto removedReason = device->GetDeviceRemovedReason();
    NAU_LOG_ERROR("DX12: GetDeviceRemovedReason returned {}", dxgi_error_code_to_string(removedReason));

    NAU_LOG_ERROR("DX12: Trying to read debug layer message queue...");
    NAU_ASSERT(false);
    // report the debug log, if enabled it may tells what happened
    processDebugLog();

    context.onDeviceError(removedReason);

    enterErrorState();
  }
#endif

  // almost as normal shutdown, but keeps some things
#if _TARGET_PC_WIN
  LUID preRecovery();
  bool recover(DXGIFactory *factory, ComPtr<IDXGIAdapter1> input_adapter, D3D_FEATURE_LEVEL feature_level,
    const Direct3D12Enviroment &d3d_env, HWND wnd, SwapchainCreateInfo &&swapchain_create_info);
  bool finalizeRecovery();

  using debug::DeviceState::sendGPUCrashDump;
#endif

  void registerDeviceResetEventHandler(DeviceResetEventHandler *handler);
  void unregisterDeviceResetEventHandler(DeviceResetEventHandler *handler);

  RenderStateSystem &getRenderStateSystem() { return renderStateSystem; }

#if _TARGET_XBOXONE
  constexpr unsigned getVariableShadingRateTier() const { return 0; }
#else
  unsigned getVariableShadingRateTier() const
  {
    // The T1's bit is the 0b01, the T2's bit is the 0b10.
    // T2 can never be defined without T1, so this always yields 1 if only T1 is define and 3 with both are.
    return caps.test(Caps::SHADING_RATE_T2) ? 3 : (caps.test(Caps::SHADING_RATE_T1) ? 1 : 0);
  }
#endif

#if DX12_DOES_SET_DEBUG_NAMES
  bool shouldNameObjects() const { return config.features.test(DeviceFeaturesConfig::NAME_OBJECTS); }
#else
  bool shouldNameObjects() const { return false; }
#endif

  uint32_t registerBindlessSampler(BaseTex *texture) { return bindlessManager.registerSampler(*this, context, texture); }

  uint32_t allocateBindlessResourceRange(uint32_t count) { return bindlessManager.allocateBindlessResourceRange(count); }

  uint32_t resizeBindlessResourceRange(uint32_t index, uint32_t current_count, uint32_t new_count)
  {
    return bindlessManager.resizeBindlessResourceRange(context, index, current_count, new_count);
  }

  void freeBindlessResourceRange(uint32_t index, uint32_t count) { bindlessManager.freeBindlessResourceRange(index, count); }

  void updateBindlessBuffer(uint32_t index, GenericBufferInterface *buffer)
  {
    bindlessManager.updateBindlessBuffer(context, index, buffer);
  }

  void updateBindlessTexture(uint32_t index, BaseTex *res) { bindlessManager.updateBindlessTexture(context, index, res); }

  void updateBindlessNull(uint32_t resource_type, uint32_t index, uint32_t count)
  {
    bindlessManager.updateBindlessNull(context, resource_type, index, count, nullResourceTable);
  }

#if _TARGET_PC_WIN
  DriverVersion getDriverVersion() const { return driverVersion; }
#endif

  void memoryDebugOverlay()
  {
#if DAGOR_DBGLEVEL > 0
    resources.debugOverlay();
#endif
  }

#if DX12_USE_ESRAM
  void selectESRAMLayout(const wchar_t *wcs) { resources.selectLayout(wcs); }
  void deselectESRAMLayout() { resources.deselectLayout(); }
  void resetESRAMLayouts()
  {
    context.finish();
    resources.resetAllLayouts();
  }
  void fetchTexturesToESRAM() { resources.fetchMovableTextures(context); }
  void writeBackTexturesFromESRAM() { resources.writeBackMovableTextures(context); }
  void registerMovableESRAMTexture(Image *texture) { resources.registerMovableTexture(texture); }
#endif

#if DX12_CONFIGUREABLE_BARRIER_MODE
  bool generatesBarriers() const { return config.features.test(DeviceFeaturesConfig::GENERATE_ALL_BARRIERS); }

  bool processesUserBarriers() const { return config.features.test(DeviceFeaturesConfig::PROCESS_USER_BARRIERS); }

  bool validatesUserBarriers() const { return config.features.test(DeviceFeaturesConfig::VALIDATE_USER_BARRIERS); }
#else
#if DX12_PROCESS_USER_BARRIERS
  constexpr bool generatesBarriers() const { return false; }

  constexpr bool processesUserBarriers() const { return true; }

  constexpr bool validatesUserBarriers() const { return false; }
#else
  constexpr bool generatesBarriers() const { return true; }

  constexpr bool processesUserBarriers() const { return false; }

  constexpr bool validatesUserBarriers() const { return false; }
#endif
#endif

  ResourceHeap *newUserHeap(ResourceHeapGroup *group, size_t size, ResourceHeapCreateFlags flags)
  {
    return resources.newUserHeap(getDXGIAdapter(), device.get(), group, size, flags);
  }

  ResourceAllocationProperties getResourceAllocationProperties(const ResourceDescription &desc)
  {
    return resources.getResourceAllocationProperties(device.get(), desc);
  }

  BufferState placeBufferInHeap(::ResourceHeap *heap, const ResourceDescription &desc, size_t offset,
    const ResourceAllocationProperties &alloc_info, const char8_t *name);
  Image *placeTextureInHeap(::ResourceHeap *heap, const ResourceDescription &desc, size_t offset,
    const ResourceAllocationProperties &alloc_info, const char8_t *name);
  ResourceHeapGroupProperties getResourceHeapGroupProperties(::ResourceHeapGroup *heap_group)
  {
    return resources.getResourceHeapGroupProperties(heap_group);
  }

  size_t getFramePushRingMemorySize() { return resources.getFramePushRingMemorySize(); }
  size_t getUploadRingMemorySize() { return resources.getUploadRingMemorySize(); }
  size_t getTemporaryUploadMemorySize() { return resources.getTemporaryUploadMemorySize(); }
  size_t getPersistentUploadMemorySize() { return resources.getPersistentUploadMemorySize(); }
  size_t getPersistentReadBackMemorySize() { return resources.getPersistentReadBackMemorySize(); }
  size_t getPersistentBidirectionalMemorySize() { return resources.getPersistentBidirectionalMemorySize(); }

  template <typename T>
  void visitHeaps(T clb)
  {
    resources.visitHeaps(eastl::forward<T>(clb));
  }

  template <typename T>
  void visitBuffers(T clb)
  {
    resources.visitBuffers(eastl::forward<T>(clb));
  }

  template <typename T>
  void visitImageObjects(T clb)
  {
    resources.visitImageObjects(eastl::forward<T>(clb));
  }

#if DAGOR_DBGLEVEL > 0
  void resourceUseOverlay() { static_cast<ResourceUsageHistoryDataSetDebugger &>(context).debugOverlay(); }
#endif

  bool allowStreamBuffers() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_BUFFERS); }

  bool allowConstStreamBuffers() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_CONST_BUFFERS); }

  bool allowVertexStreamBuffers() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_VERTEX_BUFFERS); }

  bool allowIndexStreamBuffers() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_INDEX_BUFFERS); }

  bool allowIndirectStreamBuffer() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_INDIRECT_BUFFERS); }

  bool allowStagingStreamBuffer() const { return config.features.test(DeviceFeaturesConfig::ALLOW_STREAM_STAGING_BUFFERS); }

  template <typename... Args>
  TextureInterfaceBase *newTextureObject(Args &&...args)
  {
    return resources.newTextureObject(eastl::forward<Args>(args)...);
  }

  template <typename T>
  void visitTextureObjects(T &&clb)
  {
    resources.visitTextureObjects(eastl::forward<T>(clb));
  }

  void reserveTextureObjects(size_t count) { resources.reserveTextureObjects(count); }

  size_t getTextureObjectCapacity() { return resources.getTextureObjectCapacity(); }

  size_t getActiveTextureObjectCount() { return resources.getActiveTextureObjectCount(); }

  template <typename... Args>
  GenericBufferInterface *newBufferObject(Args &&...args)
  {
    return resources.newBufferObject(eastl::forward<Args>(args)...);
  }

  template <typename T>
  void visitBufferObjects(T &&clb)
  {
    resources.visitBufferObjects(eastl::forward<T>(clb));
  }

  void deleteBufferObject(GenericBufferInterface *buffer) { resources.deleteBufferObject(buffer); }

  void reserveBufferObjects(size_t size) { resources.reserveBufferObjects(size); }

  size_t getBufferObjectCapacity() { return resources.getBufferObjectCapacity(); }

  size_t getActiveBufferObjectCount() { return resources.getActiveBufferObjectCount(); }

#if DX12_ENABLE_CONST_BUFFER_DESCRIPTORS
  bool rootSignaturesUsesCBVDescriptorRanges() const
  {
    return config.features.test(DeviceFeaturesConfig::ROOT_SIGNATURES_USES_CBV_DESCRIPTOR_RANGES);
  }
#endif

  ResourceMemory getResourceMemoryForBuffer(BufferResourceReference ref)
  {
    return resources.getResourceMemoryForBuffer(ref.resourceId);
  }

  bool ignorePredication() const { return config.features.test(DeviceFeaturesConfig::IGNORE_PREDICATION); }

  // This buffer can also be used to initialize other GPU resources with 0, as long as they are not bigger than this buffer.
  BufferResourceReferenceAndAddressRange getNullBuffer() const { return nullBuffer; }

  D3D12_CONSTANT_BUFFER_VIEW_DESC getNullConstBufferView() const
  {
    D3D12_CONSTANT_BUFFER_VIEW_DESC view;
    auto buf = getNullBuffer();
    view.BufferLocation = buf.gpuPointer;
    view.SizeInBytes = buf.size;
    return view;
  }

  TextureTilingInfo getTextureTilingInfo(BaseTex *tex, size_t subresource);

  HostDeviceSharedMemoryRegion allocatePersistentUploadMemory(size_t size, size_t alignment)
  {
    if (!checkResourceCreateState(u8"allocatePersistentUploadMemory"))
    {
      return {};
    }
    return resources.allocatePersistentUploadMemory(getDXGIAdapter(), device.get(), size, alignment);
  }
  HostDeviceSharedMemoryRegion allocatePersistentReadBackMemory(size_t size, size_t alignment)
  {
    if (!checkResourceCreateState(u8"allocatePersistentReadBackMemory"))
    {
      return {};
    }
    return resources.allocatePersistentReadBack(getDXGIAdapter(), device.get(), size, alignment);
  }
  HostDeviceSharedMemoryRegion allocatePersistentBidirectionalMemory(size_t size, size_t alignment)
  {
    if (!checkResourceCreateState(u8"allocatePersistentBidirectionalMemory"))
    {
      return {};
    }
    return resources.allocatePersistentBidirectional(getDXGIAdapter(), device.get(), size, alignment);
  }
  HostDeviceSharedMemoryRegion allocateTemporaryUploadMemory(size_t size, size_t alignment)
  {
    if (!checkResourceCreateState(u8"allocateTemporaryUploadMemory"))
    {
      return {};
    }
    bool shouldFlush = false;
    auto result = resources.allocateTempUpload(getDXGIAdapter(), device.get(), size, alignment, shouldFlush);
    if (shouldFlush)
    {
      context.flushDraws();
    }
    return result;
  }
  HostDeviceSharedMemoryRegion allocateTemporaryUploadMemoryForUploadBuffer(size_t size, size_t alignment)
  {
    if (!checkResourceCreateState(u8"allocateTemporaryUploadMemoryForUploadBuffer"))
    {
      return {};
    }
    return resources.allocateTempUploadForUploadBuffer(getDXGIAdapter(), device.get(), size, alignment);
  }

  void generateResourceAndMemoryReport(uint32_t *num_textures, uint64_t *total_mem, nau::string *out_text)
  {
    resources.generateResourceAndMemoryReport(num_textures, total_mem, out_text);
  }

  void reportOOMInformation() { resources.reportOOMInformation(); }
  bool isImageAlive(Image *image) { return resources.isImageAlive(image); }
};

// makes all uses as dirty so that a discarded buffer is correctly used
void notify_discard(Sbuffer *buffer, bool check_vb, bool check_const, bool check_tex, bool check_storage);
void notify_delete(Sbuffer *buffer);

template <typename T>
inline void PipelineStageStateBase::enumerateUAVResources(uint32_t uav_mask, T clb)
{
  for (auto i : nau::math::LsbVisitor{uav_mask})
  {
    if (uRegisters[i].image)
    {
      clb(uRegisters[i].image->getHandle());
    }
    else if (uRegisters[i].buffer)
    {
      clb(uRegisters[i].buffer.buffer);
    }
  }
}

inline void PipelineStageStateBase::pushConstantBuffers(ID3D12Device *device, ShaderResourceViewDescriptorHeapManager &heap,
  ConstBufferStreamDescriptorHeap &stream_heap, D3D12_CONSTANT_BUFFER_VIEW_DESC default_const_buffer, uint32_t cb_mask,
  StatefulCommandBuffer &cmd, uint32_t stage, ConstantBufferPushMode mode)
{
  const uint32_t count = ConstantBufferPushMode::DESCRIPTOR_HEAP == mode ? nau::math::__popcount(cb_mask) : 0;
  if (bRegisterValidMask == cb_mask)
  {
    if (ConstantBufferPushMode::DESCRIPTOR_HEAP == mode)
    {
      // verify that the descriptors are really there, backend supports on per pipeline basis mode switch
      if (bRegisterDescribtorRange.count == count)
      {
        return;
      }
    }
    else
    {
      return;
    }
  }

  bRegisterValidMask = cb_mask;

  if (0 == cb_mask)
  {
    bRegisterDescribtorRange = {};
    return;
  }

  if (ConstantBufferPushMode::DESCRIPTOR_HEAP == mode)
  {
    const auto base = stream_heap.getDescriptors(device, count);
    const auto width = stream_heap.getDescriptorSize();
    auto pos = base;

    auto defView = &constRegisterLastBuffer;

    for (auto i : nau::math::LsbVisitor{cb_mask}) // pos may be changed on each iteration
    {
      auto view = bRegisters[i].BufferLocation ? &bRegisters[i] : defView;
      defView = nullptr;
      device->CreateConstantBufferView(view, pos);
      pos.ptr += width;
    }

    auto index = heap.appendToConstScratchSegment(device, base, count);
    bRegisterDescribtorRange = DescriptorHeapRange::make(index, count);
  }
  else
  {
    auto defAdr = constRegisterLastBuffer.BufferLocation;

    for (auto i : nau::math::LsbVisitor{cb_mask})
    {
      auto adr = bRegisters[i].BufferLocation ? bRegisters[i].BufferLocation : defAdr;
      defAdr = default_const_buffer.BufferLocation;
      cmd.setConstantBuffer(stage, i, adr);
    }
  }
}


inline void PipelineStageStateBase::pushSamplers(ID3D12Device *device, SamplerDescriptorHeapManager &heap,
  D3D12_CPU_DESCRIPTOR_HANDLE default_sampler, D3D12_CPU_DESCRIPTOR_HANDLE default_cmp_sampler, uint32_t sampler_mask,
  uint32_t cmp_sampler_mask)
{
  // mask has to match exactly as a potentially sparse array is condensed into a continuous array
  if (sRegisterValidMask == sampler_mask)
  {
    return;
  }
  sRegisterValidMask = sampler_mask;

  D3D12_CPU_DESCRIPTOR_HANDLE samplerTable[dxil::MAX_S_REGISTERS];
  auto it = samplerTable;

  for (auto i : nau::math::LsbVisitor{sampler_mask})
  {
    *it++ = sRegisters[i].ptr ? sRegisters[i] : ((cmp_sampler_mask & (1u << i)) ? default_cmp_sampler : default_sampler);
  }

  uint32_t count = eastl::distance(samplerTable, it);
  auto index = heap.findInScratchSegment(samplerTable, count);
  if (!index)
  {
    heap.ensureScratchSegmentSpace(device, count);
    index = heap.appendToScratchSegment(device, samplerTable, count);
  }
  sRegisterDescriptorRange = DescriptorHeapRange::make(index, count);
}

inline void PipelineStageStateBase::pushUnorderedViews(ID3D12Device *device, ShaderResourceViewDescriptorHeapManager &heap,
  const NullResourceTable &null_table, uint32_t uav_mask, const uint8_t *uav_types)
{
  if (uRegisterValidMask == uav_mask)
  {
    return;
  }
  uRegisterValidMask = uav_mask;

  D3D12_CPU_DESCRIPTOR_HANDLE table[dxil::MAX_U_REGISTERS];
  auto it = table;

  for (auto i : nau::math::LsbVisitor{uav_mask})
  {
    auto resType = static_cast<D3D_SHADER_INPUT_TYPE>(uav_types[i] & 0xF);
    auto resDim = static_cast<D3D_SRV_DIMENSION>(uav_types[i] >> 4);
    *it++ = uRegisters[i].is(resType, resDim) ? uRegisters[i].view : null_table.get(resType, resDim);
  }

  uint32_t count = eastl::distance(table, it);

  DescriptorHeapIndex index;
#if DX12_REUSE_UNORDERD_ACCESS_VIEW_DESCRIPTOR_RANGES
  index = heap.findInUAVScratchSegment(table, count);
  if (!index)
#endif
  {
    index = heap.appendToUAVScratchSegment(device, table, count);
  }
  uRegisterDescriptorRange = DescriptorHeapRange::make(index, count);
}

inline void PipelineStageStateBase::pushShaderResourceViews(ID3D12Device *device, ShaderResourceViewDescriptorHeapManager &heap,
  const NullResourceTable &null_table, uint32_t srv_mask, const uint8_t *srv_types)
{
  if (tRegisterValidMask == srv_mask)
  {
    return;
  }
  tRegisterValidMask = srv_mask;

  D3D12_CPU_DESCRIPTOR_HANDLE table[dxil::MAX_T_REGISTERS];
  auto it = table;

  for (auto i : nau::math::LsbVisitor{srv_mask})
  {
    auto resType = static_cast<D3D_SHADER_INPUT_TYPE>(srv_types[i] & 0xF);
    auto resDim = static_cast<D3D_SRV_DIMENSION>(srv_types[i] >> 4);
    *it++ = tRegisters[i].is(resType, resDim) ? tRegisters[i].view : null_table.get(resType, resDim);
  }

  uint32_t count = eastl::distance(table, it);

  DescriptorHeapIndex index;
#if DX12_REUSE_SHADER_RESOURCE_VIEW_DESCRIPTOR_RANGES
  if (heap.highSRVScratchSegmentUsage())
  {
    index = heap.findInSRVScratchSegment(table, count);
  }
  else
  {
    index = DescriptorHeapIndex::make_invalid();
  }
  if (!index)
#endif
  {
    index = heap.appendToSRVScratchSegment(device, table, count);
  }
  tRegisterDescriptorRange = DescriptorHeapRange::make(index, count);
}

inline void PipelineStageStateBase::migrateAllSamplers(ID3D12Device *device, SamplerDescriptorHeapManager &heap)
{
  if (!sRegisterDescriptorRange)
  {
    return;
  }

  sRegisterDescriptorRange = heap.migrateToActiveScratchSegment(device, sRegisterDescriptorRange);
  if (!sRegisterDescriptorRange)
  {
    NAU_FAILURE("DX12: Did run out of sampler descriptor heap space");
  }
}

inline uint64_t DeviceContext::getCompletedFenceProgress() { return front.completedFrameProgress; }

inline Extent2D FramebufferInfo::makeDrawArea(Extent2D def /*= {}*/) const
{
  // if swapchain for 0 is used we need to check depth stencil use,
  // in some cases depth stencil was used with swapchain where it
  // was smaller than the swapchain images and crashes.
  if ((attachmentMask.colorAttachmentMask & 1) && (colorAttachments[0].image))
  {
    return colorAttachments[0].image->getMipExtents2D(colorAttachments[0].view.getMipBase());
  }
  else if (auto mask = attachmentMask.colorAttachmentMask >> 1)
  {
    // Need to add 1 as we shifted one down
    auto index = nau::math::__bsf_unsafe(mask) + 1;
    return colorAttachments[index].image->getMipExtents2D(colorAttachments[index].view.getMipBase());
  }

  if (attachmentMask.hasDepthStencilAttachment && depthStencilAttachment.image)
  {
    return depthStencilAttachment.image->getMipExtents2D(depthStencilAttachment.view.getMipBase());
  }
  return def;
}

inline bool FrontendQueryManager::postRecovery(Device &device, ID3D12Device *dx_device)
{
  for (auto &&heap : predicateHeaps)
  {
    D3D12_QUERY_HEAP_DESC desc;
    desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    desc.Count = heap_size;
    desc.NodeMask = 0;
    if (!DX12_CHECK_OK(dx_device->CreateQueryHeap(&desc, COM_ARGS(&heap.heap))))
    {
      return false;
    }

    heap.buffer = device.createBuffer(sizeof(uint64_t) * heap_size, sizeof(uint64_t), 1, DeviceMemoryClass::DEVICE_RESIDENT_BUFFER,
      D3D12_RESOURCE_FLAG_NONE, 0, u8"<query resolve buffer>");
    if (!heap.buffer)
    {
      return false;
    }
  }
  return true;
}

inline FrontendQueryManager::HeapPredicate *FrontendQueryManager::newPredicateHeap(Device &device, ID3D12Device *dx_device)
{
  FrontendQueryManager::HeapPredicate newHeap;
  D3D12_QUERY_HEAP_DESC desc;
  desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  desc.Count = heap_size;
  desc.NodeMask = 0;
  if (!DX12_CHECK_OK(dx_device->CreateQueryHeap(&desc, COM_ARGS(&newHeap.heap))))
  {
    return nullptr;
  }

#if _TARGET_XBOX
  // predication buffer is the same as indirect buffer (same state id - so needs this flag)
  D3D12_RESOURCE_FLAGS flags = RESOURCE_FLAG_ALLOW_INDIRECT_BUFFER;
#else
  D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
#endif

  newHeap.buffer = device.createBuffer(sizeof(uint64_t) * heap_size, sizeof(uint64_t), 1, DeviceMemoryClass::DEVICE_RESIDENT_BUFFER,
    flags, 0, u8"<query resolve buffer>");
  if (!newHeap.buffer)
  {
    return nullptr;
  }
  return &predicateHeaps.emplace_back(eastl::move(newHeap));
}
inline uint64_t FrontendQueryManager::createPredicate(Device &device, ID3D12Device *dx_device)
{
  dag::CSAutoLock lock(predicateGuard);
  HeapPredicate *ptr = nullptr;
  auto ref = eastl::find_if(begin(predicateHeaps), end(predicateHeaps),
    [](const FrontendQueryManager::HeapPredicate &heap) { return heap.hasAnyFree(); });
  if (ref == end(predicateHeaps))
  {
    ptr = newPredicateHeap(device, dx_device);
  }
  else
  {
    ptr = &*ref;
  }
  if (!ptr)
  {
    return ~uint64_t(0);
  }
  Query *q = newQuery();
  auto slotIndex = ptr->addQuery(q);
  auto blockIndex = ptr - predicateHeaps.data();
  auto index = blockIndex * heap_size + slotIndex;
  q->setId(index, Query::Qtype::SURVEY);
  return static_cast<uint64_t>(q->getId());
}
inline void FrontendQueryManager::shutdownPredicate(DeviceContext &ctx)
{
  dag::CSAutoLock lock(predicateGuard);
  for (auto &&heap : predicateHeaps)
  {
    for (int i = 0; i < heap_size; i++)
    {
      if (heap.qArr[i])
        deleteQuery(heap.qArr[i]);
    }
    ctx.destroyBuffer(eastl::move(heap.buffer), u8"QueryBuffer");
  }
  predicateHeaps.clear();
}
#define CHECK_IMAGE_TYPE(type)                                \
  if (!image)                                                 \
    DAG_FATAL("Expected an image but was a buffer!");         \
  else if (image->getType() != type)                          \
    DAG_FATAL("Expected an image of type %u but was %u", /**/ \
      type, image->getType());                                \
  else                                                        \
    return true;

#define CHECK_BUFFER                                  \
  if (!buffer)                                        \
    DAG_FATAL("Expected an buffer but was a image!"); \
  else                                                \
    return true;

#define ENABLE_RESOURCE_CHECK 0

inline static bool check_tregister(Image *image, const BufferResourceReference &buffer, D3D12_CPU_DESCRIPTOR_HANDLE view,
  D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dim)
{
  if (view.ptr == 0)
    return false;

#if ENABLE_RESOURCE_CHECK
  if (D3D_SIT_CBUFFER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_TBUFFER == type)
  {
    return !!buffer;
  }
  else if (D3D_SIT_TEXTURE == type)
  {
    if (dim == D3D_SRV_DIMENSION_BUFFER || dim == D3D_SRV_DIMENSION_BUFFEREX)
      CHECK_BUFFER;

    if (!image)
      return false;

    switch (dim)
    {
      case D3D_SRV_DIMENSION_UNKNOWN: return true;
      case D3D_SRV_DIMENSION_TEXTURE1D:
      case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
      case D3D_SRV_DIMENSION_TEXTURE2DMS:
      case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: DAG_FATAL("unexpected descriptor check"); break;
      case D3D_SRV_DIMENSION_TEXTURE2D: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURE2DARRAY: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURE3D: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE3D); break;
      case D3D_SRV_DIMENSION_TEXTURECUBE: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
    }
  }
  else if (D3D_SIT_SAMPLER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWTYPED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_STRUCTURED == type)
  {
    CHECK_BUFFER;
  }
  else if (D3D_SIT_UAV_RWSTRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_BYTEADDRESS == type)
  {
    CHECK_BUFFER;
  }
  else if (D3D_SIT_UAV_RWBYTEADDRESS == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_APPEND_STRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_CONSUME_STRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  return false;
#else
  G_UNUSED(image);
  G_UNUSED(buffer);
  G_UNUSED(dim);
  G_UNUSED(type);
  return true;
#endif
}

inline bool PipelineStageStateBase::TRegister::is(D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dim)
{
  if (12 == type) // rtx structure TODO replace with correct symbol
  {
#if D3D_HAS_RAY_TRACING
    return view.ptr != 0 && as != nullptr;
#else
    DAG_FATAL("unexpected descriptor check");
#endif
  }
  return check_tregister(image, buffer, view, type, dim);
}

inline bool PipelineStageStateBase::URegister::is(D3D_SHADER_INPUT_TYPE type, D3D_SRV_DIMENSION dim)
{
  if (view.ptr == 0)
    return false;

#if ENABLE_RESOURCE_CHECK
  if (D3D_SIT_CBUFFER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_TBUFFER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_TEXTURE == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_SAMPLER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWTYPED == type)
  {
    switch (dim)
    {
      case D3D_SRV_DIMENSION_UNKNOWN: return (!!buffer) || image;
      case D3D_SRV_DIMENSION_TEXTURE1D:
      case D3D_SRV_DIMENSION_TEXTURE1DARRAY:
      case D3D_SRV_DIMENSION_TEXTURE2DMS:
      case D3D_SRV_DIMENSION_TEXTURE2DMSARRAY: DAG_FATAL("unexpected descriptor lookup"); break;
      case D3D_SRV_DIMENSION_BUFFER:
      case D3D_SRV_DIMENSION_BUFFEREX: CHECK_BUFFER; break;
      case D3D_SRV_DIMENSION_TEXTURE2D: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURE2DARRAY: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURE3D: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE3D); break;
      case D3D_SRV_DIMENSION_TEXTURECUBE: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
      case D3D_SRV_DIMENSION_TEXTURECUBEARRAY: CHECK_IMAGE_TYPE(D3D12_RESOURCE_DIMENSION_TEXTURE2D); break;
    }
  }
  else if (D3D_SIT_STRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWSTRUCTURED == type)
  {
    CHECK_BUFFER;
  }
  else if (D3D_SIT_BYTEADDRESS == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWBYTEADDRESS == type)
  {
    CHECK_BUFFER;
  }
  else if (D3D_SIT_UAV_APPEND_STRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_CONSUME_STRUCTURED == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER == type)
  {
    DAG_FATAL("unexpected descriptor check");
  }
  else if (12 == type) // rtx structure TODO replace with correct symbol
  {
    DAG_FATAL("unexpected descriptor check");
  }
  return false;
#else
  G_UNUSED(dim);
  G_UNUSED(type);
  return true;
#endif
}

#undef CHECK_BUFFER
#undef CHECK_IMAGE_TYPE

inline DynamicArray<StaticRenderStateID> RenderStateSystem::loadStaticStatesFromBlk(DeviceContext &ctx, const Driver3dDesc &desc,
  const nau::DataBlock *blk, const char *default_format)
{
  DynamicArray<StaticRenderStateID> renderStateIdTable{blk->blockCount()};
  pipeline::DataBlockDecodeEnumarator<pipeline::RenderStateDecoder> decoder{*blk, 0, default_format};
  for (; !decoder.completed(); decoder.next())
  {
    auto rsi = decoder.index();
    renderStateIdTable[rsi] = StaticRenderStateID::Null();
    decoder.invoke([rsi, this, &desc, &renderStateIdTable, &ctx](auto &state) {
      if (this->is_compatible(desc, state))
      {
        renderStateIdTable[rsi] = this->registerStaticState(ctx, state);
      }
      return true;
    });
  }
  return renderStateIdTable;
}

inline StaticRenderStateID RenderStateSystem::registerStaticState(DeviceContext &ctx, const StaticState &def)
{
  auto ref = eastl::find(begin(staticStateTable), end(staticStateTable), def);
  if (ref == end(staticStateTable))
  {
    ref = staticStateTable.insert(end(staticStateTable), def);
    ctx.registerStaticRenderState(StaticRenderStateID{static_cast<int>(ref - begin(staticStateTable))}, *ref);
  }
  return StaticRenderStateID{static_cast<int>(ref - begin(staticStateTable))};
}

inline BufferInterfaceConfigCommon::BufferType PlatformBufferInterfaceConfig::createBuffer(uint32_t size, uint32_t structure_size,
  uint32_t discard_count, MemoryClass memory_class, uint32_t buf_flags, const char8_t *name)
{
  auto &device = get_device();
  BufferType buf = device.createBuffer(size, structure_size, discard_count, memory_class, getResourcFlags(buf_flags), buf_flags, name);
  return buf;
}

inline BufferInterfaceConfigCommon::BufferType PlatformBufferInterfaceConfig::discardBuffer(GenericBufferInterface *self,
  BufferReferenceType current_buffer, uint32_t size, uint32_t structure_size, MemoryClass memory_class, FormatStore view_format,
  uint32_t buf_flags, const char8_t *name)
{
  G_UNUSED(size);
  auto &device = get_device();
  auto nextBuffer = device.getContext().discardBuffer(eastl::move(current_buffer), memory_class, view_format, structure_size,
    0 != (buf_flags & SBCF_MISC_ALLOW_RAW), 0 != (buf_flags & SBCF_MISC_STRUCTURED), getResourcFlags(buf_flags), buf_flags, name);
  // notify state system, that it has to mark all uses as dirty
  notify_discard(self, SBCF_BIND_VERTEX & buf_flags, SBCF_BIND_CONSTANT & buf_flags, SBCF_BIND_SHADER_RES & buf_flags,
    SBCF_BIND_UNORDERED & buf_flags);
  return nextBuffer;
}

inline void BufferInterfaceConfigCommon::deleteBuffer(BufferReferenceType buffer, const char8_t *name)
{
  get_device().getContext().destroyBuffer(eastl::move(buffer), name);
}

inline void BufferInterfaceConfigCommon::addRawResourceView(BufferReferenceType buffer)
{
  get_device().addBufferView(buffer, BufferViewType::SRV, BufferViewFormating::RAW, FormatStore{}, 4);
}
inline void BufferInterfaceConfigCommon::addStructuredResourceView(BufferReferenceType buffer, uint32_t struct_size)
{
  get_device().addBufferView(buffer, BufferViewType::SRV, BufferViewFormating::STRUCTURED, FormatStore{}, struct_size);
}
inline void BufferInterfaceConfigCommon::addShaderResourceView(BufferReferenceType buffer, FormatStore format)
{
  get_device().addBufferView(buffer, BufferViewType::SRV, BufferViewFormating::FORMATED, format, 1);
}
inline void BufferInterfaceConfigCommon::addRawUnorderedView(BufferReferenceType buffer)
{
  get_device().addBufferView(buffer, BufferViewType::UAV, BufferViewFormating::RAW, FormatStore{}, 4);
}
inline void BufferInterfaceConfigCommon::addStructuredUnorderedView(BufferReferenceType buffer, uint32_t struct_size)
{
  get_device().addBufferView(buffer, BufferViewType::UAV, BufferViewFormating::STRUCTURED, FormatStore{}, struct_size);
}
inline void BufferInterfaceConfigCommon::addUnorderedAccessView(BufferReferenceType buffer, FormatStore format)
{
  get_device().addBufferView(buffer, BufferViewType::UAV, BufferViewFormating::FORMATED, format, 1);
}

inline void BufferInterfaceConfigCommon::onDestroyRequest(GenericBufferInterface *self)
{
  notify_delete(self);
  get_device().deleteBufferObject(self);
}

inline HostDeviceSharedMemoryRegion BufferInterfaceConfigCommon::allocateTemporaryUploadMemory(uint32_t size)
{
  return get_device().allocateTemporaryUploadMemory(size, 1);
}

inline void BufferInterfaceConfigCommon::updateBuffer(HostDeviceSharedMemoryRegion mem, GenericBufferInterface *self,
  uint32_t buf_flags, BufferReferenceType buffer, uint32_t dst_offset)
{
  G_UNUSED(self);
  G_UNUSED(buf_flags);
  get_device().getContext().updateBuffer(mem, {buffer, dst_offset});
}

inline void BufferInterfaceConfigCommon::copyBuffer(GenericBufferInterface *src_buf, uint32_t src_flags, BufferReferenceType src,
  TemporaryMemoryType src_stream, bool src_is_stream, GenericBufferInterface *dst_buf, uint32_t dst_flags, BufferReferenceType dst,
  TemporaryMemoryType dst_stream, bool dst_is_stream, uint32_t src_offset, uint32_t dst_offset, uint32_t size)
{
  G_UNUSED(src_buf);
  G_UNUSED(src_flags);
  G_UNUSED(dst_buf);
  G_UNUSED(dst_flags);
  src.resourceId.markUsedAsCopySourceBuffer();

  BufferReference srcRef = src;
  if (src_is_stream)
  {
    srcRef = BufferReference{src_stream};
  }

  BufferReference dstRef = dst;
  if (dst_is_stream)
  {
    dstRef = BufferReference{dst_stream};
  }

  get_device().getContext().copyBuffer({srcRef, src_offset}, {dstRef, dst_offset}, size);
}

inline void BufferInterfaceConfigCommon::invalidateMappedRange(BufferReferenceType buffer, uint32_t offset, uint32_t size)
{
  buffer.invalidateMappedMemory(offset, size);
}
inline void BufferInterfaceConfigCommon::flushMappedRange(BufferReferenceType buffer, uint32_t offset, uint32_t size)
{
  buffer.flushMappedMemory(offset, size);
}
inline uint8_t *BufferInterfaceConfigCommon::getMappedPointer(BufferReferenceType buffer, uint32_t offset)
{
  return buffer.cpuPointer + buffer.size * buffer.currentDiscardIndex + offset;
}
inline void BufferInterfaceConfigCommon::invalidateMemory(HostDeviceSharedMemoryRegion mem, uint64_t offset, uint32_t size)
{
  mem.invalidateRegion(make_value_range(offset, size));
}
inline void BufferInterfaceConfigCommon::readBackBuffer(BufferReferenceType src, HostDeviceSharedMemoryRegion dst, uint32_t src_offset,
  uint32_t dst_offset, uint32_t size)
{
  get_device().getContext().readBackFromBuffer(dst, dst_offset, {src, src_offset, size});
}
inline void BufferInterfaceConfigCommon::blockingReadBackBuffer(BufferReferenceType src, HostDeviceSharedMemoryRegion dst,
  uint32_t src_offset, uint32_t dst_offset, uint32_t size)
{
  readBackBuffer(src, dst, src_offset, dst_offset, size);
  get_device().getContext().wait();
}
inline void BufferInterfaceConfigCommon::flushBuffer(BufferReferenceType, uint32_t, uint32_t)
{
  // Done automatically at the end of each command list
}
inline void BufferInterfaceConfigCommon::blockingFlushBuffer(BufferReferenceType src, uint32_t offset, uint32_t size)
{
  flushBuffer(src, offset, size);
  get_device().getContext().wait();
}

inline void BufferInterfaceConfigCommon::uploadBuffer(HostDeviceSharedMemoryRegion src, BufferReferenceType dst, uint32_t src_offset,
  uint32_t dst_offset, uint32_t size)
{
  get_device().getContext().uploadToBuffer({dst, dst_offset, size}, src, src_offset);
}

inline void BufferInterfaceConfigCommon::freeMemory(HostDeviceSharedMemoryRegion mem) { get_device().getContext().freeMemory(mem); }
inline HostDeviceSharedMemoryRegion BufferInterfaceConfigCommon::allocateReadWriteStagingMemory(uint32_t size)
{
  return get_device().allocatePersistentBidirectionalMemory(size, 16);
}
inline HostDeviceSharedMemoryRegion BufferInterfaceConfigCommon::allocateReadOnlyStagingMemory(uint32_t size)
{
  return get_device().allocatePersistentReadBackMemory(size, 16);
}
inline HostDeviceSharedMemoryRegion BufferInterfaceConfigCommon::allocateWriteOnlyStagingMemory(uint32_t size)
{
  return get_device().allocatePersistentUploadMemory(size, 16);
}

inline uint32_t BufferInterfaceConfigCommon::getBufferSize(BufferConstReferenceType buffer)
{
  return buffer.size * buffer.discardCount;
}

inline uint32_t BufferInterfaceConfigCommon::minBufferSize(uint32_t cflags)
{
  G_UNUSED(cflags);
  return D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
}

inline void BufferInterfaceConfigCommon::pushBufferUpdate(GenericBufferInterface *self, uint32_t buf_flags, BufferReferenceType buffer,
  uint32_t offset, const void *src, uint32_t size)
{
  G_UNUSED(self);
  G_UNUSED(buf_flags);
  get_device().getContext().pushBufferUpdate({buffer, offset}, src, size);
}

inline BufferInterfaceConfigCommon::TemporaryMemoryType BufferInterfaceConfigCommon::discardStreamMememory(
  GenericBufferInterface *self, uint32_t size, uint32_t struct_size, uint32_t flags, TemporaryMemoryType prev_memory, const char8_t *name)
{
  G_UNUSED(struct_size);
  G_UNUSED(prev_memory);
  G_UNUSED(name);
  notify_discard(self, SBCF_BIND_VERTEX & flags, SBCF_BIND_CONSTANT & flags, SBCF_BIND_SHADER_RES & flags,
    SBCF_BIND_UNORDERED & flags);
  uint32_t alignment = (SBCF_BIND_CONSTANT & flags) ? D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT : sizeof(uint32_t);
  return get_device().getContext().allocatePushMemory(size, alignment);
}

inline bool BufferInterfaceConfigCommon::allowsStreamBuffer(uint32_t flags)
{
  // UAV makes no sense.
  // SRV would work but needs some sort of temp descriptor heap.
  constexpr uint32_t usupported_uses_mask = SBCF_BIND_SHADER_RES | SBCF_BIND_UNORDERED;
  if (0 != (usupported_uses_mask & flags))
  {
    return false;
  }

  auto &device = get_device();
  if (device.allowStreamBuffers())
  {
    if (!device.allowConstStreamBuffers())
    {
      if (SBCF_BIND_CONSTANT & flags)
      {
        return false;
      }
    }

    if (!device.allowVertexStreamBuffers())
    {
      if (SBCF_BIND_VERTEX & flags)
      {
        return false;
      }
    }

    if (!device.allowIndexStreamBuffers())
    {
      if (SBCF_BIND_INDEX & flags)
      {
        return false;
      }
    }

    if (!device.allowIndirectStreamBuffer())
    {
      if (SBCF_MISC_DRAWINDIRECT & flags)
      {
        return false;
      }
    }

    if (!device.allowStagingStreamBuffer())
    {
      if ((0 == (SBCF_BIND_MASK & flags)) && (SBCF_CPU_ACCESS_MASK == (SBCF_CPU_ACCESS_MASK & flags)))
      {
        return false;
      }
    }

    return true;
  }
  else
  {
    return false;
  }
}

#if _TARGET_PC_WIN
inline bool PlatformBufferInterfaceConfig::isMapable(BufferReferenceType buf) { return buf.cpuPointer != nullptr; }
Device::Config update_config_for_vendor(Device::Config config, const nau::DataBlock *cfg, Device::AdapterInfo &adapterInfo);
#endif

Device::Config get_device_config(const nau::DataBlock *cfg);

inline BaseTex *DeviceContext::getSwapchainColorTexture(SWAPID id)
{
    //NAU_LOG("getSwapchainColorTexture {}", back.addSwapchain.currentColorTargetIndex);
    NAU_ASSERT(back.addSwapchains.count(id));
    return back.addSwapchains[id].getColorTexture();
}
inline eastl::vector<BaseTex*> DeviceContext::getSwapchainsColorTextures()
{
    eastl::vector<BaseTex*> scTextures;
    scTextures.push_back(getSwapchainColorTexture());
    for (auto& [id, swapchain] : back.addSwapchains)
    {
        scTextures.push_back(swapchain.getColorTexture());
    }
    return scTextures;
}
inline BaseTex *DeviceContext::getSwapchainColorTexture() { return front.swapchain.getColorTexture(); }

inline BaseTex *DeviceContext::getSwapchainSecondaryColorTexture() { return front.swapchain.getSecondaryColorTexture(); }

inline BaseTex *DeviceContext::getSwapchainDepthStencilTexture(Extent2D ext)
{
  return front.swapchain.getDepthStencilTexture(device, ext);
}

inline BaseTex *DeviceContext::getSwapchainDepthStencilTextureAnySize() { return front.swapchain.getDepthStencilTextureAnySize(); }

inline Extent2D DeviceContext::getSwapchainExtent() const { return front.swapchain.getExtent(); }

inline bool DeviceContext::isVrrSupported() const { return back.swapchain.isVrrSupported(); }

inline bool DeviceContext::isVsyncOn() const { return front.swapchain.isVsyncOn(); }

inline FormatStore DeviceContext::getSwapchainDepthStencilFormat() const { return front.swapchain.getDepthStencilFormat(); }

inline FormatStore DeviceContext::getSwapchainColorFormat() const { return front.swapchain.getColorFormat(); }

inline FormatStore DeviceContext::getSwapchainSecondaryColorFormat() const { return front.swapchain.getSecondaryColorFormat(); }

inline Extent2D frontend::Swapchain::getExtent() const
{
  Extent2D extent{};
  if (swapchainColorTex)
  {
    extent.width = swapchainColorTex->width;
    extent.height = swapchainColorTex->height;
  }
  return extent;
}

inline void frontend::Swapchain::bufferResize(const Extent2D &extent)
{
  swapchainColorTex->setParams(extent.width, extent.height, 1, 1, u8"swapchain color target");
}

inline FormatStore frontend::Swapchain::getColorFormat() const { return swapchainColorTex->getFormat(); }

inline FormatStore frontend::Swapchain::getSecondaryColorFormat() const
{
#if _TARGET_XBOX
  if (swapchainSecondaryColorTex)
    return swapchainSecondaryColorTex->getFormat();
#endif
  return {};
}


#if _TARGET_PC_WIN
inline void frontend::Swapchain::preRecovery()
{
  swapchainDepthStencilTex->tex.image = nullptr;
  swapchainColorTex->tex.stagingMemory = {};
  waitableObject.reset();
}
#endif


inline bool is_swapchain_color_image(Image *img)
{
  // make pvs happy
  if (nullptr == img)
  {
    return false;
  }
  return swapchain_color_texture_global_id == img->getGlobalSubResourceIdBase() ||
         swapchain_secondary_color_texture_global_id == img->getGlobalSubResourceIdBase() || (img->swapChainIndex >= 0);
}

} // namespace drv3d_dx12


#endif //__DRV_DX12_DEVICE_H__
