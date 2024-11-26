// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "render/daBfg/bfg.h"
#include "dabfg/runtime/runtime.h"


namespace dabfg
{

detail::NodeUid detail::register_node(NameSpaceNameId nsId, const char *name, const char *node_source,
  DeclarationCallback &&declaration_callback)
{
  auto &runtime = Runtime::get();
  auto &tracker = runtime.getNodeTracker();
  auto &registry = runtime.getInternalRegistry();

  const auto nodeId = registry.knownNames.addNameId<NodeNameId>(nsId, name);

  tracker.unregisterNode(nodeId, registry.nodes.get(nodeId).generation);
  tracker.registerNode(nullptr, nodeId);

  auto &nodeData = registry.nodes[nodeId];
  nodeData.declare = eastl::move(declaration_callback);
  nodeData.nodeSource = node_source;

  return {nodeId, nodeData.generation, 1};
}

void detail::set_node_enabled(NodeUid nodeId, bool enabled)
{
    auto &runtime = Runtime::get();
    auto &registry = runtime.getInternalRegistry();

    NAU_ASSERT(registry.nodes.isMapped(nodeId.nodeId));

    registry.nodes[nodeId.nodeId].enabled = enabled;
}

void detail::unregister_node(detail::NodeUid uid)
{
  auto &tracker = Runtime::get().getNodeTracker();
  tracker.unregisterNode(uid.nodeId, uid.generation);
}

NameSpace root() { return {}; }

void update_external_state(ExternalState state) { Runtime::get().updateExternalState(state); }

void set_multiplexing_extents(multiplexing::Extents extents) { Runtime::get().setMultiplexingExtents(extents); }

void run_nodes() { Runtime::get().runNodes(); }

void startup() { Runtime::startup(); }

void shutdown() { Runtime::shutdown(); }

} // namespace dabfg
