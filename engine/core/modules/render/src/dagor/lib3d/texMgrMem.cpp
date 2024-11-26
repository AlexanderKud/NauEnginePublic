// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "texMgrData.h"
#include "nau/3d/dag_drv3d.h"
#include "nau/3d/dag_tex3d.h"
#include "nau/3d/dag_texPackMgr2.h"
#include "nau/dataBlock/dag_dataBlock.h"
#include "nau/threading/dag_atomic.h"
#include "nau/osApiWrappers/dag_cpuJobs.h"
#include "nau/osApiWrappers/dag_miscApi.h"
#include "nau/startup/dag_globalSettings.h"
#include "nau/startup/dag_globalSettings.h"

#include "nau/osApiWrappers/dag_threads.h"
#include "nau/osApiWrappers/dag_miscApi.h"
#include "nau/3d/tql.h"
#include <EASTL/vector_set.h>

#if _TARGET_PC_WIN
#include <windows.h>
#endif
#include "nau/diag/logging.h"
#include "nau/perfMon/dag_cpuFreq.h"

#include "nau/utils/span.h"

// #define RMGR_DUMP_TEX_STATE(IDX) RMGR.dumpTexState(IDX)  // trace textures state
#ifndef RMGR_DUMP_TEX_STATE
#define RMGR_DUMP_TEX_STATE(IDX) ((void)0)
#endif

using tql::dyn_qlev_decrease;
using tql::mem_quota_reserve_kb;
using tql::mem_used_persistent_kb;
using tql::sys_mem_add_free_mb;
using tql::sys_mem_usage_thres_mb;

static int mem_used_discardable_kb = 0;
static int mem_used_discardable_kb_max = 0, mem_used_persistent_kb_max = 0, mem_used_sum_kb_max = 0;
static int tex_used_discardable_cnt = 0, tex_used_persistent_cnt = 0;
static int mgr_log_level = 0;
using texmgr_internal::reload_jobmgr_id;
using texmgr_internal::RMGR;

static void load_tex_on_usage(eastl::span<const TEXTUREID> tid);
static void free_up_sys_mem(int mem_to_free_tb, bool force_report = false);
static void free_up_gpu_mem(int mem_to_free_kb, int actual_quota_kb);

void tql::get_tex_streaming_stats(int &_mem_used_discardable_kb, int &_mem_used_persistent_kb, int &_mem_used_discardable_kb_max,
  int &_mem_used_persistent_kb_max, int &_mem_used_sum_kb_max, int &_tex_used_discardable_cnt, int &_tex_used_persistent_cnt,
  int &_max_mem_used_overdraft_kb)
{
  _mem_used_discardable_kb = mem_used_discardable_kb;
  _mem_used_persistent_kb = mem_used_persistent_kb;
  _mem_used_discardable_kb_max = mem_used_discardable_kb_max;
  _mem_used_persistent_kb_max = mem_used_persistent_kb_max;
  _mem_used_sum_kb_max = mem_used_sum_kb_max;
  _tex_used_discardable_cnt = tex_used_discardable_cnt;
  _tex_used_persistent_cnt = tex_used_persistent_cnt;
  _max_mem_used_overdraft_kb = 0 /*-mem_quota_reserve_kb*/;
}

static void on_tex_created(BaseTexture *t)
{
  using namespace texmgr_internal;

  int sz = 4 * ((t->ressize() + 4095) / 4096);
  TEXTUREID tid = t->getTID();
  if (tid != BAD_TEXTUREID)
  {
    interlocked_add(mem_used_discardable_kb, sz);
    interlocked_increment(tex_used_discardable_cnt);
  }
  else
  {
    interlocked_add(mem_used_persistent_kb, sz);
    interlocked_increment(tex_used_persistent_cnt);
  }
  if (mem_used_persistent_kb_max < mem_used_persistent_kb)
    mem_used_persistent_kb_max = mem_used_persistent_kb;
  if (mem_used_discardable_kb_max < mem_used_discardable_kb)
    mem_used_discardable_kb_max = mem_used_discardable_kb;
  if (mem_used_sum_kb_max < mem_used_persistent_kb + mem_used_discardable_kb)
    mem_used_sum_kb_max = mem_used_persistent_kb + mem_used_discardable_kb;

  if (mgr_log_level >= 3)
    NAU_LOG_DEBUG("TEX+ {:p} {}K -> pers({})={}K discr({})={}K quota={}K (Vectormath::max: {}M/{}M sum={}M) {}", (void*)t, sz, tex_used_persistent_cnt,
      mem_used_persistent_kb, tex_used_discardable_cnt, mem_used_discardable_kb, tql::mem_quota_kb, mem_used_persistent_kb_max >> 10,
      mem_used_discardable_kb_max >> 10, mem_used_sum_kb_max >> 10, t->getTexName());
}
static void on_tex_released(BaseTexture *t)
{
  using namespace texmgr_internal;

  int sz = 4 * ((t->ressize() + 4095) / 4096);
  unsigned lfu = 0;
  bool is_loading = false;
  TEXTUREID tid = t->getTID();
  unsigned smd_sz = 0;
  if (tid != BAD_TEXTUREID)
  {
    interlocked_add(mem_used_discardable_kb, -sz);
    interlocked_decrement(tex_used_discardable_cnt);
    if (RMGR.isValidID(tid, nullptr))
    {
      lfu = RMGR.getResLFU(tid);
      is_loading = RMGR.resQS[tid.index()].isReading();
      smd_sz = RMGR.getTexBaseDataSize(tid);
    }
  }
  else
  {
    interlocked_add(mem_used_persistent_kb, -sz);
    interlocked_decrement(tex_used_persistent_cnt);
  }
  if (tex_used_persistent_cnt == 0 && tex_used_discardable_cnt == 0)
    NAU_LOG_DEBUG("TEX- {:p} {}K -> pers(0)={}K discr(0)={}K (Vectormath::max: {}M/{}M sum={}M)", (void*)t, sz, mem_used_persistent_kb, mem_used_discardable_kb,
      mem_used_persistent_kb_max >> 10, mem_used_discardable_kb_max >> 10, mem_used_sum_kb_max >> 10);
  else if (mgr_log_level >= 2 && smd_sz)
    NAU_LOG_DEBUG("TEX- {:p} {}K -> pers({})={}K discr({})={}K quota={}K LFU={} [sysMemCopy={}]", (void*)t, sz, tex_used_persistent_cnt,
      mem_used_persistent_kb, tex_used_discardable_cnt, mem_used_discardable_kb, tql::mem_quota_kb, lfu, smd_sz);
  else if (mgr_log_level >= 2 && !is_loading)
    NAU_LOG_DEBUG("TEX- {:p} {}K -> pers({})={}K discr({})={}K quota={}K LFU={}", (void*)t, sz, tex_used_persistent_cnt, mem_used_persistent_kb,
      tex_used_discardable_cnt, mem_used_discardable_kb, tql::mem_quota_kb, lfu);
  else if (mgr_log_level >= 3)
    NAU_LOG_DEBUG("TEX- {:p} {}K -> pers({})={}K discr({})={}K quota={}K", (void*)t, sz, tex_used_persistent_cnt, mem_used_persistent_kb,
      tex_used_discardable_cnt, mem_used_discardable_kb, tql::mem_quota_kb);
}
static void on_buf_changed(bool add, int delta_sz_kb)
{
  interlocked_add(mem_used_persistent_kb, delta_sz_kb);
  if (mgr_log_level >= 3)
    NAU_LOG_DEBUG("{} {:+d}K", add ? "BUF+" : "BUF-", delta_sz_kb);
  G_UNUSED(add);
}
static bool check_texture_id_valid(TEXTUREID tid) { return is_managed_texture_id_valid(tid, true); }

