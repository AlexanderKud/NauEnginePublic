// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

namespace dabfg
{

// Prints a raw list of nodes and their declared resources.
// Useful for diagnosing inconsistent graphs when compilation fails.
class IGraphDumper
{
public:
  virtual void dumpRawUserGraph() const = 0;

protected:
  ~IGraphDumper() = default;
};

} // namespace dabfg
