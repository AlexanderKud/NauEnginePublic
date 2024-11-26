// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include "nau/3d/dag_resId.h"
#include <EASTL/vector_set.h>
//#include <generic/dag_span.h>

class TextureIdSet : public eastl::vector_set<TEXTUREID>
{
public:
  void reset() { clear(); }

  bool add(TEXTUREID tid) { return insert(tid).second; }
  bool del(TEXTUREID tid) { return erase(tid); }
  bool has(TEXTUREID tid) const { return find(tid) != end(); }
};
