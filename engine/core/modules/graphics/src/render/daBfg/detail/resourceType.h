// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/bit.h>
#include <EASTL/numeric_limits.h>
#include <stdint.h>


// TODO: probably should move this to detail namespace too
namespace dabfg
{

// Broad categorization of resources
enum class ResourceType : uint8_t
{
  Invalid,
  Texture,
  Buffer,
  Blob
};

// Tags used for validating resource types on requests
enum class ResourceSubtypeTag : uintptr_t
{
  Unknown = 0,
  Invalid = ~uintptr_t{0}
};

inline bool resourceTagsMatch(ResourceSubtypeTag fst, ResourceSubtypeTag snd)
{
  return fst == ResourceSubtypeTag::Unknown || snd == ResourceSubtypeTag::Unknown || fst == snd;
}

namespace detail
{
template <class T>
inline void *tagger{nullptr};
}

template <class T>
inline ResourceSubtypeTag tag_for()
{
  return static_cast<ResourceSubtypeTag>(eastl::bit_cast<uintptr_t>(&detail::tagger<T>));
}

} // namespace dabfg