static void do_stop_bkg_tex_loading(int sleep_quant)
{
    // TODO: fix it
  //if (reload_jobmgr_id >= 0 && cpujobs::is_inited())
  //{
  //  cpujobs::reset_job_queue(reload_jobmgr_id, true);
  //  while (cpujobs::is_job_manager_busy(reload_jobmgr_id))
  //  {
  //    ddsx::cease_delayed_data_loading(0);
  //    if (ddsx::hq_tex_priority != 0)
  //      ddsx::cease_delayed_data_loading(ddsx::hq_tex_priority);
  //    cpujobs::reset_job_queue(reload_jobmgr_id, true);
  //    sleep_msec(sleep_quant);
  //  }
  //}
}
static void unload_on_drv_shutdown()
{
  using namespace texmgr_internal;
  do_stop_bkg_tex_loading(10);

  acquire_texmgr_lock();
  TEX_REC_LOCK();
  for (unsigned idx = 0, ie = RMGR.getAccurateIndexCount(); idx < ie; idx++)
  {
    if (RMGR.getRefCount(idx) < 0)
      continue;

    if (!RMGR.getRefCount(idx) && RMGR.getFactory(idx))
      if (BaseTexture *bt = RMGR.baseTexture(idx))
      {
        if (bt->getTID() == BAD_TEXTUREID) // only for non-texPackMgr2
          RMGR.setD3dRes(idx, nullptr);
        TEX_REC_UNLOCK();
        RMGR.getFactory(idx)->releaseTexture(bt, RMGR.toId(idx));
        TEX_REC_LOCK();
      }
  }
  TEX_REC_UNLOCK();
  release_texmgr_lock();
}

static void load_tex_on_usage_cb(void *data)
{
  int *list = (int *)(data);
  load_tex_on_usage(make_span_const((TEXTUREID *)(list + 1), list[0]));
  memfree(data, tmpmem);
}

