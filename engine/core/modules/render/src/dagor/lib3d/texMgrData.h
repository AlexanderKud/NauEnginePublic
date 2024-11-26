// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#ifndef __DAGOR_TEX_MGR_DATA_H
#define __DAGOR_TEX_MGR_DATA_H
#pragma once

#include "nau/3d/dag_texMgr.h"
#include "nau/3d/dag_texMgrTags.h"
#include "nau/3d/dag_tex3d.h"
#include "nau/3d/dag_drv3dCmd.h"
#include "nau/threading/critical_section.h"
#include "nau/threading/dag_atomic.h"
#include "nau/util/dag_fastStrMap.h"
#include "nau/3d/dag_texIdSet.h"
#include "nau/3d/ddsxTex.h"
#include "nau/3d/tql.h"
#include <EASTL/vector.h>
#include "nau/startup/dag_globalSettings.h"
#include "nau/diag/logging.h"

namespace ddsx
{
struct Header;
}

namespace texmgr_internal
{
struct D3dResMgrDataFinal;

struct TexTagInfoArray : public eastl::array<TexTagInfo, TEXTAG__COUNT>
{
    TexTagInfoArray()
    {
        mem_set_0(*this);
    }
};

struct MipBiasRule
{
  const char *substring = nullptr;
  float bias = 0.0f;
};

extern D3dResMgrDataFinal RMGR; //< main resource manager data

extern eastl::vector<MipBiasRule> mip_bias_rules;
extern float default_mip_bias;

extern TextureIdSet anisotropy_exceptions;

extern TextureFactory *default_tex_mgr_factory;
extern TexTagInfoArray textagInfo;
extern int mt_enabled;
extern dag::CritSecStorage crit_sec;
extern dag::CriticalSection rec_lock; // Intentionally not spinlock, since waiting times might be quite long (>1ms) e.g. within
                            // unload_tex_to_reserve
extern int (*drv3d_cmd)(int command, void *par1, void *par2, void *par3);
extern bool auto_add_tex_on_get_id;
extern bool disable_add_warning;
extern int def_texq;
extern bool texq_load_on_demand;
extern uint8_t texq_load_disabled;
extern int reload_jobmgr_id;
extern int dbg_texq_load_sleep_ms;
extern bool dbg_texq_only_stubs;
extern volatile int drv_res_updates_flush_count;
extern NauFastStrMapT<int, -1> managed_tex_map_by_name; // helper container for fast resolve by name
extern eastl::vector<const char *> managed_tex_map_by_idx;     // helper container for fast resolve by index
extern bool enable_cur_ql_mismatch_assert;

extern bool (*should_release_tex)(BaseTexture *b);
extern void (*stop_bkg_tex_loading)(int sleep_quant); //< =NULL by default
extern void (*hook_on_get_texture_id)(TEXTUREID id);

extern bool (*d3d_load_genmip_sysmemcopy)(TEXTUREID tid, TEXTUREID base_tid, const ddsx::Header &h, nau::iosys::IGenLoad &r, int q_id);
} // namespace texmgr_internal

namespace tql
{
extern dag::CriticalSection reqLDCritSection;
extern eastl::vector_set<TEXTUREID> reqLD;

extern int gpu_mem_reserve_kb;
extern int mem_used_persistent_kb;
extern int mem_quota_reserve_kb;
extern int sys_mem_usage_thres_mb, sys_mem_add_free_mb;
extern int dyn_qlev_decrease;

inline unsigned get_tex_type(const ddsx::Header &h)
{
  if (h.flags & h.FLG_CUBTEX)
    return RES3D_CUBETEX;
  else if (h.flags & h.FLG_VOLTEX)
    return RES3D_VOLTEX;
  else if (h.flags & h.FLG_ARRTEX)
    return RES3D_ARRTEX;
  return RES3D_TEX;
}
} // namespace tql

struct texmgr_internal::D3dResMgrDataFinal : public D3dResManagerData
{
  using D3dResManagerData::INVALID_REFCOUNT;
  enum : unsigned
  {
    RCBIT_FOR_REMOVE = 0x20000000u
  }; // special bit embedded to refCount to mark entry scheduled for removal
  struct TexDesc
  {
    struct PackRecIdx
    {
      int16_t pack;
      uint16_t rec;
    } packRecIdx[TQL__COUNT];
    struct Dim
    {
      uint16_t w, h, d;
      uint8_t l : 4, maxLev : 4;
      uint16_t bqGenmip : 1;
      int8_t stubIdx : 7;
    } dim;

