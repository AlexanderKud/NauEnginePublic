// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "render/daBfg/detail/nodeNameId.h"


namespace dabfg::detail
{

// Index stamped with a generational counter.
// Used to implement RAII node handles
// NOTE: there's an additional validness flag to make this type
// trivially relocatable from das's point of view
// NOTE: nodeId should always be valid within this structure
struct NodeUid
{
  NodeNameId nodeId;
  uint16_t generation : 15;
  uint16_t valid : 1;
};

static_assert(sizeof(NodeUid) == sizeof(uint16_t) * 2);

} // namespace dabfg::detail