static void on_frame_finished()
{
  using namespace texmgr_internal;

  //TIME_PROFILE(tql_on_frame_finished);

  static dag::CriticalSection critSec(__FUNCTION__);
  dag::CSAutoLock lock(critSec);

  {
    int *list = nullptr;
    {
      dag::CSAutoLock lock(tql::reqLDCritSection);
      if (!tql::reqLD.empty()) // we have to check under crit section!
      {
        list = (int *)memalloc(sizeof(int) * (1 + tql::reqLD.size()), tmpmem);
        list[0] = tql::reqLD.size();
        memcpy(list + 1, tql::reqLD.data(), tql::reqLD.size() * sizeof(int));
        tql::reqLD.clear();
      }
    }
    //TODO: add_delayed_callback
    //if (list)
    //  is_main_thread() ? load_tex_on_usage_cb(list) : add_delayed_callback(load_tex_on_usage_cb, list);
  }

  if (tql::mem_quota_kb && sys_mem_usage_thres_mb)
  {
    //size_t mem_used_mb = (dagor_memory_stat::get_memchunk_count(true) * 32 + dagor_memory_stat::get_memory_allocated(true)) >> 20;
    size_t mem_used_mb = 0;
#if _TARGET_PC_WIN
    MEMORYSTATUSEX msx;
    msx.dwLength = sizeof(msx);
    if (GlobalMemoryStatusEx(&msx))
      if ((msx.ullAvailPhys >> 20) < sys_mem_add_free_mb)
      {
        if (mgr_log_level >= 1)
          NAU_LOG_DEBUG("SYSMEM# AvailPhys={}M AvailVirtual={}M mem_used={}M", msx.ullAvailPhys >> 20, msx.ullAvailVirtual >> 20, mem_used_mb);
        mem_used_mb += sys_mem_add_free_mb;
        if (mem_used_mb < sys_mem_usage_thres_mb)
          mem_used_mb = sys_mem_usage_thres_mb + 1;
      }
#endif
    if (mem_used_mb > sys_mem_usage_thres_mb && RMGR.getTotalBdCount())
      free_up_sys_mem(int(mem_used_mb - sys_mem_usage_thres_mb + sys_mem_add_free_mb) << 10);
  }

  if (!(dagor_frame_no() & 0x7F))
  {
    int total_gpu_mem_sz_kb = 0, free_gpu_mem_sz_kb = 0;
    // we can't tune quota of vulkan as it can make some resources non resident,
    // and it is reflected on free GPU memory
    if (d3d::is_inited() && !d3d::get_driver_code().is(d3d::vulkan) &&
        d3d::driver_command(DRV3D_COMMAND_GET_VIDEO_MEMORY_BUDGET, (void *)&total_gpu_mem_sz_kb, (void *)&free_gpu_mem_sz_kb, nullptr))
    {
      int new_quota_kb =
        Vectormath::min(mem_used_persistent_kb + RMGR.getTotalUsedTexSzKB() + free_gpu_mem_sz_kb, total_gpu_mem_sz_kb) - tql::gpu_mem_reserve_kb;
      if (new_quota_kb + (2 << 10) < tql::mem_quota_kb || new_quota_kb > tql::mem_quota_kb + (64 << 10))
      {
        if (new_quota_kb > tql::mem_quota_limit_kb)
          new_quota_kb = tql::mem_quota_limit_kb;

        if (new_quota_kb != tql::mem_quota_kb)
        {
          NAU_LOG_DEBUG("freeGPUmem= {}M ({}M), so tql::mem_quota_kb changed ({}M -> {}M)", free_gpu_mem_sz_kb >> 10,
            total_gpu_mem_sz_kb >> 10, tql::mem_quota_kb >> 10, new_quota_kb >> 10);
          tql::mem_quota_kb = new_quota_kb;
        }
      }
    }
  }

  int actual_quota_kb = tql::mem_quota_kb - mem_used_persistent_kb - mem_quota_reserve_kb;
  if (actual_quota_kb < RMGR.getTotalUsedTexSzKB() + RMGR.getTotalAddMemNeededSzKB() && texmgr_internal::texq_load_on_demand)
    free_up_gpu_mem(RMGR.getTotalUsedTexSzKB() + RMGR.getTotalAddMemNeededSzKB() - actual_quota_kb, actual_quota_kb);

  if (!(dagor_frame_no() & 0x3FF))
  {
    //size_t mem_used_mb = (dagor_memory_stat::get_memchunk_count(true) * 32 + dagor_memory_stat::get_memory_allocated(true)) >> 20;
    //
    //if (RMGR.getTotalBdCount())
    //  NAU_LOG_DEBUG("SYSmem= {:4d}M in {} ptrs, BaseData: {}K in {} blk (ready to free {}K in {} blk), memQuota={}M+{}M", mem_used_mb,
    //    dagor_memory_stat::get_memchunk_count(true), RMGR.getTotalBdSzKB(), RMGR.getTotalBdCount(), RMGR.getReadyForDiscardBdSzKB(),
    //    RMGR.getReadyForDiscardBdCount(), sys_mem_usage_thres_mb, sys_mem_add_free_mb);
    //else
    //  NAU_LOG_DEBUG("SYSmem= {:4d}M in {} ptrs, memQuota={}M+{}M", mem_used_mb, dagor_memory_stat::get_memchunk_count(true),
    //    sys_mem_usage_thres_mb, sys_mem_add_free_mb);

    NAU_LOG_DEBUG("GPUmem= {:4d}M ({}M pers + {}M in {} tex), (ready to free {}K in {} tex) futureMem={}M, frame={:<5d} strmQuota={}M+{}M",
      (RMGR.getTotalUsedTexSzKB() + interlocked_relaxed_load(mem_used_persistent_kb)) >> 10,
      interlocked_relaxed_load(mem_used_persistent_kb) >> 10, RMGR.getTotalUsedTexSzKB() >> 10, RMGR.getTotalUsedTexCount(),
      RMGR.getReadyForDiscardTexSzKB(), RMGR.getReadyForDiscardTexCount(), RMGR.getTotalAddMemNeededSzKB() >> 10, dagor_frame_no(),
      actual_quota_kb >> 10, mem_quota_reserve_kb >> 10);
    //G_UNUSED(mem_used_mb);
  }

  RMGR.copyMaxReqLevToPrev();

  if (reload_jobmgr_id < 0)
    return;
  //if (!interlocked_acquire_load(texq_load_disabled) && (should_start_load_prio(0) || should_start_load_prio(ddsx::hq_tex_priority)))
  //{
  //  if (!should_start_load_prio(0) && get_pending_tex_prio(0))
  //    return;
  //  if (mgr_log_level >= 2)
  //    NAU_LOG_DEBUG("should start load ({},{})", (int)should_start_load_prio(0), (int)should_start_load_prio(ddsx::hq_tex_priority));
  //
  //  if (ddsx::hq_tex_priority != 0 && should_start_load_prio(0))
  //  {
  //    cpujobs::reset_job_queue(reload_jobmgr_id, true);
  //    ddsx::cease_delayed_data_loading(ddsx::hq_tex_priority);
  //  }
  //
  //  if (should_start_load_prio(0))
  //    ddsx::tex_pack2_perform_delayed_data_loading_async(0x700, reload_jobmgr_id, false, 4);
  //  if (ddsx::hq_tex_priority != 0 && should_start_load_prio(1))
  //  {
  //    ddsx::cease_delayed_data_loading(ddsx::hq_tex_priority);
  //    ddsx::tex_pack2_perform_delayed_data_loading_async(0x700 | ddsx::hq_tex_priority, reload_jobmgr_id, false, 4);
  //  }
  //}
}

