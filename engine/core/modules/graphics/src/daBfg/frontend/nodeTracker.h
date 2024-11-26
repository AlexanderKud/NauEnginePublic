// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <eastl/vector_set.h>
#include "render/daBfg/detail/nodeNameId.h"
#include "dabfg/common/graphDumper.h"
#include "dabfg/id/idIndexedMapping.h"


namespace dabfg
{

struct InternalRegistry;
struct DependencyData;

// Responsible for registering nodes from various APIs
class NodeTracker final : public IGraphDumper
{
public:
  NodeTracker(InternalRegistry &reg, const DependencyData &deps) : registry{reg}, depData{deps} {}

public:
  // A context is an opaque token that can be used to wipe a group of nodes.
  // The intended use case is to pass a scripting language runtime and
  // wipe the nodes when a runtime is hot-reloaded.
  void registerNode(void *context, NodeNameId nodeId);
  void unregisterNode(NodeNameId nodeId, uint16_t gen);

  void wipeContextNodes(void *context);

  // Lazily initializes nodes
  void updateNodeDeclarations();

  bool acquireNodesChanged() { return eastl::exchange(nodesChanged, false); }

  void dumpRawUserGraph() const override;

  void lock()
  {
    NAU_ASSERT(nodeChangesLocked == false);
    nodeChangesLocked = true;
  }
  void unlock() { nodeChangesLocked = false; }

private:
  InternalRegistry &registry;
  const DependencyData &depData;

  eastl::vector_set<NodeNameId> deferredDeclarationQueue;
  IdIndexedMapping<NodeNameId, void *> nodeToContext;
  eastl::vector_set<void *> trackedContexts;

  bool nodesChanged{false};
  bool nodeChangesLocked{false};

  void checkChangesLock() const;
};

} // namespace dabfg
