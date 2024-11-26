// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <nau/diag/assertion.h>

// sample usage
// class EntityIdDummy {};
// typedef GenerationRefId<8, EntityIdDummy> EntityId;// 8 bits for generation
// see also generationReferenceData.h

// class T is for strong typing of different GenerationRefId with same amount of bits
template <unsigned generation_bits, class T>
class GenerationRefId
{
  static_assert(generation_bits <= 16);

public:
  GenerationRefId() = default;
  explicit GenerationRefId(uint32_t h) : handle(h) {}
  explicit operator uint32_t() const { return handle; }
  explicit operator bool() const { return handle != INVALID_ID; }
  bool operator==(const GenerationRefId &rhs) const { return handle == rhs.handle; }
  bool operator!=(const GenerationRefId &rhs) const { return handle != rhs.handle; }
  bool operator<(const GenerationRefId &rhs) const { return handle < rhs.handle; }
  bool operator>(const GenerationRefId &rhs) const { return handle > rhs.handle; }
  void reset() { handle = INVALID_ID; }
  unsigned index() const { return handle & INDEX_MASK; }
  unsigned generation() const { return (handle >> INDEX_BITS); }
  enum : uint32_t
  {
    INVALID_ID = ~0u,
    GENERATION_BITS = generation_bits,
    INDEX_BITS = 32 - generation_bits,
    INDEX_MASK = (1 << INDEX_BITS) - 1,
    GENERATION_MASK = ~INDEX_MASK
  };
  static inline GenerationRefId make(uint32_t index, uint32_t gen)
  {
    return GenerationRefId((index & INDEX_MASK) | (gen << INDEX_BITS));
  }

private:
  uint32_t handle = INVALID_ID;
};