static void load_tex_on_usage(eastl::span<const TEXTUREID> tid)
{
  using namespace texmgr_internal;

  int changed = 0;
  for (int i = 0; i < tid.size(); i++)
  {
    if (!RMGR.isValidID(tid[i], nullptr))
      continue;

    int idx = tid[i].index();
    if (auto f = RMGR.getFactory(idx))
      if (RMGR.scheduleReading(idx, f))
        changed++;
  }
  if (changed && reload_jobmgr_id >= 0)
  {
    if (mgr_log_level >= 2)
      NAU_LOG_DEBUG("start load tex on usage ({}/{} tex)", changed, tid.size());
  }
}
static void free_up_sys_mem(int mem_to_free_kb, bool force_report)
{
  //TIME_PROFILE(free_up_sys_mem);

  static int last_fc = 0, log_hide_cnt = 0;

  unsigned frame = dagor_frame_no();
  bool skip_log = (last_fc + 1 >= frame) && log_hide_cnt < 120;
  last_fc = frame;

  //if ((mgr_log_level >= 1 && !skip_log) || force_report)
  //  NAU_LOG_DEBUG("SYSMEM* {}M in {} ptrs; to_free={}K; BaseData: {}K in {} blk (ready to free {}K in {} blk) frame={} ({})",
  //    dagor_memory_stat::get_memory_allocated(true) >> 20, dagor_memory_stat::get_memchunk_count(true), mem_to_free_kb,
  //    RMGR.getTotalBdSzKB(), RMGR.getTotalBdCount(), RMGR.getReadyForDiscardBdSzKB(), RMGR.getReadyForDiscardBdCount(), frame,
  //    log_hide_cnt);
  log_hide_cnt = skip_log ? log_hide_cnt + 1 : 0;

  if (!RMGR.getReadyForDiscardBdCount())
    return;
  int target_bd_size = RMGR.getTotalBdSzKB() - mem_to_free_kb;
  int start_bd_count = RMGR.getTotalBdCount(), removed = 0, removed_sz_kb = 0;
  for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
    if (RMGR.getBaseTexUsedCount(i) == 0)
    {
      if (auto *bd = RMGR.replaceTexBaseData(i, nullptr, false))
      {
        removed_sz_kb += tql::sizeInKb(bd->sz);
        RMGR.decReadyForDiscardBd(tql::sizeInKb(bd->sz));
        texmgr_internal::D3dResMgrDataFinal::BaseData::free(bd);
        removed++;
      }
      if (RMGR.getTotalBdSzKB() < target_bd_size)
        break;
    }
  if (start_bd_count != RMGR.getTotalBdCount())
  {
    if (mgr_log_level >= 1)
      NAU_LOG_DEBUG("SYSMEM! removed {}K in {} blk; more_to_free={}K; BaseData: {}K in {} blk (ready to free {}K in {} blk) frame={}",
        removed_sz_kb, removed, RMGR.getTotalBdSzKB() - target_bd_size, RMGR.getTotalBdSzKB(), RMGR.getTotalBdCount(),
        RMGR.getReadyForDiscardBdSzKB(), RMGR.getReadyForDiscardBdCount(), frame);
    last_fc = frame - 2, log_hide_cnt = 0;
  }
}
static void free_up_gpu_mem(int mem_to_free_kb, int actual_quota_kb)
{
  //TIME_PROFILE(free_up_gpu_mem);

  static unsigned last_fc = 0, log_hide_cnt = 0;

  unsigned frame = dagor_frame_no();
  bool skip_log = (last_fc + 1 >= frame) && log_hide_cnt < 120;
  last_fc = frame;

  if (mgr_log_level >= 2 && !skip_log && mem_to_free_kb > RMGR.getTotalAddMemNeededSzKB())
    NAU_LOG_DEBUG("GPUMEM* {}M ({}M pers + {}M in {} tex); to_free={}K (ready to free {}M in {} tex), strmQuota={}M frame={} ({})",
      (RMGR.getTotalUsedTexSzKB() + interlocked_relaxed_load(mem_used_persistent_kb)) >> 10,
      interlocked_relaxed_load(mem_used_persistent_kb) >> 10, RMGR.getTotalUsedTexSzKB() >> 10, RMGR.getTotalUsedTexCount(),
      interlocked_relaxed_load(mem_to_free_kb), RMGR.getReadyForDiscardTexSzKB() >> 10, RMGR.getReadyForDiscardTexCount(),
      actual_quota_kb >> 10, frame, log_hide_cnt);
  log_hide_cnt = skip_log ? log_hide_cnt + 1 : 0;

  if (!RMGR.getTotalUsedTexCount())
    return;
  if (RMGR.getTotalUsedTexSzKB() < mem_to_free_kb && RMGR.getTotalUsedTexSzKB() <= RMGR.getTotalUsedTexCount() * 4)
    return;

  int target_tex_size = RMGR.getTotalUsedTexSzKB() + RMGR.getTotalAddMemNeededSzKB() - mem_to_free_kb, removed = 0, downgraded = 0;
#define BREAK_WHEN_CLEANED_ENOUGH_GPUMEM()                                              \
  if ((RMGR.getTotalUsedTexSzKB() + mem_quota_reserve_kb / 2 < actual_quota_kb) ||      \
      (RMGR.getTotalUsedTexSzKB() + RMGR.getTotalAddMemNeededSzKB() < target_tex_size)) \
  break
  if (RMGR.getReadyForDiscardTexSzKB() > RMGR.getReadyForDiscardTexCount() * 4 || mem_to_free_kb < RMGR.getReadyForDiscardTexSzKB())
  {
    bool evict_unused =
      RMGR.getReadyForDiscardTexSzKB() < RMGR.getReadyForDiscardTexCount() * 12 || d3d::get_driver_code().is(d3d::metal);
    for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
      if (RMGR.getResLFU(i) + 3 < frame && RMGR.getRefCount(i) == 0 && RMGR.getFactory(i) &&
          RMGR.texDesc[i].packRecIdx[TQL_base].pack >= 0 && RMGR.getTexMemSize4K(i) > 8 && RMGR.getD3dResRelaxed(i))
      {
        // RMGR.dumpTexState(i);
        if (evict_unused || RMGR.texDesc[i].dim.stubIdx < 0)
        {
          discard_unused_managed_texture(RMGR.toId(i));
          if (RMGR.baseTexture(i))
            continue; // failed to destroy
          // NAU_LOG_DEBUG("GPUMEM- evicted tex: {}  now={}M+{}M thres={}M @{}",
          //   RMGR.getName(i), RMGR.getTotalUsedTexSzKB()>>10, RMGR.getTotalAddMemNeededSzKB()>>10, target_tex_size>>10, frame);
        }
        else if (BaseTexture *t = RMGR.baseTexture(i))
        {
          if (!RMGR.downgradeTexQuality(i, *t, Vectormath::max<int>(RMGR.getLevDesc(i, TQL_thumb), 1)))
            continue; // failed to downgrade
          // NAU_LOG_DEBUG("GPUMEM- down-to-thumb tex: {}  now={}M+{}M thres={}M @{}",
          //   RMGR.getName(i), RMGR.getTotalUsedTexSzKB()>>10, RMGR.getTotalAddMemNeededSzKB()>>10, target_tex_size>>10, frame);
        }
        else
          continue; // nothing to remove
        removed++;
        BREAK_WHEN_CLEANED_ENOUGH_GPUMEM();
      }
      else if (RMGR.getResLFU(i) + 3 < frame && RMGR.getRefCount(i) == 0 && RMGR.getD3dResRelaxed(i) && RMGR.getFactory(i) &&
               RMGR.texDesc[i].packRecIdx[TQL_base].pack < 0)
      {
        BaseTexture *t = RMGR.baseTexture(i);
        if (!t)
          continue;

        int size_kb = tql::sizeInKb(t->ressize());
        if (size_kb < 16)
          continue;

        discard_unused_managed_texture(RMGR.toId(i));
        if (RMGR.baseTexture(i))
          continue; // failed to destroy

        target_tex_size += size_kb; // we discarded persistent tex, so strmQuota becomes larger
        // NAU_LOG_DEBUG("GPUMEM- evicted pers-tex: {} ({}K)  now={}M+{}M (+{}M pers) thres={}M @{}",
        //   RMGR.getName(i), size_kb, RMGR.getTotalUsedTexSzKB()>>10, RMGR.getTotalAddMemNeededSzKB()>>10,
        //   mem_used_persistent_kb>>10, target_tex_size>>10, frame);
        removed++;
        BREAK_WHEN_CLEANED_ENOUGH_GPUMEM();
      }
  }
  if (!texmgr_internal::is_gpu_mem_enough_to_load_hq_tex())
  {
    target_tex_size = tql::mem_quota_kb - tql::mem_used_persistent_kb - tql::mem_quota_reserve_kb;
    for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
      if ((RMGR.getRefCount(i) < 0) || RMGR.texDesc[i].dim.stubIdx < 0 || RMGR.texDesc[i].packRecIdx[TQL_base].pack < 0) //DAGOR_UNLIKELY
        continue;
      else if (RMGR.getResLFU(i) + 120 < frame && RMGR.getFactory(i) && RMGR.getD3dResRelaxed(i) && RMGR.getTexMemSize4K(i) > 256)
      {
        if (BaseTexture *t = RMGR.baseTexture(i))
          if (RMGR.downgradeTexQuality(i, *t, Vectormath::max<int>(RMGR.getLevDesc(i, TQL_thumb), 1)))
          {
            // NAU_LOG_DEBUG("GPUMEM- down tex: {}  rc={} now={}M+{}M thres={}M @{}", RMGR.getName(i), RMGR.getRefCount(i),
            //   RMGR.getTotalUsedTexSzKB()>>10, RMGR.getTotalAddMemNeededSzKB()>>10, target_tex_size>>10, frame);
            removed++;
            BREAK_WHEN_CLEANED_ENOUGH_GPUMEM();
          }
      }
      else if (downgraded < 10 && RMGR.getD3dResRelaxed(i) && RMGR.resQS[i].getLdLev() > RMGR.resQS[i].getMaxLev() &&
               !RMGR.resQS[i].isReading())
      {
        if (BaseTexture *t = RMGR.baseTexture(i))
          if (RMGR.downgradeTexQuality(i, *t, RMGR.resQS[i].getMaxLev()))
          {
            downgraded++;
            RMGR.dumpTexState(i);
            if (RMGR.getTotalUsedTexSzKB() < target_tex_size)
              break;
          }
      }
    if (dyn_qlev_decrease < 4 && RMGR.getReadyForDiscardTexSzKB() <= RMGR.getReadyForDiscardTexCount() * 12 &&
        (RMGR.getTotalAddMemNeededSzKB() >> (dyn_qlev_decrease * 2)) + RMGR.getTotalUsedTexSzKB() > actual_quota_kb)
    {
      dyn_qlev_decrease++;
      for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
        if (RMGR.getRefCount(i) >= 0 && RMGR.resQS[i].getQLev() > 9 && RMGR.texDesc[i].dim.stubIdx >= 0 &&
            RMGR.getTexImportance(i) < 1 && RMGR.resQS[i].getQLev() - dyn_qlev_decrease >= RMGR.getLevDesc(i, TQL_base))
          RMGR.changeTexMaxLev(i, RMGR.resQS[i].getQLev() - dyn_qlev_decrease);
      NAU_LOG_DEBUG("gpumem** switched to dyn_qlev_decrease={}", dyn_qlev_decrease);
    }

    for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
      if (int imp = RMGR.getTexImportance(i))
        interlocked_add(RMGR.texImportance[i], -imp + (RMGR.resQS[i].getQLev() == RMGR.resQS[i].getLdLev() ? 0 : 1));
  }

  if (dyn_qlev_decrease > 0 &&
      (RMGR.getTotalAddMemNeededSzKB() >> (dyn_qlev_decrease * 2 - 2)) + RMGR.getTotalUsedTexSzKB() < actual_quota_kb)
  {
    dyn_qlev_decrease--;
    for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
      if (RMGR.getRefCount(i) >= 0 && RMGR.resQS[i].getQLev() > 9 && RMGR.texDesc[i].dim.stubIdx >= 0 &&
          RMGR.resQS[i].getMaxLev() < RMGR.resQS[i].getQLev() - dyn_qlev_decrease)
        RMGR.changeTexMaxLev(i, RMGR.resQS[i].getQLev() - dyn_qlev_decrease);
    NAU_LOG_DEBUG("gpumem** switched back to dyn_qlev_decrease={}", dyn_qlev_decrease);
  }

  if (removed + downgraded > 0)
  {
    if (mgr_log_level >= 2)
      NAU_LOG_DEBUG("GPUMEM! removed {} tex and {} downgraded; more_to_free={}K ({}K); "
            "Now: {}M in {} tex and {}M needed (ready to free {}M in {} tex) frame={}",
        removed, downgraded, RMGR.getTotalUsedTexSzKB() + RMGR.getTotalAddMemNeededSzKB() - target_tex_size,
        RMGR.getTotalUsedTexSzKB() - actual_quota_kb, RMGR.getTotalUsedTexSzKB() >> 10, RMGR.getTotalUsedTexCount(),
        RMGR.getTotalAddMemNeededSzKB() >> 10, RMGR.getReadyForDiscardTexSzKB() >> 10, RMGR.getReadyForDiscardTexCount(), frame);
    last_fc = frame - 2, log_hide_cnt = 0;
  }
}
static bool texql_try_unload_tex_on_oom(size_t sz)
{
  // basically the same as free_up_sys_mem() but without any logs and with smarter early exit condition
  if (!RMGR.getReadyForDiscardBdCount())
    return false;
  size_t total_freed = 0;
  for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
    if (RMGR.getBaseTexUsedCount(i) == 0)
      if (auto *bd = RMGR.replaceTexBaseData(i, nullptr, false))
      {
        size_t free_sz_kb = bd->sz;
        RMGR.decReadyForDiscardBd(tql::sizeInKb(free_sz_kb));
        texmgr_internal::D3dResMgrDataFinal::BaseData::free(bd);
        if (free_sz_kb > sz)
          return true;
        total_freed += free_sz_kb;
      }
  return total_freed > sz;
}