    void init()
    {
      memset(packRecIdx, 0xFF, sizeof(packRecIdx));
      memset(&dim, 0, sizeof(dim));
      dim.stubIdx = -1;
    }
    void term()
    {
      memset(packRecIdx, 0xFF, sizeof(packRecIdx));
      memset(&dim, 0, sizeof(dim));
      dim.stubIdx = -1;
    }
    unsigned getMinLev() const
    {
      unsigned minl = dim.maxLev + 1u - dim.l;
      return dim.maxLev >= 2 && minl <= 2 ? 2 : minl;
    }
  };
  struct TexUsedSz
  {
    uint16_t memSize4K, addMemSizeNeeded4K;
  };
  struct BaseData
  {
    unsigned sz;
    ddsx::Header hdr;
    uint8_t data[4]; // variable size data

    static BaseData *make(const ddsx::Header &h, const void *d)
    {
      unsigned size = sizeof(BaseData) + h.getSurfaceSz(0) - sizeof(BaseData::data);
      BaseData *bd = (BaseData *)memalloc(size, midmem);
      bd->sz = size;
      bd->hdr = h;
      bd->hdr.flags &= ~h.FLG_REV_MIP_ORDER;
      bd->hdr.levels = 1;
      memcpy(bd->data, d, h.getSurfaceSz(0));
      return bd;
    }
    static void free(BaseData *bd) { memfree(bd, midmem); }
  };

public:
  void init(int max_res_entry_count);
  void term();

  // entry allocation/deallocation
  inline unsigned getPredPosWrapped(unsigned pos) const { return pos > 0 ? (pos - 1) & FREE_LIST_BMASK : FREE_LIST_BMASK; }
  int allocEntry()
  {
    unsigned wr_pos = interlocked_relaxed_load(freeListWrPos);
    unsigned rd_pos = interlocked_acquire_load(freeListRdPos);
    if (wr_pos != rd_pos && (wr_pos > rd_pos + REUSE_LAG || (wr_pos < rd_pos && wr_pos + FREE_LIST_BMASK + 1 > rd_pos + REUSE_LAG)))
    {
      rd_pos = getPredPosWrapped(interlocked_increment(freeListRdPos));
      return freeListIdx[rd_pos];
    }
    int idx = interlocked_increment(indexCount) - 1;
    return idx < maxTotalIndexCount ? idx : onEntryOverflow(idx);
  }
  void releaseEntry(unsigned idx)
  {
    if (idx > FREE_LIST_BMASK)
      return onFreeListOverflow(idx);
    unsigned wr_pos = getPredPosWrapped(interlocked_increment(freeListWrPos));
    freeListIdx[wr_pos] = (free_index_t)idx;
    interlocked_add(generation[idx], D3DRESID::GENERATION_BITS_STEP);
    interlocked_and(generation[idx], D3DRESID::GENERATION_MASK);
  }

  // D3DRESID <-> index conversions
  static D3DRESID toId(int idx) { return idx >= 0 ? D3DRESID::make(idx, getCurGeneration(idx)) : BAD_D3DRESID; }
#if DAGOR_DBGLEVEL > 0
  static int toIndex(D3DRESID tid) { return isValidID(tid) ? tid.index() : -1; }
#else
  static int toIndex(D3DRESID tid) { return isValidID(tid, nullptr) ? tid.index() : -1; }
#endif

