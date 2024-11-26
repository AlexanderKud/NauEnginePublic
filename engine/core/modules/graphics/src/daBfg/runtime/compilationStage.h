// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once


namespace dabfg
{

enum class CompilationStage
{
  REQUIRES_NODE_DECLARATION_UPDATE,
  REQUIRES_NAME_RESOLUTION,
  REQUIRES_DEPENDENCY_DATA_CALCULATION,
  REQUIRES_IR_GRAPH_BUILD,
  REQUIRES_NODE_SCHEDULING,
  REQUIRES_STATE_DELTA_RECALCULATION,
  REQUIRES_RESOURCE_SCHEDULING,
  REQUIRES_HISTORY_OF_NEW_RESOURCES_INITIALIZATION,
  UP_TO_DATE,

  REQUIRES_FULL_RECOMPILATION = REQUIRES_NODE_DECLARATION_UPDATE
};

inline bool operator<(CompilationStage first, CompilationStage second) { return static_cast<int>(first) < static_cast<int>(second); }

} // namespace dabfg
