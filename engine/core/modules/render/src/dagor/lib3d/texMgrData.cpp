// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "texMgrData.h"
#include "nau/util/dag_texMetaData.h"
#include "nau/dag_ioSys/dag_genIo.h"
#include "nau/math/dag_adjpow2.h"
#include "nau/osApiWrappers/dag_miscApi.h"
#include "nau/3d/ddsxTex.h"
#include "nau/diag/logging.h"

// The performance overhead of this check is too much to be
// done all the time on resources which are registered/unregistered
// in every frame, or even more than once per frame, like the
// BackBufferHolder
#define CHECK_MANAGED_TEX_MAP 0

#define DECL_RESMGR_PUBLIC_DATA(NM) decltype(D3dResManagerData::NM) D3dResManagerData::NM = nullptr
int D3dResManagerData::indexCount = 0, D3dResManagerData::maxTotalIndexCount = 0;
DECL_RESMGR_PUBLIC_DATA(generation);
DECL_RESMGR_PUBLIC_DATA(refCount);
DECL_RESMGR_PUBLIC_DATA(d3dRes);
DECL_RESMGR_PUBLIC_DATA(lastFrameUsed);
DECL_RESMGR_PUBLIC_DATA(resQS);
DECL_RESMGR_PUBLIC_DATA(levDesc);
#undef DECL_RESMGR_PUBLIC_DATA

#define DECL_RESMGR_PRIVATE_DATA(NM) \
  decltype(texmgr_internal::D3dResMgrDataFinal::NM) texmgr_internal::D3dResMgrDataFinal::NM = nullptr
texmgr_internal::D3dResMgrDataFinal texmgr_internal::RMGR;
DECL_RESMGR_PRIVATE_DATA(tagMask);
DECL_RESMGR_PRIVATE_DATA(btUsageRefCount);
DECL_RESMGR_PRIVATE_DATA(factory);
DECL_RESMGR_PRIVATE_DATA(namePtr);
DECL_RESMGR_PRIVATE_DATA(texDesc);
DECL_RESMGR_PRIVATE_DATA(pairedBaseTexId);
DECL_RESMGR_PRIVATE_DATA(texBaseData);
DECL_RESMGR_PRIVATE_DATA(texUsedSz);
DECL_RESMGR_PRIVATE_DATA(maxReqLevelPrev);
DECL_RESMGR_PRIVATE_DATA(texImportance);
#undef DECL_RESMGR_PRIVATE_DATA
using texmgr_internal::RMGR;

D3DRESID D3DRESID::fromIndex(unsigned index) { return make(index, RMGR.getCurGeneration(index)); }
void D3dResManagerData::require_tex_load(D3DRESID id)
{
  dag::CSAutoLock lock(tql::reqLDCritSection);
  tql::reqLD.insert(id);
}

