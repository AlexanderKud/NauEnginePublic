// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "nau/3d/dag_renderStates.h"
#include "nau/3d/dag_sampler.h"
#include "nau/shaders/shInternalTypes.h"
#include "nau/shaders/dag_renderStateId.h"
#include "nau/shaders/dag_shaderCommon.h"
#include "nau/shaders/dag_shaderHash.h"
//#include <math/integer/dag_IPoint4.h>
//#include <math/dag_Tnau::math::Matrix4.h>
#include "nau/util/dag_bindump_ext.h"
#include <EASTL/array.h>

#define USE_16BIT_BLK 1

namespace shader_layout
{
using shaders_internal::Buf;
using shaders_internal::Tex;

#if USE_16BIT_BLK
typedef uint16_t blk_word_t;
enum
{
  BLK_WORD_FULLMASK = 0xFFFF,
  BLK_WORD_BITS = 16
};
#else
typedef uint32_t blk_word_t;
enum
{
  BLK_WORD_FULLMASK = 0xFFFFFFFF,
  BLK_WORD_BITS = 32
};
#endif

BINDUMP_BEGIN_LAYOUT(Var)
  Address<int> valPtr;
  uint16_t nameId = 0;
  uint8_t type = 0, isPublic = 0;
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(VarList)
  BINDUMP_USING_EXTENSION()
  using Var = Field<Var>;
  uint16_t getNameId(int i) const { return v[i].nameId; }
  uint8_t getType(int i) const { return v[i].type; }
  bool isPublic(int i) const { return v[i].isPublic; }
  int size() const { return v.size(); }

  Span<Var> v;

  template <typename Type>
  const Type &get(int index) const
  {
    return *(Type *)&v[index].valPtr.get();
  }
  template <typename Type>
  Type &get(int index)
  {
    return *(Type *)&v[index].valPtr.get();
  }
  template <typename Type>
  void set(int index, const Type &value)
  {
    get<Type>(index) = value;
  }

  inline const Tex &getTex(int i) const { return get<Tex>(i); }
  inline const Buf &getBuf(int i) const { return get<Buf>(i); }

  inline void setTexId(int i, TEXTUREID a) { get<Tex>(i).texId = a; }
  inline void setTex(int i, class BaseTexture *a) { get<Tex>(i).tex = a; }
  inline void setBufId(int i, D3DRESID a) { get<Buf>(i).bufId = a; }
  inline void setBuf(int i, Sbuffer *a) { get<Buf>(i).buf = a; }

  int findVar(int var_name_id) const;
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(Interval)
  BINDUMP_USING_EXTENSION()
  enum
  {
    TYPE_MODE = 0,
    TYPE_INTERVAL,
    TYPE_GLOBAL_INTERVAL,
    TYPE_ASSUMED_INTERVAL,
    TYPE_VIOLATED_ASSUMED_INTERVAL
  };
  enum
  {
    MODE_2SIDED = 0,
    MODE_REAL2SIDED,
    MODE_LIGHTING
  };

  Span<float> maxVal;
  uint16_t nameId = 0;
  uint8_t type = 0;

  inline unsigned getValCount() const { return maxVal.size() + 1; }

  uint32_t getAssumedVal() const
  {
    NAU_ASSERT(type == TYPE_ASSUMED_INTERVAL || type == TYPE_VIOLATED_ASSUMED_INTERVAL);
    // The assumed value is stored in the `mCount` field of the `maxVal` member,
    // this is done because the `maxVal` member is not used for the assumed interval,
    // and there are no other fields, and in order not to change the dump format, it is done this way
    return maxVal.size();
  }

