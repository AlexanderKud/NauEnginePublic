// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "nameResolver.h"
#include "nau/memory/eastl_aliases.h"


namespace dabfg
{

void NameResolver::update()
{
  updateMapping();
  updateInverseMapping();
}

void NameResolver::updateMapping()
{
  // This validity info simply tells us whether an entity was created
  // from a user's point of view and is only used for the purposes of
  // name lookup.
  IdIndexedFlags<ResNameId, nau::EastlFrameAllocator> resourceValid(registry.knownNames.nameCount<ResNameId>(), false);
  IdIndexedFlags<NodeNameId, nau::EastlFrameAllocator> nodeValid(registry.knownNames.nameCount<NodeNameId>(), false);
  for (auto [nodeId, nodeData] : registry.nodes.enumerate())
  {
    if (!nodeData.declare)
      continue;
    nodeValid[nodeId] = true;
    for (auto resId : nodeData.createdResources)
      resourceValid[resId] = true;
    for (auto [to, from] : nodeData.renamedResources)
      resourceValid[to] = true;
  }
  for (auto [resId, data] : registry.resourceSlots.enumerate())
    if (data.has_value())
      resourceValid[resId] = true;

  IdIndexedFlags<AutoResTypeNameId, nau::EastlFrameAllocator> autoResTypeValid(registry.knownNames.nameCount<AutoResTypeNameId>(), false);
  for (auto [autoResTypeId, data] : registry.autoResTypes.enumerate())
    if (data.staticResolution.getX() > 0 && data.staticResolution.getY() > 0 && data.dynamicResolution.getX() > 0 && data.dynamicResolution.getY() > 0)
      autoResTypeValid[autoResTypeId] = true;


  resolver.rebuild(registry.knownNames, resourceValid, nodeValid, autoResTypeValid);
}

void NameResolver::updateInverseMapping()
{
  // Don't free memory pre-allocated inside the vector maps
  inverseMapping.resize(registry.knownNames.nameCount<NodeNameId>());
  for (auto &mapping : inverseMapping)
    mapping.clear();
  inverseHistoryMapping.resize(registry.knownNames.nameCount<NodeNameId>());
  for (auto &mapping : inverseHistoryMapping)
    mapping.clear();
  for (auto [nodeId, nodeData] : registry.nodes.enumerate())
  {
    for (const auto &[resId, _] : nodeData.resourceRequests)
      inverseMapping[nodeId][resolve(resId)].push_back(resId);
    for (const auto &[resId, _] : nodeData.historyResourceReadRequests)
      inverseHistoryMapping[nodeId][resolve(resId)].push_back(resId);
  }
}

template <class T>
T NameResolver::resolve(T name_id) const
{
  // Missing optional resources will not get resolved to anything
  if (auto resolvedId = resolver.resolve<T>(name_id); resolvedId != T::Invalid)
    name_id = resolvedId;

  // We may want to have slots stuff other than resources in the future
  if constexpr (eastl::is_same_v<T, ResNameId>)
    if (const auto &slotData = registry.resourceSlots[name_id]; slotData.has_value())
      name_id = slotData->contents;

  return name_id;
}

template ResNameId NameResolver::resolve(ResNameId res_name_id) const;
template NodeNameId NameResolver::resolve(NodeNameId res_name_id) const;
template AutoResTypeNameId NameResolver::resolve(AutoResTypeNameId res_name_id) const;

eastl::span<ResNameId const> NameResolver::unresolve(NodeNameId nodeId, ResNameId resId) const
{
  const auto it = inverseMapping[nodeId].find(resId);
  return it != inverseMapping[nodeId].end() ? it->second : eastl::span<ResNameId const>{};
}

} // namespace dabfg
