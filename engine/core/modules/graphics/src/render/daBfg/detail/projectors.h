// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <EASTL/functional.h>

namespace dabfg::detail
{

using TypeErasedProjector = const void *(*)(const void *);

template <class T>
constexpr bool is_const_lvalue_reference = eastl::is_lvalue_reference_v<T> && eastl::is_const_v<eastl::remove_reference_t<T>>;

template <auto projector, class T>
TypeErasedProjector erase_projector_type()
{
  return [](const void *blob) -> const void * { return &eastl::invoke(projector, *static_cast<const T *>(blob)); };
};

} // namespace dabfg::detail