  // clear newly registered entry
  static void initAllocatedRec(int idx, const char *nm, TextureFactory *f)
  {
    setD3dRes(idx, nullptr);
    setTagMask(idx, 0);
    setFactory(idx, nullptr);
    setName(idx, nm, f);
    interlocked_release_store(lastFrameUsed[idx], 0);
    resQS[idx].setMaxReqLev(1);
    resQS[idx].setLdLev(1);
    resQS[idx].setQLev(1);
    resQS[idx].setMaxQL(TQL_base, TQL_base);
    texUsedSz[idx].memSize4K = texUsedSz[idx].addMemSizeNeeded4K = 0;
    levDesc[idx] = 0x0010;
    texDesc[idx].init();
    interlocked_release_store(texImportance[idx], 0);
    interlocked_release_store(refCount[idx], 0);
  }
  // cleanup unregistered entry
  static void clearReleasedRec(int idx)
  {
    interlocked_release_store(refCount[idx], INVALID_REFCOUNT);
    setD3dRes(idx, nullptr);
    setTagMask(idx, 0);
    TextureFactory *f = getFactory(idx);
    setFactory(idx, nullptr);
    setName(idx, nullptr, f);
    interlocked_release_store(lastFrameUsed[idx], 0);
    resQS[idx].setMaxReqLev(0);
    resQS[idx].setLdLev(0);
    resQS[idx].setQLev(0);
    resQS[idx].setMaxQL(TQL_stub, TQL_stub);
    texUsedSz[idx].memSize4K = texUsedSz[idx].addMemSizeNeeded4K = 0;
    levDesc[idx] = 0x0000;
    texDesc[idx].term();
    interlocked_release_store(texImportance[idx], 0);
  }

  // entries count gettors
  static unsigned getRelaxedIndexCount() { return unsigned(interlocked_relaxed_load(indexCount)); }
  static unsigned getAccurateIndexCount() { return unsigned(interlocked_acquire_load(indexCount)); }
  static unsigned getMaxTotalIndexCount() { return unsigned(interlocked_relaxed_load(maxTotalIndexCount)); }

  // main refcount management
  using D3dResManagerData::getRefCount;
  static int getRefCount(int idx) { return interlocked_acquire_load(refCount[idx]); }
  static int incRefCount(int idx) { return interlocked_increment(refCount[idx]); }
  static int decRefCount(int idx) { return interlocked_decrement(refCount[idx]); }
  static void scheduleForRemoval(int idx) { interlocked_or(refCount[idx], RCBIT_FOR_REMOVE); }
  static bool isScheduledForRemoval(int idx) { return interlocked_acquire_load(refCount[idx]) & RCBIT_FOR_REMOVE; }
  static bool isAlive(int idx)
  {
    int rc = getRefCount(idx);
    return rc >= 0 && !(rc & RCBIT_FOR_REMOVE);
  }

  using D3dResManagerData::getResLFU;
  static __forceinline unsigned getResLFU(int idx) { return (unsigned)interlocked_acquire_load(lastFrameUsed[idx]); }

  // D3dRes management
  using D3dResManagerData::getD3dRes;
  static D3dResource *getD3dRes(int idx) { return interlocked_acquire_load_ptr(asVolatile(d3dRes[idx])); }
  static D3dResource *getD3dResRelaxed(int idx) { return interlocked_relaxed_load_ptr(asVolatile(d3dRes[idx])); }
  static void setD3dRes(int idx, D3dResource *r) { interlocked_release_store_ptr(asVolatile(d3dRes[idx]), r); }
  static D3dResource *exchangeD3dRes(int idx, D3dResource *rx, D3dResource *rc)
  {
    return interlocked_compare_exchange_ptr(asVolatile(d3dRes[idx]), rx, rc);
  }
  static BaseTexture *baseTexture(int idx)
  {
    D3dResource *r = getD3dRes(idx);
    int t = r ? r->restype() : 0;
    if (t == RES3D_TEX || t == RES3D_CUBETEX || t == RES3D_VOLTEX || t == RES3D_ARRTEX || t == RES3D_CUBEARRTEX)
      return static_cast<BaseTexture *>(r);
    return nullptr;
  }

  // TEXTAG mask management
  static unsigned getTagMask(int idx) { return (unsigned)interlocked_relaxed_load((int &)tagMask[idx]); }
  static void updTagMaskOr(int idx, unsigned or_m) { interlocked_or((int &)tagMask[idx], or_m); }
  static void updTagMaskAnd(int idx, unsigned and_m) { interlocked_and((int &)tagMask[idx], and_m); }
  static void setTagMask(int idx, unsigned m) { interlocked_release_store((int &)tagMask[idx], m); }