  inline unsigned getNormalizedValue(float v) const
  {
    for (int i = 0; i < maxVal.size(); i++)
      if (v < maxVal[i])
        return i;
    return maxVal.size();
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(VariantTable)
  BINDUMP_USING_EXTENSION()
  struct IntervalBind
  {
    uint16_t intervalId = 0;
    uint16_t totalMul = 0;
  };
  enum
  {
    FIND_NOTFOUND = 0xFFFF, //< returned when variant not found
    FIND_NULL = 0xFFFE,     //< returned when variant is nil
  };
  enum
  {
    MAPTYPE_EQUAL = 0,
    MAPTYPE_LOOKUP,
    MAPTYPE_QDIRECT,
    MAPTYPE_QINTERVAL
  };

  Span<uint16_t> mapData;
  Span<IntervalBind> codePieces;
  int mapType = 0;

  inline int variantCount() const { return (mapType == MAPTYPE_LOOKUP) ? mapData.size() : mapData.size() / 2; }

  inline int findVariant(unsigned code) const
  {
    if (mapType == MAPTYPE_EQUAL)
      return code;
    if (mapType == MAPTYPE_LOOKUP)
      return mapData[code];
    //if (mapType == MAPTYPE_QDIRECT)
    //  return qfindDirectVariant(code);
    //return qfindIntervalVariant(code);
    return -1;
  }

  // This reverses findVariant, it takes a variant index and returns all shader codes that is mapped to this variant
  template <typename CLB>
  inline void enumerateCodesForVariant(unsigned variant, CLB clb) const
  {
    if (MAPTYPE_EQUAL == mapType)
    {
      clb(variant);
    }
    else if (MAPTYPE_LOOKUP == mapType)
    {
      for (uint32_t i = 0; i < mapData.size(); ++i)
      {
        if (variant == mapData[i])
        {
          clb(i);
        }
      }
    }
    else if (MAPTYPE_QDIRECT == mapType)
    {
      auto halfSize = mapData.size() / 2;
      for (uint32_t i = 0; i < halfSize; ++i)
      {
        if (variant == mapData[halfSize + i])
        {
          clb(mapData[i]);
        }
      }
    }
    else
    {
      auto halfSize = mapData.size() / 2;
      for (uint32_t i = 0; i < halfSize; ++i)
      {
        if (variant == mapData[halfSize + i])
        {
          for (uint32_t j = mapData[i]; j < mapData[i + 1]; ++j)
          {
            clb(j);
          }
        }
      }
    }
  }

protected:
  //int qfindDirectVariant(unsigned code) const;
  //int qfindIntervalVariant(unsigned code) const;
BINDUMP_END_LAYOUT()

namespace detail
{
struct IntPair
{
  int i1, i2;
};
struct ShRef
{
  uint16_t vprId, fshId, stcodeId, stblkcodeId, renderStateNo;
  eastl::array<uint16_t, 3> threadGroupSizes;
};
} // namespace detail

BINDUMP_BEGIN_LAYOUT(Pass)
  BINDUMP_USING_EXTENSION()
  Address<detail::ShRef> rpass;
  Span<int> __unused1;
  Span<detail::IntPair> __unused2;
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ShaderCode)
  BINDUMP_USING_EXTENSION()
  using ShRef = detail::ShRef;
  static constexpr int INVALID_FSH_VPR_ID = 0xFFFF;
  using Pass = Field<Pass>;
  Field<VariantTable> dynVariants;
  VecHolder<Pass> passes;
  Span<uint32_t> stVarMap;
  Span<ShaderChannelId> channel;
  Span<int> initCode;

  int16_t varSize = 0, __unused1 = -1, codeFlags = 0;
  int16_t vertexStride = 0;
  VecHolder<Span<blk_word_t>> suppBlockUid;

  int findVar(int var_id) const;

  inline bool isBlockSupported(int dyn_var_n, unsigned sw) const
  {
    const blk_word_t *sb = suppBlockUid[dyn_var_n].data();
    while (*sb != BLK_WORD_FULLMASK)
      if (sw == *sb)
        return true;
      else
        sb++;
    return false;
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ShaderClass)
  BINDUMP_USING_EXTENSION()
  VecHolder<ShaderCode<>> code;
  Field<VariantTable> stVariants;
  Field<VarList> localVars;
  int32_t nameId = -1, __unused3 = -1;
  Span<char> name;

  Span<int> initCode;