void texmgr_internal::D3dResMgrDataFinal::init(int max_res_entry_count)
{
  if (max_res_entry_count <= maxTotalIndexCount)
    return;
  NAU_ASSERT_RETURN((!indexCount && !maxTotalIndexCount), , "duplicate init({}) call? indexCount={} maxTotalIndexCount={}",
    max_res_entry_count, indexCount, maxTotalIndexCount);
  maxTotalIndexCount = max_res_entry_count;

#define ALLOC_DATA(NM) NM = (decltype(NM))memalloc(sizeof(*NM) * max_res_entry_count, inimem)
#define ALLOC_DATA_Z(NM) \
  ALLOC_DATA(NM);        \
  memset((void *)NM, 0, sizeof(*NM) * max_res_entry_count)
#define ALLOC_DATA_V(NM, V)                          \
  ALLOC_DATA(NM);                                    \
  for (unsigned i = 0; i < max_res_entry_count; i++) \
  NM[i] = V
  ALLOC_DATA_V(generation, D3DRESID::GENERATION_BITS_START);
  ALLOC_DATA_V(refCount, INVALID_REFCOUNT);
  ALLOC_DATA_Z(d3dRes);
  ALLOC_DATA_Z(lastFrameUsed);
  ALLOC_DATA_Z(resQS);
  ALLOC_DATA_Z(levDesc);

  ALLOC_DATA_Z(tagMask);
  ALLOC_DATA_Z(btUsageRefCount);
  ALLOC_DATA_Z(factory);
  ALLOC_DATA_Z(namePtr);
  ALLOC_DATA_Z(texDesc);
  ALLOC_DATA_V(pairedBaseTexId, BAD_TEXTUREID);
  ALLOC_DATA_Z(texBaseData);
  ALLOC_DATA_Z(texUsedSz);
  ALLOC_DATA_Z(maxReqLevelPrev);
  ALLOC_DATA_Z(texImportance);
#undef ALLOC_DATA
#undef ALLOC_DATA_0
#undef ALLOC_DATA_V

  FREE_LIST_BMASK = nau::math::get_bigger_pow2(max_res_entry_count) - 1;
  unsigned half_shift = sizeof(freeListRdPos) * 4;
  //G_UNUSED(half_shift);
  NAU_ASSERT(FREE_LIST_BMASK > 0 && FREE_LIST_BMASK < (1 << 20) && !((FREE_LIST_BMASK >> half_shift) >> half_shift),
    "bad FREE_LIST_BMASK=0x%x for maxTotalIndexCount=%u and uint%d_t freeListRdPos", FREE_LIST_BMASK, max_res_entry_count,
    half_shift * 2);
  freeListIdx = new free_index_t[FREE_LIST_BMASK + 1];
  memset(freeListIdx, 0xFF, sizeof(free_index_t) * (FREE_LIST_BMASK + 1));
}
void texmgr_internal::D3dResMgrDataFinal::term()
{
  if (!maxTotalIndexCount)
    return;
  // dumpMemStats();
  // debug("maxTotalIndexCount=%d indexCount=%d freeListRdPos=%d freeListWrPos=%d",
  //   maxTotalIndexCount, indexCount, freeListRdPos, freeListWrPos);
  for (unsigned i = 0, ie = RMGR.getRelaxedIndexCount(); i < ie; i++)
  {
    setName(i, nullptr, getFactory(i));
    BaseData::free(texBaseData[i]);
  }

#define RELEASE_DATA(NM)       \
  memfree((void *)NM, inimem); \
  NM = nullptr
  RELEASE_DATA(generation);
  RELEASE_DATA(refCount);
  RELEASE_DATA(d3dRes);
  RELEASE_DATA(lastFrameUsed);
  RELEASE_DATA(resQS);
  RELEASE_DATA(levDesc);

  RELEASE_DATA(tagMask);
  RELEASE_DATA(btUsageRefCount);
  RELEASE_DATA(factory);
  RELEASE_DATA(namePtr);
  RELEASE_DATA(texDesc);
  RELEASE_DATA(pairedBaseTexId);
  RELEASE_DATA(texBaseData);
  RELEASE_DATA(texUsedSz);
  RELEASE_DATA(maxReqLevelPrev);
  RELEASE_DATA(texImportance);
#undef RELEASE_DATA
  maxTotalIndexCount = 0;
  indexCount = 0;

  delete[] freeListIdx;
  freeListIdx = nullptr;
  freeListRdPos = freeListWrPos = 0;
}
int texmgr_internal::D3dResMgrDataFinal::onEntryOverflow(int idx)
{
  if (!maxTotalIndexCount && nau::hal::is_main_thread() && getAccurateIndexCount() == 1)
  {
    interlocked_decrement(indexCount);
    //mt_enabled ? LOGLEVEL_ERR : LOGLEVEL_DEBUG,
    NAU_LOG_DEBUG("enable_res_mgr_mt() not called before first tex register, using implicit init for 64K textures");
    // enable and then disable MT to leave mt_enabled/crit_sec is previous state; just prealloc pools here
    enable_res_mgr_mt(true, 64 << 10);
    enable_res_mgr_mt(false, 64 << 10);
    interlocked_increment(indexCount);
    return idx;
  }

  NAU_ASSERT(idx < maxTotalIndexCount,
    "res entries overflow idx={} maxTotalIndexCount={} indexCount={}, [freeListRdPos={}]={} freeListWrPos={} ", idx,
    maxTotalIndexCount, indexCount, freeListRdPos, freeListIdx[freeListRdPos], freeListWrPos);
  interlocked_decrement(indexCount);
  return -1;
}
void texmgr_internal::D3dResMgrDataFinal::onFreeListOverflow(unsigned idx)
{
  NAU_ASSERT(idx <= FREE_LIST_BMASK, "insufficient freeList or bad index (idx={} used={}/{}  FREE_LIST_SZ={} wrPos={} rdPos={})", idx,
    interlocked_acquire_load(indexCount), maxTotalIndexCount, FREE_LIST_BMASK + 1, interlocked_acquire_load(freeListWrPos),
    interlocked_acquire_load(freeListRdPos));
  //G_UNUSED(idx);
}
void D3dResManagerData::report_bad_generation_used(D3DRESID id)
{
  NAU_LOG_ERROR("resMgr: access to bad id={:#x} detected (index={} gen={} != {})", (unsigned)id, id.index(), id.generation(),
    getCurGeneration(id.index()));
  //G_UNUSED(id);
}