static bool should_release_tex(BaseTexture *b)
{
  using namespace texmgr_internal;
  TEXTUREID tid = b ? b->getTID() : BAD_TEXTUREID;
  return (!is_gpu_mem_enough_to_load_hq_tex() || !texmgr_internal::texq_load_on_demand) &&
         (tid == BAD_TEXTUREID || RMGR.getTexMemSize4K(tid.index()) > 64);
}

void init_managed_textures_streaming_support(int _reload_jobmgr_id)
{
  if (_reload_jobmgr_id == -2)
    _reload_jobmgr_id = cpujobs::create_virtual_job_manager(256 << 10, WORKER_THREADS_AFFINITY_MASK, "reloadTex");
  if (d3d::is_inited())
  {
    NAU_FAILURE("init_managed_textures_streaming_support() must be called BEFORE d3d init");
    return;
  }
  if (tql::streaming_enabled)
  {
    NAU_FAILURE("init_managed_textures_streaming_support() double call");
    return;
  }
  const nau::DataBlock &b = *dgs_get_settings()->getBlockByNameEx("texStreaming");
  const nau::DataBlock &b_platform_spec = *b.getBlockByNameEx(nau::hal::get_platform_string_id());
#define READ_PROP(TYPE, NAME, DEF) b_platform_spec.get##TYPE(NAME, b.get##TYPE(NAME, DEF))

  if (!READ_PROP(Bool, "enableStreaming", true))
    return;

  reload_jobmgr_id = _reload_jobmgr_id;
  mgr_log_level = b.getInt("logLevel", 1);

  tql::get_tex_lfu = &get_managed_res_lfu;
  tql::get_tex_cur_ql = &get_managed_res_cur_tql;
  tql::check_texture_id_valid = &check_texture_id_valid;
  tql::on_tex_created = &on_tex_created;
  tql::on_tex_released = &on_tex_released;
  tql::on_buf_changed = &on_buf_changed;
  tql::unload_on_drv_shutdown = &unload_on_drv_shutdown;
  if (!b.getBool("alwaysRelease", false))
    texmgr_internal::should_release_tex = &should_release_tex;
  texmgr_internal::stop_bkg_tex_loading = &do_stop_bkg_tex_loading;
  mem_quota_reserve_kb = READ_PROP(Int, "quotaReserveMB", mem_quota_reserve_kb >> 10) << 10;
  sys_mem_usage_thres_mb = READ_PROP(Int, "sysMemUsageThresholdMB", sys_mem_usage_thres_mb);
  sys_mem_add_free_mb = READ_PROP(Int, "sysMemAddFreeMB", sys_mem_add_free_mb);
  texmgr_internal::enable_cur_ql_mismatch_assert = b.getBool("enableCurQLMismatchAssert", true);
#if _TARGET_PC_WIN
  MEMORYSTATUSEX msx;
  msx.dwLength = sizeof(msx);
  if (GlobalMemoryStatusEx(&msx))
  {
    NAU_LOG_DEBUG("avail virt={}M phys={}M", msx.ullAvailVirtual >> 20, msx.ullAvailPhys >> 20);
    int64_t max_phys_mb = msx.ullAvailPhys >> 20;
    int64_t max_virt_mb = msx.ullAvailVirtual >> 20;
    if (max_phys_mb > 144)
      max_phys_mb -= 144;
    if (max_virt_mb > 304)
      max_virt_mb -= 304;
    if (sys_mem_usage_thres_mb > max_virt_mb)
      sys_mem_usage_thres_mb = (int)max_virt_mb;
    if (sys_mem_usage_thres_mb > max_phys_mb)
      sys_mem_usage_thres_mb = (int)max_phys_mb;
    NAU_LOG_DEBUG("maxVirt={}M maxPhys={}M -> Vectormath::max={}M", max_virt_mb, max_phys_mb, sys_mem_usage_thres_mb);
    int allow_up_sysmem_to_physmem_avail_percent = READ_PROP(Int, "useSysMemUptoPhysMemPercent", 60);
    int allowed_max_mem = int(Vectormath::min(max_phys_mb, max_virt_mb) * allow_up_sysmem_to_physmem_avail_percent / 100);
    if (sys_mem_usage_thres_mb && sys_mem_usage_thres_mb < allowed_max_mem)
    {
      sys_mem_usage_thres_mb = allowed_max_mem;
      NAU_LOG_DEBUG("using sysMemThres={}M due to useSysMemUptoPhysMemPercent={}% and phys={}M", sys_mem_usage_thres_mb,
        allow_up_sysmem_to_physmem_avail_percent, Vectormath::min<int>(max_phys_mb, max_virt_mb));
    }
  }
#endif

  tql::streaming_enabled = true;
  NAU_LOG_DEBUG("texture streaming enabled: reload_jobmgr={}, mem_quota_reserve_kb={}M (sysMemThres={}M +{}M)", reload_jobmgr_id,
    mem_quota_reserve_kb >> 10, sys_mem_usage_thres_mb, sys_mem_add_free_mb);

  //if (sys_mem_usage_thres_mb)
  //  dgs_on_out_of_memory = texql_try_unload_tex_on_oom;

  if (const char *tq = b.getStr("defaultTexQ", NULL))
  {
    if (strcmp(tq, "ULQ") == 0)
      texmgr_internal::dbg_texq_only_stubs = true;
    else if (strcmp(tq, "LQ") != 0 && strcmp(tq, "FQ") != 0 && strcmp(tq, "HQ") != 0)
      NAU_LOG_ERROR("unrecognized defaultTexQ:t={}", tq);
  }
  texmgr_internal::texq_load_on_demand = b.getBool("texLoadOnDemand", texmgr_internal::texq_load_on_demand);
  if (reload_jobmgr_id < 0)
    texmgr_internal::texq_load_on_demand = false;
#if _TARGET_D3D_MULTI
  if (strcmp(::dgs_get_settings()->getBlockByNameEx("video")->getStr("driver", "auto"), "stub") == 0)
#else
  if (d3d::is_stub_driver())
#endif
  {
    texmgr_internal::texq_load_on_demand = false;
    mgr_log_level = 0;
    NAU_LOG_DEBUG("tex streaming is restricted under STUB driver!");
  }

  if (texmgr_internal::texq_load_on_demand)
    NAU_LOG_DEBUG("will load tex on demand!");
  texmgr_internal::dbg_texq_load_sleep_ms = b.getInt("dbgBkgLoadPerTexSleepMs", 0);
  if (texmgr_internal::dbg_texq_load_sleep_ms > 0)
    NAU_LOG_DEBUG("will use {} ms wait before each tex loaded in reload_jobmg", texmgr_internal::dbg_texq_load_sleep_ms);

  if (reload_jobmgr_id >= 0 && texmgr_internal::texq_load_on_demand)
    tql::on_frame_finished = &on_frame_finished;
#undef READ_PROP
}