  // paired-base-tex refcount management
  static unsigned getBaseTexUsedCount(unsigned idx) { return interlocked_acquire_load(btUsageRefCount[idx]); }
  static int getBaseTexRc(TEXTUREID id)
  {
    int idx = toIndex(id);
    return (idx >= 0) ? getBaseTexUsedCount(idx) : 0;
  }
  void incBaseTexRc(TEXTUREID id)
  {
    int idx = toIndex(id);
    if (idx >= 0)
      if (interlocked_increment(btUsageRefCount[idx]) == 1)
        if (unsigned sz = getTexBaseDataSize(idx))
          decReadyForDiscardBd(tql::sizeInKb(sz));
  }
  void decBaseTexRc(TEXTUREID id)
  {
    int idx = toIndex(id);
    if (idx >= 0)
      if (interlocked_decrement(btUsageRefCount[idx]) == 0)
        if (unsigned sz = getTexBaseDataSize(idx))
          incReadyForDiscardBd(tql::sizeInKb(sz));
  }
  static bool mayNeedBaseTex(int idx, unsigned lev) { return lev > getLevDesc(idx, TexQL(TQL_base - texDesc[idx].dim.bqGenmip)); }

  // factory management
  static TextureFactory *getFactory(unsigned idx) { return interlocked_acquire_load_ptr(asVolatile(factory[idx])); }
  static void setFactory(unsigned idx, TextureFactory *f) { return interlocked_release_store_ptr(asVolatile(factory[idx]), f); }

  // name management
  static const char *getName(unsigned idx) { return namePtr[idx]; }
  static void setName(unsigned idx, const char *nm, TextureFactory *f);

  // LFU gettor
  static __forceinline unsigned getLFU(int idx) { return (unsigned)interlocked_acquire_load(lastFrameUsed[idx]); }

  // levDesc and curQL/maxQL calculations
  static void setLevDesc(int idx, unsigned ql, unsigned lev)
  {
    levDesc[idx] = (levDesc[idx] & ~(0xF << (ql * 4))) | ((lev & 0xF) << (ql * 4));
  }
  static TexQL calcCurQL(int idx, unsigned ldLev)
  {
    if (ldLev == resQS[idx].getQLev())
      return resQS[idx].getMaxQL();
    for (TexQL ql = TQL__LAST; ql > TQL__FIRST; ql = TexQL(ql - 1))
      if (getLevDesc(idx, ql) && ldLev >= getLevDesc(idx, ql))
        return ql;
    return getLevDesc(idx, TQL__FIRST) && ldLev >= getLevDesc(idx, TQL__FIRST) ? TQL__FIRST : TQL_stub;
  }
  static TexQL calcMaxQL(int idx)
  {
    for (TexQL ql = TQL__LAST; ql > TQL__FIRST; ql = TexQL(ql - 1))
      if (getLevDesc(idx, ql))
        return ql;
    return getLevDesc(idx, TQL__FIRST) ? TQL__FIRST : TQL_stub;
  }
  static bool isTexLoaded(int idx, bool fq_loaded)
  {
    unsigned lev = fq_loaded ? getLevDesc(idx, TQL_high) : 0;
    if (!lev)
      lev = getLevDesc(idx, TQL_base);
    return resQS[idx].getLdLev() >= resQS[idx].clampLev(lev) ||
           (!getTexImportance(idx) && resQS[idx].getLdLev() >= resQS[idx].getMaxLev());
  }
  static void setMaxLev(int idx, unsigned l) { resQS[idx].setMaxLev(Vectormath::max(l, texDesc[idx].getMinLev())); }
  static void changeTexMaxLev(int idx, int lev)
  {
    unsigned ld_lev = resQS[idx].getLdLev();
    setMaxLev(idx, lev);
    resQS[idx].setCurQL(ld_lev < resQS[idx].getQLev() ? calcCurQL(idx, ld_lev) : resQS[idx].getMaxQL());
  }

  // init resQS (texture quality state)
  static bool initResQS(int idx, TexQL req_ql)
  {
    resQS[idx].setLdLev(1);
    resQS[idx].setCurQL(TQL_stub);
    if (req_ql != TQL_stub)
    {
      unsigned lev = getLevDesc(idx, Vectormath::min(req_ql, resQS[idx].getMaxQL()));
      resQS[idx].setMaxReqLev(lev ? lev : resQS[idx].getQLev());
    }
    return texDesc[idx].dim.stubIdx >= 0;
  }