namespace texmgr_internal
{
eastl::vector<MipBiasRule> mip_bias_rules;
float default_mip_bias = 0.0f;
TextureIdSet anisotropy_exceptions;
TextureFactory *default_tex_mgr_factory = NULL;
NauFastStrMapT<int, -1> managed_tex_map_by_name(strmem, false);
eastl::vector<const char *> managed_tex_map_by_idx(*strmem);
TexTagInfoArray textagInfo;
int mt_enabled = 0;
bool auto_add_tex_on_get_id = false;
bool disable_add_warning = false;
static thread_local int texmgr_critsec_lock_count = 0;
bool texq_load_on_demand = false;
uint8_t texq_load_disabled = 0;
int reload_jobmgr_id = -1;
int dbg_texq_load_sleep_ms = 0;
bool dbg_texq_only_stubs = false;
volatile int drv_res_updates_flush_count = 0;
bool enable_cur_ql_mismatch_assert = true;

static bool always_should_release_tex(BaseTexture *) { return true; }
bool (*should_release_tex)(BaseTexture *b) = &always_should_release_tex;
void (*stop_bkg_tex_loading)(int sleep_quant) = NULL;
void (*hook_on_get_texture_id)(TEXTUREID id) = nullptr;

static struct RmgrTerminator
{
  ~RmgrTerminator() { RMGR.term(); }
} rmgrTerminator;
dag::CritSecStorage crit_sec;
dag::CriticalSection rec_lock;
int (*drv3d_cmd)(int command, void *par1, void *par2, void *par3) = NULL;
}; // namespace texmgr_internal

eastl::vector<BaseTexture *> tql::texStub(*eastl::GetDefaultAllocator());
bool tql::streaming_enabled = false;
int tql::mem_quota_kb = 0;
int tql::mem_quota_limit_kb = INT32_MAX;
int tql::gpu_mem_reserve_kb = 0;
bool tql::lq_not_more_than_split_bq = true;
bool tql::lq_not_more_than_split_bq_difftex = true;

static void skip_on_tex_created(BaseTexture *) {}
static void skip_on_tex_released(BaseTexture *) {}
static void skip_on_buf_changed(bool, int) {}
static void skip_unload_on_drv_shutdown() {}
static unsigned no_tex_lfu(TEXTUREID) { return 0; }
static TexQL no_tex_cur_ql(TEXTUREID) { return TQL_base; }
static bool texture_id_always_valid(TEXTUREID) { return true; }

void (*tql::on_tex_created)(BaseTexture *t) = &skip_on_tex_created;
void (*tql::on_tex_released)(BaseTexture *t) = &skip_on_tex_released;
void (*tql::on_buf_changed)(bool add, int delta_sz_kb) = &skip_on_buf_changed;
void (*tql::unload_on_drv_shutdown)() = &skip_unload_on_drv_shutdown;
void (*tql::on_frame_finished)() = NULL;
unsigned (*tql::get_tex_lfu)(TEXTUREID texId) = &no_tex_lfu;
TexQL (*tql::get_tex_cur_ql)(TEXTUREID texId) = &no_tex_cur_ql;
bool (*tql::check_texture_id_valid)(TEXTUREID texId) = &texture_id_always_valid;

static const char *return_tex_info(TEXTUREID texId, bool /*verbose*/, nau::string &tmp_stor)
{
  if (!RMGR.isValidID(texId, nullptr))
    return "";

  int idx = texId.index();
  //tmp_stor = nau::string::format("  LFU=%d req=%X/%X  desc=0x%04X ld=%X rd=%X max=%X(%X/%X) rc=%d bt.rc=%d ql=%d(%d) gpu=%dK(+%dK) bd=%dK",
  //  RMGR.getResLFU(idx), RMGR.resQS[idx].getMaxReqLev(), RMGR.maxReqLevelPrev[idx], RMGR.levDesc[idx], RMGR.resQS[idx].getLdLev(),
  //  RMGR.resQS[idx].getRdLev(), RMGR.texDesc[idx].dim.maxLev, RMGR.resQS[idx].getQLev(), RMGR.resQS[idx].getMaxLev(),
  //  RMGR.getRefCount(idx), RMGR.getBaseTexUsedCount(idx), RMGR.resQS[idx].getCurQL(), RMGR.resQS[idx].getMaxQL(),
  //  RMGR.getTexMemSize4K(idx) * 4, RMGR.getTexAddMemSizeNeeded4K(idx) * 4, RMGR.getTexBaseDataSize(idx) >> 10);
  return (const char*)tmp_stor.c_str();
}
const char *(*tql::get_tex_info)(TEXTUREID, bool, nau::string &) = &return_tex_info;