  // storages
  VecHolder<detail::ShRef> shrefStorage;
  VecHolder<ShaderVarTextureType> staticTextureTypeBySlot;
  VecHolder<detail::IntPair> __unused2;
  VecHolder<ShaderChannelId> chanStorage;
  VecHolder<int> icStorage;
  VecHolder<uint32_t> svStorage;

BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ShaderBlock)
  blk_word_t uidMask = 0, uidVal = 0;
  blk_word_t suppBlkMask = 0;
  int16_t stcodeId = 0, nameId = 0;
  Address<blk_word_t> suppBlockUid;

  inline unsigned modifyBlockStateWord(unsigned sw) const { return (sw & ~uidMask) | uidVal; }

  inline bool isBlockSupported(unsigned sw) const
  {
    if (!suppBlkMask)
      return true;

    const blk_word_t *sb = &suppBlockUid.get();
    blk_word_t w = (sw & suppBlkMask);

    while (*sb != BLK_WORD_FULLMASK)
      if (w == *sb)
        return true;
      else
        sb++;
    return false;
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ShGroup)
  BINDUMP_USING_EXTENSION()
  VecHolder<VecHolder<uint8_t>> shaders;
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ScriptedShadersBinDump)
  BINDUMP_USING_EXTENSION()
  VecHolder<shaders::RenderState> renderStates;
  List<VecHolder<int>> stcode;
  List<int> vprId, fshId;

  // storages
  VecHolder<float> iValStorage;
  VecHolder<VariantTable<>::IntervalBind> vtPcsStorage;
  VecHolder<uint16_t> vtLmapStorage;
  VecHolder<uint16_t> vtQmapStorage;
  VecHolder<int, 4 * sizeof(int)> varStorage;
  LayoutList<Var> variables;
  VecHolder<blk_word_t> blkPartSign;
  VecHolder<blk_word_t> globalSuppBlkSign;
  VecHolder<int> shInitCodeStorage;

  // common intervals
  VecHolder<Interval<>> intervals;

  // global data
  VecHolder<StrHolder> varMap;
  Field<VarList> globVars;
  VecHolder<uint32_t> gvMap;
  int maxRegSize = 0;

  // shader classes
  VecHolder<StrHolder> shaderNameMap;
  VecHolder<ShaderClass<>> classes;

  // shader state blocks
  VecHolder<StrHolder> blockNameMap;
  VecHolder<ShaderBlock<>> blocks;

  // shader hashes
  VecHolder<ShaderHashValue> shaderHashes;

  // compressed shaders
  struct ShGroupsMapping
  {
    uint16_t groupId = 0;
    uint16_t indexInGroup = 0;
  };
  VecHolder<ShGroupsMapping> shGroupsMapping;
  VecHolder<Compressed<Field<ShGroup>>> shGroups;
  VecHolder<char> dictionary;

  // shader context/work data
  enum
  {
    MAX_VARS = 3072,
    VARIDX_ABSENT = 0xFFFEu,
    VARIDX_INVALID = 0xFFFFu
  };
  uint16_t varIdx[MAX_VARS] = {}, globvarIdx[MAX_VARS] = {};

  void reinitVarTables();

  const Field<ShaderClass> *findShaderClass(const char *name) const;
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(ScriptedShadersBinDumpCompressed)
  BINDUMP_USING_EXTENSION()
  uint32_t signPart1, signPart2;
  uint32_t version;
  CompressedLayout<ScriptedShadersBinDump> scriptedShadersBindumpCompressed;
BINDUMP_END_LAYOUT()

// New version of shaders bindump

BINDUMP_BEGIN_LAYOUT(IntervalInfo)
  BINDUMP_USING_EXTENSION()
  uint32_t intervalId = 0;
  uint32_t intervalNameHash = 0;
  VecHolder<StrHolder> subintervals;
  VecHolder<uint32_t> subintervalHashes;
  // subinterval name hash -> subinterval index
  VecHolder<uint8_t> subintervalIndexByHash;
  uint32_t hashShift = 0;

  inline uint8_t getSubintervalIndexByHash(uint32_t subinterval_name_hash) const
  {
    return subintervalIndexByHash[(subinterval_name_hash >> hashShift) & (subintervalIndexByHash.size() - 1)];
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_LAYOUT(IntervalInfosBucket)
  BINDUMP_USING_EXTENSION()
  // interval name hash -> interval info
  VecHolder<IntervalInfo<>> intervalInfoByHash;
  uint32_t hashShift = 0;

  inline Field<IntervalInfo> &getIntervalInfoByHash(uint32_t interval_name_hash)
  {
    return intervalInfoByHash[(interval_name_hash >> hashShift) & (intervalInfoByHash.size() - 1)];
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_EXTEND_LAYOUT(ScriptedShadersBinDumpV2, ScriptedShadersBinDump)
  BINDUMP_USING_EXTENSION()
  // shader class id -> array of messages
  VecHolder<VecHolder<StrHolder>> messagesByShclass;
  // interval name hash -> interval infos bucket
  VecHolder<IntervalInfosBucket<>> intervalInfosBuckets;

  Field<IntervalInfosBucket> &getIntervalInfosBucketByHash(uint32_t interval_name_hash)
  {
    return intervalInfosBuckets[interval_name_hash & (intervalInfosBuckets.size() - 1)];
  }

  inline Field<IntervalInfo> &getIntervalInfoByHash(uint32_t interval_name_hash)
  {
    auto &bucket = getIntervalInfosBucketByHash(interval_name_hash);
    return bucket.getIntervalInfoByHash(interval_name_hash);
  }

  bool isIntervalPresented(uint32_t interval_name_hash)
  {
    auto &bucket = getIntervalInfosBucketByHash(interval_name_hash);
    if (bucket.intervalInfoByHash.empty())
      return false;
    return bucket.getIntervalInfoByHash(interval_name_hash).intervalNameHash == interval_name_hash;
  }
BINDUMP_END_LAYOUT()

BINDUMP_BEGIN_EXTEND_LAYOUT(ScriptedShadersBinDumpV3, ScriptedShadersBinDumpV2)
  BINDUMP_USING_EXTENSION()
  VecHolder<d3d::SamplerInfo> samplers;
BINDUMP_END_LAYOUT()
} // namespace shader_layout