  // texture loads management
  static bool scheduleReading(int idx, TextureFactory *f);
  static bool readDdsxTex(TEXTUREID tid, const ddsx::Header &hdr, nau::iosys::IGenLoad &crd, int quality_id);
  static void finishReading(int idx);
  static void cancelReading(int idx);
  static bool startReading(int idx, unsigned rd_lev)
  {
    uint8_t ld_rd = interlocked_acquire_load(resQS[idx].ldLev__rdLev);
    if (ld_rd & 0xF)
      return false;
    return interlocked_compare_exchange(resQS[idx].ldLev__rdLev, (ld_rd & 0xF0) | (rd_lev & 0xF), ld_rd) == ld_rd;
  }

  //! downgrades texture to specified level; returns true when downgrade done
  static bool downgradeTexQuality(int idx, BaseTexture &this_tex, int req_lev);
  //! calculate GPU memory size for texture for specified target level
  static unsigned calcTexMemSize(int idx, int target_lev, const ddsx::Header &hdr);

  static void markUpdated(int idx, unsigned new_ld_lev);
  static void markUpdatedAfterLoad(int idx) { return markUpdated(idx, resQS[idx].getRdLev()); }

  // base-data management (for textures with ddsx::Header::FLG_HOLD_SYSMEM_COPY flag)
  BaseData *replaceTexBaseData(int idx, BaseData *bd, bool delete_old = true)
  {
    auto dest = (BaseData *volatile *)&texBaseData[idx];
    if (bd)
      incBdTotal(tql::sizeInKb(bd->sz));
    for (;;)
    {
      auto *old_bd = *dest;
      if (interlocked_compare_exchange_ptr(*dest, bd, old_bd) == old_bd)
      {
        if (getBaseTexUsedCount(idx) == 0)
        {
          if (bd)
            RMGR.incReadyForDiscardBd(tql::sizeInKb(bd->sz));
          if (old_bd && delete_old)
            RMGR.decReadyForDiscardBd(tql::sizeInKb(old_bd->sz));
        }
        if (old_bd)
        {
          decBdTotal(tql::sizeInKb(old_bd->sz));
          if (delete_old)
            BaseData::free(old_bd);
        }
        return delete_old ? nullptr : old_bd;
      }
    }
    return nullptr; // unreachable
  }
  static bool hasTexBaseData(int idx) { return interlocked_acquire_load_ptr(asVolatile(texBaseData[idx])) != nullptr; }
  static bool hasTexBaseData(TEXTUREID tid) { return tid != BAD_TEXTUREID ? hasTexBaseData(toIndex(tid)) : false; }
  static unsigned getTexBaseDataSize(int idx)
  {
    if (BaseData *bd = interlocked_acquire_load_ptr(asVolatile(texBaseData[idx])))
      return bd->sz;
    return 0;
  }
  static unsigned getTexBaseDataSize(TEXTUREID tid) { return tid != BAD_TEXTUREID ? getTexBaseDataSize(toIndex(tid)) : 0; }
  static BaseData *getTexBaseData(TEXTUREID tid)
  {
    return tid != BAD_TEXTUREID ? interlocked_acquire_load_ptr(asVolatile(texBaseData[toIndex(tid)])) : nullptr;
  }

  // texture importance
  static uint16_t getTexImportance(int idx) { return interlocked_relaxed_load(texImportance[idx]); }

  using D3dResManagerData::updateResReqLev;

