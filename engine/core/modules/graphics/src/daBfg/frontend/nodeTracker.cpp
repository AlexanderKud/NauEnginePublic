// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "nodeTracker.h"

#include "dabfg/debug/backendDebug.h"
#include "nau/math/dag_random.h"

#include "dabfg/frontend/dumpInternalRegistry.h"
#include "dabfg/frontend/dependencyDataCalculator.h"
#include "dabfg/frontend/internalRegistry.h"


// Useful for ensuring that not dependencies are missing.
// Will shuffle nodes while preserving all ordering constraints.
// If anything depends on a "lucky" node order, this should break it.
//CONSOLE_BOOL_VAL("dabfg", randomize_order, false);
bool randomize_order = false;


namespace dabfg
{

void NodeTracker::registerNode(void *context, NodeNameId nodeId)
{
  checkChangesLock();

  nodesChanged = true;
  invalidate_graph_visualization();

  deferredDeclarationQueue.emplace(nodeId);

  // Make sure that the id is mapped, as the nodes map is the
  // single point of truth here.
  registry.nodes.get(nodeId);

  nodeToContext.set(nodeId, context);
  trackedContexts.insert(context);
}

void NodeTracker::unregisterNode(NodeNameId nodeId, uint16_t gen)
{
  checkChangesLock();

  // If there was no such node in the first place, no need to
  // invalidate caches and try to clean up
  if (!registry.nodes.get(nodeId).declare)
    return;

  // If the node was already re-registered and generation incremented,
  // we shouldn't wipe the "new" node's data
  if (gen < registry.nodes[nodeId].generation)
    return;

  nodesChanged = true;
  invalidate_graph_visualization();

  // In case the node didn't have a chance to declare resources yet,
  // clear from it the cache
  deferredDeclarationQueue.erase(nodeId);

  auto evictResId = [this](ResNameId res_id) {
    // Invalidate all renamed versions of this resource
    for (;;)
    {
      if (registry.resources.isMapped(res_id))
      {
        // History needs to be preserved, as it is determined by
        // the virtual resource itself and not inhereted from the
        // thing that was renamed into it. Everything else should be
        // reset to the defaults.
        auto &res = registry.resources.get(res_id);
        auto oldHist = res.history;
        res = {};
        res.history = oldHist;
      }

      if (!depData.renamingChains.isMapped(res_id) || depData.renamingChains[res_id] == res_id)
        break;

      res_id = depData.renamingChains[res_id];
    }
  };

  auto &nodeData = registry.nodes[nodeId];

  // Evict all resNameIds *produced* by this node
  for (const auto &[to, _] : nodeData.renamedResources)
    evictResId(to);

  for (const auto &resId : nodeData.createdResources)
    evictResId(resId);

  // Clear node data
  nodeData = {};
  nodeData.generation = gen + 1;
  nodeToContext.set(nodeId, {});
}

void NodeTracker::wipeContextNodes(void *context)
{
  const auto it = trackedContexts.find(context);
  if (it == trackedContexts.end())
    return;

  for (auto [nodeId, ctx] : nodeToContext.enumerate())
    if (ctx == context)
      unregisterNode(nodeId, registry.nodes[nodeId].generation);

  trackedContexts.erase(it);
}

void NodeTracker::updateNodeDeclarations()
{
  if (randomize_order)
    eastl::random_shuffle(deferredDeclarationQueue.begin(), deferredDeclarationQueue.end(),
      [](uint32_t n) { return static_cast<uint32_t>(grnd() % n); });

  for (auto nodeId : deferredDeclarationQueue)
    if (auto &declare = registry.nodes.get(nodeId).declare)
    {
      // Reset the value first to avoid funny side-effects later on
      registry.nodes[nodeId].execute = {};
      registry.nodes[nodeId].execute = declare(nodeId, &registry);
    }

  deferredDeclarationQueue.clear();

  // Makes sure that further code doesn't go out of bounds on any of these
  registry.resources.resize(registry.knownNames.nameCount<ResNameId>());
  registry.nodes.resize(registry.knownNames.nameCount<NodeNameId>());
  registry.autoResTypes.resize(registry.knownNames.nameCount<AutoResTypeNameId>());
  registry.resourceSlots.resize(registry.knownNames.nameCount<ResNameId>());
}

void NodeTracker::dumpRawUserGraph() const { dump_internal_registry(registry); }

void NodeTracker::checkChangesLock() const
{
  if (nodeChangesLocked)
  {
    NAU_LOG_ERROR("Attempted to modify framegraph structure while it was being executed!"
           "This is not supported, see callstack and remove the modification!");
  }
}

} // namespace dabfg