bool is_managed_textures_streaming_active() { return tql::streaming_enabled; }
bool is_managed_textures_streaming_load_on_demand() { return tql::streaming_enabled && texmgr_internal::texq_load_on_demand; }

bool texmgr_internal::D3dResMgrDataFinal::downgradeTexQuality(int idx, BaseTexture &this_tex, int req_lev)
{
  unsigned ld_lev = resQS[idx].getLdLev();
  if (ld_lev <= 1 || (ld_lev <= req_lev && getTexAddMemSizeNeeded4K(idx) != 0)) //==
    return false;

  if (texDesc[idx].dim.stubIdx >= 0 && req_lev == 1 && getTexMemSize4K(idx) > 1 && RMGR.startReading(idx, 1))
  {
    if (incRefCount(idx) == 1)
      RMGR.decReadyForDiscardTex(idx);
    this_tex.discardTex();
    RMGR.changeTexUsedMem(idx, tql::sizeInKb(this_tex.ressize()), (getTexMemSize4K(idx) + getTexAddMemSizeNeeded4K(idx)) * 4);
    resQS[idx].setCurQL(TQL_stub);
    resQS[idx].setLdLev(1);
    resQS[idx].setMaxReqLev(1);
    if (decRefCount(idx) == 0)
      RMGR.incReadyForDiscardTex(idx);
    return true;
  }

  if (req_lev > 1 && RMGR.startReading(idx, req_lev))
  {
    if (incRefCount(idx) == 1)
      RMGR.decReadyForDiscardTex(idx);
    unsigned skip = (texDesc[idx].dim.maxLev - req_lev);
    unsigned w = Vectormath::max(texDesc[idx].dim.w >> skip, 1), h = Vectormath::max(texDesc[idx].dim.h >> skip, 1);
    unsigned d = Vectormath::max(this_tex.restype() == RES3D_VOLTEX ? texDesc[idx].dim.d >> skip : texDesc[idx].dim.d, 1);
    unsigned l = texDesc[idx].dim.l - skip;
    tql::resizeTexture(&this_tex, w, h, d, l, ld_lev);
    RMGR.changeTexUsedMem(idx, tql::sizeInKb(this_tex.ressize()), (getTexMemSize4K(idx) + getTexAddMemSizeNeeded4K(idx)) * 4);

    resQS[idx].setCurQL(req_lev < resQS[idx].getQLev() ? calcCurQL(idx, req_lev) : resQS[idx].getMaxQL());
    resQS[idx].setLdLev(req_lev);
    resQS[idx].setMaxReqLev(req_lev);
    if (decRefCount(idx) == 0)
      RMGR.incReadyForDiscardTex(idx);

    RMGR_DUMP_TEX_STATE(idx);
    return true;
  }
  return false;
}