  // current GPU mem usage counters management
  void incReadyForDiscardTex(int idx)
  {
    int tex_sz_kb = getTexMemSize4K(idx) * 4;
    if (!tex_sz_kb)
      return;
    interlocked_add(readyForDiscardTexSzKB, tex_sz_kb);
    interlocked_increment(readyForDiscardTexCount);
    interlocked_add(totalAddMemNeededSzKB, -int(getTexAddMemSizeNeeded4K(idx)) * 4);
    NAU_ASSERT(interlocked_relaxed_load(readyForDiscardTexSzKB) >= 0 && interlocked_relaxed_load(readyForDiscardTexCount) >= 0 &&
                interlocked_relaxed_load(totalAddMemNeededSzKB) >= 0,
      "readyForDiscardTexSzKB=%d readyForDiscardTexCount=%d totalAddMemNeededSzKB=%d tex_sz_kb=%d add_sz_kb=%d %s",
      readyForDiscardTexSzKB, readyForDiscardTexCount, totalAddMemNeededSzKB, tex_sz_kb, getTexAddMemSizeNeeded4K(idx) * 4,
      getName(idx));
  }
  void decReadyForDiscardTex(int idx)
  {
    int tex_sz_kb = getTexMemSize4K(idx) * 4;
    if (!tex_sz_kb)
      return;
    interlocked_add(readyForDiscardTexSzKB, -tex_sz_kb);
    interlocked_decrement(readyForDiscardTexCount);
    interlocked_add(totalAddMemNeededSzKB, getTexAddMemSizeNeeded4K(idx) * 4);
    NAU_ASSERT(interlocked_relaxed_load(readyForDiscardTexSzKB) >= 0 && interlocked_relaxed_load(readyForDiscardTexCount) >= 0 &&
                interlocked_relaxed_load(totalAddMemNeededSzKB) >= 0,
      "readyForDiscardTexSzKB=%d readyForDiscardTexCount=%d totalAddMemNeededSzKB=%d tex_sz_kb=%d add_sz_kb=%d %s",
      readyForDiscardTexSzKB, readyForDiscardTexCount, totalAddMemNeededSzKB, tex_sz_kb, getTexAddMemSizeNeeded4K(idx) * 4,
      getName(idx));
  }

  int getTotalUsedTexCount() const { return interlocked_acquire_load(totalUsedTexCount); }
  int getTotalUsedTexSzKB() const { return interlocked_acquire_load(totalUsedTexSzKB); }
  int getTotalAddMemNeededSzKB() const { return interlocked_acquire_load(totalAddMemNeededSzKB); }
  int getReadyForDiscardTexCount() const { return interlocked_acquire_load(readyForDiscardTexCount); }
  int getReadyForDiscardTexSzKB() const { return interlocked_acquire_load(readyForDiscardTexSzKB); }

  // current system mem usage counters management (for base-data)
  void incReadyForDiscardBd(int sz_kb)
  {
    interlocked_add(readyForDiscardBdSzKB, sz_kb);
    interlocked_increment(readyForDiscardBdCount);
    NAU_ASSERT(interlocked_relaxed_load(readyForDiscardBdSzKB) >= 0 && interlocked_relaxed_load(readyForDiscardBdCount) >= 0,
      "readyForDiscardBdSzKB=%d readyForDiscardBdCount=%d", readyForDiscardBdSzKB, readyForDiscardBdCount);
  }
  void decReadyForDiscardBd(int sz_kb)
  {
    interlocked_add(readyForDiscardBdSzKB, -sz_kb);
    interlocked_decrement(readyForDiscardBdCount);
    NAU_ASSERT(interlocked_relaxed_load(readyForDiscardBdSzKB) >= 0 && interlocked_relaxed_load(readyForDiscardBdCount) >= 0,
      "readyForDiscardBdSzKB=%d readyForDiscardBdCount=%d", readyForDiscardBdSzKB, readyForDiscardBdCount);
  }
  void incBdTotal(int sz_kb)
  {
    interlocked_add(totalBdSzKB, sz_kb);
    interlocked_increment(totalBdCount);
  }
  void decBdTotal(int sz_kb)
  {
    interlocked_add(totalBdSzKB, -sz_kb);
    interlocked_decrement(totalBdCount);
  }

  int getTotalBdSzKB() const { return interlocked_acquire_load(totalBdSzKB); }
  int getTotalBdCount() const { return interlocked_acquire_load(totalBdCount); }
  int getReadyForDiscardBdSzKB() const { return interlocked_acquire_load(readyForDiscardBdSzKB); }
  int getReadyForDiscardBdCount() const { return interlocked_acquire_load(readyForDiscardBdCount); }

  // per-entry GPU mem usage management
  static unsigned getTexMemSize4K(int idx) { return interlocked_acquire_load(texUsedSz[idx].memSize4K); }
  static unsigned getTexAddMemSizeNeeded4K(int idx) { return interlocked_acquire_load(texUsedSz[idx].addMemSizeNeeded4K); }