static void dump_tex_state_base(TEXTUREID idx) { RMGR.dumpTexState(idx.index()); }

void (*tql::dump_tex_state)(TEXTUREID idx) = &dump_tex_state_base;

dag::CriticalSection tql::reqLDCritSection;
eastl::vector_set<TEXTUREID> tql::reqLD;
int tql::mem_used_persistent_kb = 0;
int tql::mem_quota_reserve_kb = 256 << 10;
#if _TARGET_C1 | _TARGET_C2

#elif _TARGET_64BIT && _TARGET_PC_WIN
int tql::sys_mem_usage_thres_mb = 4096, tql::sys_mem_add_free_mb = 32;
#elif _TARGET_64BIT
int tql::sys_mem_usage_thres_mb = 2048, tql::sys_mem_add_free_mb = 32;
#elif _TARGET_ANDROID || _TARGET_C3
int tql::sys_mem_usage_thres_mb = 512, tql::sys_mem_add_free_mb = 32;
#else
int tql::sys_mem_usage_thres_mb = 1792, tql::sys_mem_add_free_mb = 64;
#endif
int tql::dyn_qlev_decrease = 0;

namespace ddsx
{
int hq_tex_priority = 0;
}

static bool skip_load_ddsx_tex_contents(BaseTexture *, TEXTUREID, TEXTUREID, const ddsx::Header &hdr, nau::iosys::IGenLoad &crd, int, int,
  unsigned)
{
  crd.seekrel(hdr.compressionType() ? hdr.packedSz : hdr.memSz);
  return true;
}
static bool skip_load_ddsx_to_slice(BaseTexture *, int, const ddsx::Header &, nau::iosys::IGenLoad &, int, int, unsigned) { return true; }
static bool skip_load_genmip_sysmemcopy(TEXTUREID, TEXTUREID, const ddsx::Header &, nau::iosys::IGenLoad &, int) { return true; }

bool (*d3d_load_ddsx_tex_contents)(BaseTexture *, TEXTUREID, TEXTUREID, const ddsx::Header &, nau::iosys::IGenLoad &, int, int,
  unsigned) = &skip_load_ddsx_tex_contents;
bool (*d3d_load_ddsx_to_slice)(BaseTexture *, int, const ddsx::Header &, nau::iosys::IGenLoad &, int, int, unsigned) = &skip_load_ddsx_to_slice;
bool (*texmgr_internal::d3d_load_genmip_sysmemcopy)(TEXTUREID, TEXTUREID, const ddsx::Header &, nau::iosys::IGenLoad &,
  int) = &skip_load_genmip_sysmemcopy;

bool texmgr_internal::is_gpu_mem_enough_to_load_hq_tex()
{
  int actual_quota_kb = tql::mem_quota_kb - tql::mem_used_persistent_kb - tql::mem_quota_reserve_kb;
  int overuse = tql::mem_quota_reserve_kb / 4;
  return actual_quota_kb + overuse > RMGR.getTotalUsedTexSzKB() + Vectormath::min(RMGR.getTotalAddMemNeededSzKB(), overuse) ||
         !texmgr_internal::texq_load_on_demand;
}
bool texmgr_internal::is_sys_mem_enough_to_load_basedata()
{
  size_t mem_used_mb = 666;//(dagor_memory_stat::get_memchunk_count(true) * 32 + dagor_memory_stat::get_memory_allocated(true)) >> 20;
  return mem_used_mb < tql::sys_mem_usage_thres_mb || RMGR.getTotalBdCount() <= 2;
}

void texmgr_internal::D3dResMgrDataFinal::setName(unsigned idx, const char *nm, TextureFactory *f)
{
  if (namePtr[idx] && !(f && f->isPersistentTexName(namePtr[idx])))
    memfree(const_cast<char *>(namePtr[idx]), strmem);
  namePtr[idx] = nullptr;
  if (nm && *nm)
    namePtr[idx] = (f && f->isPersistentTexName(nm)) ? nm : str_dup(nm, strmem);
}