// texture prefetch
bool prefetch_managed_textures(eastl::span<const TEXTUREID> tid)
{
  using namespace texmgr_internal;

  int ready = 0;
  for (int i = 0; i < tid.size(); i++)
  {
    int idx = RMGR.toIndex(tid[i]);
    if (idx < 0)
      continue;

    int rc = RMGR.getRefCount(idx);
    if (rc < 0)
      continue;
    if (!rc || (RMGR.baseTexture(idx) && RMGR.isTexLoaded(idx, false)))
      ready++;
    if (RMGR.texDesc[idx].dim.stubIdx >= 0)
      RMGR.markResLFU(tid[i]);
  }
  return ready == tid.size();
}
bool prefetch_managed_texture(TEXTUREID tid) { return prefetch_managed_textures(eastl::span<const TEXTUREID>(&tid, 1)); }
bool prefetch_managed_textures_by_textag(int textag)
{
  using namespace texmgr_internal;

  NAU_ASSERT(textag >= 0 && textag < TEXTAG__COUNT);
  int changed = 0, not_ready = 0, m = 1 << textag;

  // NAU_LOG_DEBUG("prefetch_managed_textures_by_textag({})", textag);
  for (unsigned idx = 0, ie = RMGR.getRelaxedIndexCount(); idx < ie; idx++)
  {
    int rc = RMGR.getRefCount(idx);
    if (rc < 0 || !(RMGR.getTagMask(idx) & m))
      continue;
    if (rc && (!RMGR.baseTexture(idx) || !RMGR.isTexLoaded(idx, false)))
      not_ready++;
    if (RMGR.texDesc[idx].dim.stubIdx >= 0)
    {
      RMGR.markResLFU(RMGR.toId(idx));
      changed++;
    }
  }
  NAU_LOG_DEBUG("prefetch_managed_textures_by_textag({}): textures to load {}, not_ready={}", textag, changed, not_ready);
  G_UNUSED(changed);
  return not_ready == 0;
}