  void changeTexUsedMem(int idx, int new_sz_kb, int full_needed_sz_kb)
  {
    NAU_ASSERT(new_sz_kb <= full_needed_sz_kb, "new_sz_kb={} full_needed_sz_kb={}", new_sz_kb, full_needed_sz_kb);
    full_needed_sz_kb = new_sz_kb;
    if (int old_used_4k = getTexMemSize4K(idx))
    {
      interlocked_add(totalUsedTexSzKB, new_sz_kb - old_used_4k * 4);
      if (!new_sz_kb)
        interlocked_decrement(totalUsedTexCount);
    }
    else
    {
      if (new_sz_kb)
        interlocked_increment(totalUsedTexCount);
      interlocked_add(totalUsedTexSzKB, new_sz_kb);
    }
    interlocked_add(totalAddMemNeededSzKB, (full_needed_sz_kb - new_sz_kb) - int(getTexAddMemSizeNeeded4K(idx)) * 4);
    interlocked_release_store(texUsedSz[idx].memSize4K, new_sz_kb / 4);
    interlocked_release_store(texUsedSz[idx].addMemSizeNeeded4K, (full_needed_sz_kb - new_sz_kb) / 4);
    NAU_ASSERT(interlocked_relaxed_load(readyForDiscardTexSzKB) >= 0 && interlocked_relaxed_load(readyForDiscardTexCount) >= 0 &&
                interlocked_relaxed_load(totalAddMemNeededSzKB) >= 0,
      "idx=%d readyForDiscardTexSzKB=%d readyForDiscardTexCount=%d totalAddMemNeededSzKB=%d new_sz_kb=%d full_needed_sz_kb=%d", idx,
      readyForDiscardTexSzKB, readyForDiscardTexCount, totalAddMemNeededSzKB, new_sz_kb, full_needed_sz_kb);
  }

  //! setup qLev for texture based on current global quality settings
  static void setupQLev(int idx, int q_id, const ddsx::Header &hdr)
  {
    resQS[idx].setQLev(Vectormath::max<unsigned>(texDesc[idx].dim.maxLev - hdr.getSkipLevelsFromQ(q_id), texDesc[idx].getMinLev()));
  }

  //! verbose dump of current texture entry state
  static inline int uint16_to_int(uint16_t v) { return v == 0xffffu ? -1 : v; }
  static void dumpTexState(int idx)
  {
    //NAU_LOG_DEBUG("{}: {}x{}x{},L{}, desc=0x{:04X} max={}({}/{}) ld={} rd={} req={}  tm={:#x}  "
    //      "TQ={}:{} BQ={}:{} HQ={}:{} UHQ={}:{} ({}) rc={} bt.rc={} bt.id={} ql={}({}) stubIdx={} gpu={}K(+{}K) bd={}K lfu={}",
    //  idx, texDesc[idx].dim.w, texDesc[idx].dim.h, texDesc[idx].dim.d, texDesc[idx].dim.l, levDesc[idx], texDesc[idx].dim.maxLev,
    //  resQS[idx].getQLev(), resQS[idx].getMaxLev(), resQS[idx].getLdLev(), resQS[idx].getRdLev(), resQS[idx].getMaxReqLev(),
    //  getTagMask(idx), texDesc[idx].packRecIdx[TQL_thumb].pack, uint16_to_int(texDesc[idx].packRecIdx[TQL_thumb].rec),
    //  texDesc[idx].packRecIdx[TQL_base].pack, uint16_to_int(texDesc[idx].packRecIdx[TQL_base].rec),
    //  texDesc[idx].packRecIdx[TQL_high].pack, uint16_to_int(texDesc[idx].packRecIdx[TQL_high].rec),
    //  texDesc[idx].packRecIdx[TQL_uhq].pack, uint16_to_int(texDesc[idx].packRecIdx[TQL_uhq].rec), getName(idx), getRefCount(idx),
    //  getBaseTexUsedCount(idx), pairedBaseTexId[idx] ? pairedBaseTexId[idx].index() : -1, resQS[idx].getCurQL(), resQS[idx].getMaxQL(),
    //  texDesc[idx].dim.stubIdx, getTexMemSize4K(idx) * 4, getTexAddMemSizeNeeded4K(idx) * 4, getTexBaseDataSize(idx) >> 10,
    //  getResLFU(idx));
    //G_UNUSED(idx);
  }
  //! dump of utilized memory summary
  void dumpMemStats()
  {
    NAU_LOG_DEBUG("StrmTex:  {}K in {} tex, and {}K needed (ready to free {}K in {} tex)", interlocked_relaxed_load(totalUsedTexSzKB),
      interlocked_relaxed_load(totalUsedTexCount), interlocked_relaxed_load(totalAddMemNeededSzKB),
      interlocked_relaxed_load(readyForDiscardTexSzKB), interlocked_relaxed_load(readyForDiscardTexCount));
    if (interlocked_relaxed_load(totalBdCount))
      NAU_LOG_DEBUG("BaseData: {}K in {} blocks (ready to free {}K in {} blocks)", interlocked_relaxed_load(totalBdSzKB),
        interlocked_relaxed_load(totalBdCount), interlocked_relaxed_load(readyForDiscardBdSzKB),
        interlocked_relaxed_load(readyForDiscardBdCount));
  }