template <typename T, T invalidId = (T)BAD_TEXTUREID>
struct PublicFastStrMapT : public NauFastStrMapT<T, invalidId>
{
  using NauFastStrMapT<T, invalidId>::fastMap;
};

void texmgr_internal::add_mip_bias_rule(const char *substring, float bias)
{
  if (!substring || !strcmp(substring, ""))
  {
    default_mip_bias = bias;
    return;
  }

  // Remove old rule with same substring.
  mip_bias_rules.erase(eastl::remove_if(mip_bias_rules.begin(), mip_bias_rules.end(),
                         [substring](const MipBiasRule &r) { return strcmp(r.substring, substring) == 0; }),
    mip_bias_rules.end());

  // Insert new rule to begin of vector, so latest updated rule will always be first, this way we can keep ordering by
  // applying the first matching rule for newly created textures.
  MipBiasRule newRule;
  newRule.substring = substring;
  newRule.bias = bias;
  mip_bias_rules.insert(mip_bias_rules.begin(), eastl::move(newRule));
}

void texmgr_internal::apply_mip_bias_rules(BaseTexture *tex, const char *name)
{
  if (tex == nullptr || name == nullptr || !strcmp(name, ""))
    return;

  float additionalMipBias = default_mip_bias;
  for (MipBiasRule &rule : mip_bias_rules)
  {
    if (strstr(name, rule.substring))
    {
      additionalMipBias = rule.bias;
      break; // Rules are sorted in a way that the latest updated rule will be the fist, so the correct rule will be
             // applied if we break here.
    }
  }
  TextureMetaData tmd;
  tmd.decode(name);
  tex->texlod(tmd.lodBias / 1000.0f + additionalMipBias);
}

void texmgr_internal::remove_from_managed_tex_map(int idx)
{
  NAU_ASSERT(idx >= 0, "idx={}", idx);

  const char *name = texmgr_internal::managed_tex_map_by_idx[idx];
  NAU_ASSERT_RETURN(name, );

  anisotropy_exceptions.del((TEXTUREID)idx);
  int map_idx = texmgr_internal::managed_tex_map_by_name.getStrIndex(name);
  NAU_ASSERT_RETURN(map_idx >= 0, , "idx={} name={}", idx, name);

  auto &mp = static_cast<PublicFastStrMapT<int, -1> &>(texmgr_internal::managed_tex_map_by_name);

  auto f = RMGR.getFactory(idx);
  if (!(f && f->isPersistentTexName(name)))
    memfree((void *)name, &mp.fastMap.get_allocator());

  texmgr_internal::managed_tex_map_by_idx[idx] = nullptr;
  //erase_items(mp.fastMap, map_idx, 1);
  mp.delStrId(map_idx);

#if DAGOR_DBGLEVEL > 0 && (_TARGET_PC || 0) && CHECK_MANAGED_TEX_MAP
  const auto &e = texmgr_internal::managed_tex_map_by_name.getMapRaw();
  for (int i = 0; i < e.size(); ++i)
    NAU_ASSERT(e[i].id != idx, "e[{}].id={} map_idx={} idx={} name={}", i, e[i].id, map_idx, idx, name);
#endif
}

struct GetTexName
{
  char *&operator()(char **r) { return *r; }
};

void set_default_tex_factory(TextureFactory *tf)
{
  if (texmgr_internal::default_tex_mgr_factory)
    texmgr_internal::default_tex_mgr_factory->texFactoryActiveChanged(false);
  texmgr_internal::default_tex_mgr_factory = tf;
  if (texmgr_internal::default_tex_mgr_factory)
    texmgr_internal::default_tex_mgr_factory->texFactoryActiveChanged(true);
}
TextureFactory *get_default_tex_factory() { return texmgr_internal::default_tex_mgr_factory; }

namespace texmgr_internal
{
void acquire_texmgr_lock()
{
  if (mt_enabled)
  {
    dag::enter_critical_section(crit_sec);
    texmgr_critsec_lock_count++;
  }
}

void release_texmgr_lock()
{
  if (mt_enabled)
  {
    texmgr_critsec_lock_count--;
    dag::leave_critical_section(crit_sec);
  }
}

void release_all_texmgr_locks()
{
  if (mt_enabled)
    while (texmgr_critsec_lock_count--)
      dag::leave_critical_section(crit_sec);
}
} // namespace texmgr_internal