bool prefetch_and_check_managed_textures_loaded(eastl::span<const TEXTUREID> tex_list, bool fq_loaded)
{
  if (check_all_managed_textures_loaded(tex_list, fq_loaded))
    return true;
  bool loaded = is_managed_textures_streaming_load_on_demand() ? prefetch_managed_textures(tex_list) : false;
  for (TEXTUREID id : tex_list)
    mark_managed_tex_lfu(id);
  return (loaded && fq_loaded) ? check_all_managed_textures_loaded(tex_list, fq_loaded) : loaded;
}


void prefetch_and_wait_managed_textures_loaded(eastl::span<const TEXTUREID> tex_list, bool fq_loaded)
{
  int64_t reft = ref_time_ticks();
  unsigned f = dagor_frame_no();
#if DAGOR_THREAD_SANITIZER
  int timeout_usec = is_main_thread() ? 15000000 /*15 sec*/ : 40000000 /*40 sec*/; // TSAN takes longer
#else
  int timeout_usec = nau::hal::is_main_thread() ? 5000000 /*5 sec*/ : 30000000 /*30 sec*/;
#endif
  while (!prefetch_and_check_managed_textures_loaded(tex_list, fq_loaded))
  {
    if (!texmgr_internal::texq_load_on_demand)
    {
      NAU_LOG_WARNING("{} called with texq_load_on_demand=0", __FUNCTION__);
      //ddsx::tex_pack2_perform_delayed_data_loading(); // TODO: fix it if needed
    }
    on_frame_finished();
    // process pending resource updates to avoid stalling here when waiting in main thread
    if (d3d::driver_command(DRV3D_COMMAND_PROCESS_PENDING_RESOURCE_UPDATED, nullptr, nullptr, nullptr))
      interlocked_increment(texmgr_internal::drv_res_updates_flush_count);
    nau::hal::sleep_msec(10);
    if (dagor_frame_no() > f + 6)
    {
      unsigned ready = 0, bad = 0;
      for (TEXTUREID tid : tex_list)
      {
        int idx = RMGR.toIndex(tid);
        if (idx < 0)
        {
          bad++;
          continue;
        }
        if (check_managed_texture_loaded(tid, fq_loaded))
          ready++;
        else if (RMGR.resQS[idx].getRdLev() == 0)
        {
          RMGR.dumpTexState(idx);
          bad++;
        }
      }

      if (tex_list.size() == ready)
        return;
      if (tex_list.size() == ready + bad)
      {
        NAU_LOG_DEBUG("{}: finish waiting: {} ready, {} bad; waited {} msec ({} frames)", __FUNCTION__, ready, bad, get_time_usec(reft) / 1000,
          dagor_frame_no() - f);
        for (TEXTUREID tid : tex_list)
          if (RMGR.isValidID(tid, nullptr) && !check_managed_texture_loaded(tid, fq_loaded) && RMGR.resQS[tid.index()].getRdLev() == 0)
            RMGR.dumpTexState(tid.index());
        return;
      }
    }
    if (get_time_usec(reft) > timeout_usec)
    {
      for (TEXTUREID tid : tex_list)
      {
        int idx = RMGR.toIndex(tid);
        if (idx < 0)
          continue;
        bool loaded = check_managed_texture_loaded(tid, fq_loaded);
        bool loaded_bq = !loaded && fq_loaded ? check_managed_texture_loaded(tid, false) : loaded;
        int rc = RMGR.getRefCount(idx);
        int lfu = RMGR.getLFU(idx);
        //NAU_LOG_DEBUG("tex({}) id={:#x} refc={} loaded={}{} ldLev={} rdLev={} maxLev={} (maxReqLev={} lfu={}, curQL={} stubIdx={})",
        //  RMGR.getName(idx), tid, rc, loaded, !loaded && fq_loaded ? (loaded_bq ? " BQ loaded" : " BQ missing") : "",
        //  RMGR.resQS[idx].getLdLev(), RMGR.resQS[idx].getRdLev(), RMGR.resQS[idx].getMaxLev(), RMGR.resQS[idx].getMaxReqLev(), lfu,
        //  RMGR.resQS[idx].getCurQL(), RMGR.texDesc[idx].dim.stubIdx);
        if (!loaded)
          RMGR_DUMP_TEX_STATE(idx);
        G_UNUSED(lfu);
        G_UNUSED(rc);
        G_UNUSED(loaded_bq);
      }
      NAU_LOG_ERROR("{}: timeout={} usec (for {} thread) passed, waiting for {} tex, {}, frame={}", __FUNCTION__, timeout_usec,
                    nau::hal::is_main_thread() ? "main" : "back", tex_list.size(), fq_loaded ? "FQ" : "BQ", dagor_frame_no());
      break;
    }
  }
}

#if DAGOR_DBGLEVEL > 0
#include <util/dag_console.h>
PULL_CONSOLE_PROC(texmgr_dbgcontrol_console_handler)
#endif
