// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <daECS/net/component_replication_filter.h>
#include <generic/dag_smallTab.h>

namespace net
{
extern eastl::vector<replicate_component_filter_index_t> replicate_component_filter_index; // we use eastl::vector, as it's size() is faster.
extern eastl::vector<component_replication_filter> replicate_component_filters;

extern uint32_t dirty_component_filter_mask;
void register_pending_component_filters();
inline replicate_component_filter_index_t get_replicate_component_filter_index(ecs::component_index_t cidx)
{
  return cidx < replicate_component_filter_index.size() ? replicate_component_filter_index[cidx] : replicate_everywhere_filter_id;
}
inline CompReplicationFilter should_skip_component_replication(ecs::EntityId eid, ecs::component_index_t cidx, const IConnection *conn,
  ConnectionId controlled_by)
{
  replicate_component_filter_index_t it = get_replicate_component_filter_index(cidx);
  if (it != replicate_everywhere_filter_id)
    return replicate_component_filters[it](eid, controlled_by, conn);
  return CompReplicationFilter::ReplicateForConnection;
}
} // namespace net