  //! copies resQS::maxReqLev to maxReqLevelPrev (at the end of frame to have latest/previous maxReqLev value)
  static void copyMaxReqLevToPrev()
  {
    auto *dest = maxReqLevelPrev, *dest_e = dest + getAccurateIndexCount();
    auto *src = resQS;
    for (; dest < dest_e; dest++, src++)
      *dest = src->getMaxReqLev();
  }

protected:
  typedef unsigned free_index_t;
  enum : unsigned
  {
    REUSE_LAG = 512u
  };
  free_index_t *freeListIdx = nullptr;
  free_index_t freeListRdPos = 0, freeListWrPos = 0;
  /*quasi-const*/ free_index_t FREE_LIST_BMASK = 0;

  // GPU mem usage counters and system mem usage counters (for base-data)
  int totalUsedTexCount = 0, totalUsedTexSzKB = 0, totalAddMemNeededSzKB = 0;
  int readyForDiscardTexCount = 0, readyForDiscardTexSzKB = 0;
  int readyForDiscardBdCount = 0, readyForDiscardBdSzKB = 0, totalBdCount = 0, totalBdSzKB = 0;

  // entry allocation/deallocation overflow handlers
  int onEntryOverflow(int idx);
  void onFreeListOverflow(unsigned idx);

public:
  static unsigned *__restrict tagMask;
  static uint16_t *__restrict btUsageRefCount;
  static TextureFactory *__restrict *__restrict factory;
  static char const *__restrict *__restrict namePtr;
  using D3dResManagerData::levDesc;
  using D3dResManagerData::resQS;
  static TexDesc *__restrict texDesc;
  static TEXTUREID *__restrict pairedBaseTexId;
  static BaseData *__restrict *__restrict texBaseData;
  static TexUsedSz *__restrict texUsedSz;
  static uint8_t *__restrict maxReqLevelPrev;
  static uint16_t *__restrict texImportance;
};

namespace texmgr_internal
{
void add_mip_bias_rule(const char *substring, float bias);
void apply_mip_bias_rules(BaseTexture *tex, const char *name);

void remove_from_managed_tex_map(int id);

void acquire_texmgr_lock();
void release_texmgr_lock();
struct TexMgrAutoLock
{
  TexMgrAutoLock() { acquire_texmgr_lock(); };
  ~TexMgrAutoLock() { release_texmgr_lock(); };
  TexMgrAutoLock(const TexMgrAutoLock &) = delete;
  TexMgrAutoLock &operator=(const TexMgrAutoLock &) = delete;
};
void release_all_texmgr_locks();
int find_texture_rec(const char *name, bool auto_add, TextureFactory *f);

void mark_texture_rec(int idx);

//int get_pending_tex_prio(int prio);
//bool should_start_load_prio(int prio);
bool is_gpu_mem_enough_to_load_hq_tex();
bool is_sys_mem_enough_to_load_basedata();

struct TRAL
{
  TRAL() { rec_lock.lock("texMgrRex"); }
  ~TRAL() { rec_lock.unlock(); }
};

bool evict_managed_tex_and_id(TEXTUREID id);
//void register_ddsx_load_implementation();
} // namespace texmgr_internal

#if DAGOR_DBGLEVEL > 0
#define DEV_FATAL DAG_FATAL
#else
#define DEV_FATAL logerr_ctx
#endif

#define TEX_REC_LOCK()      texmgr_internal::rec_lock.lock("texMgrRex")
#define TEX_REC_UNLOCK()    texmgr_internal::rec_lock.unlock()
#define TEX_REC_AUTO_LOCK() TRAL trAL

#endif
