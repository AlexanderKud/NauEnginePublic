// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once
#include <daECS/core/entityManager.h>

namespace ecs
{

#if TIME_PROFILER_ENABLED && DAGOR_DBGLEVEL > 0
extern bool events_perf_markers;
#define PROFILE_ES(es, evt) (EASTL_UNLIKELY(events_perf_markers || evt.getFlags() & EVFLG_PROFILE))
#define TIME_SCOPE_ES(es)        \
  if (events_perf_markers)       \
    es.cacheProfileTokensOnce(); \
  DA_PROFILE_EVENT_DESC(es.dapToken);
#else
#define TIME_SCOPE_ES(es)
#define PROFILE_ES(es, evt) false
#endif

__forceinline void EntityManager::dispatchEventImmediate(EntityId eid, Event &evt)
{
  if (eid)
  {
    auto state = entDescs.getEntityState(eid);
    if (state == EntitiesDescriptors::EntityState::Dead)
      return;
    if (state == EntitiesDescriptors::EntityState::Loading) // it is likely that it is creating entity. As it is scheduled to be
                                                            // created, we can't send immediate event to it anyway.
    {
#if NAU_DEBUG // do not check that in release
      unsigned index = eid.index();
        if(index < entDescs.allocated_size() && entDescs.currentlyCreatingEntitiesCnt[index] == 0)
        {
            logerr("Entity {} isn't scheduled for creation, but has no archetype, event {}.\n", eid, evt.getName());
        }
#endif
      addEventForLoadingEntity(eid, evt);
    }
    else
    {
      DAECS_EXT_ASSERT(!(evt.getFlags() & EVFLG_CORE));
      notifyESEventHandlers(eid, evt);
    }
  }
  else
    broadcastEventImmediate(evt);
}

template <class T, typename ProcessEvent>
__forceinline uint32_t EntityManager::processEventInternal(T &buffer, ProcessEvent &&process)
{
  if (EASTL_LIKELY(!buffer.canRead(sizeof(Event) + sizeof(EntityId)))) // empty
    return 0;

  const char *__restrict reading = (const char *__restrict)buffer.reading();
  EntityId eid = *(const EntityId *__restrict)reading;
  Event *__restrict event = (Event *__restrict)(reading + sizeof(EntityId));
  const uint32_t eventSize = uint32_t(event->getLength()) + uint32_t(sizeof(EntityId));
  auto finalizeRead = buffer.justRead(eventSize);
  // we can't use reference to buffer after process
  process(eid, *event);
  if (EASTL_UNLIKELY(event->getFlags() & EVFLG_DESTROY))
    eventDb.destroy(*event);

  T::freeRead(finalizeRead, eventSize);
  return eventSize;
}

template <class T>
inline uint32_t EntityManager::processEventsActive(uint32_t count, T &storage)
{
  DAECS_EXT_FAST_ASSERT(storage.canProcess());
  uint32_t processed = 0;
  for (; processed != count && storage.canProcess(); ++processed)
  {
    const uint32_t readSz =
      processEventInternal(storage.active, [&](EntityId eid, Event &event) { dispatchEventImmediate(eid, event); });
    if (!readSz)
      break;
#if NAU_DEBUG
    auto &buf = storage.canProcess() ? storage.active : storage.buffers.back();
    DAECS_EXT_ASSERTF(int(*buf.alreadyRead) <= int(buf.readAt), "alreadyRead={} writeAt={}, readAt = {}, sz={}", *buf.alreadyRead,
      buf.writeAt, buf.readAt, readSz);
#endif
  }
  storage.active.normalize();
  return processed;
}

template <class T>
inline uint32_t EntityManager::processEventsAnyway(uint32_t count, T &storage)
{
  if (EASTL_LIKELY(storage.canProcess())) // otherwise too many events scheduled, circular buffer exhausted, we were only writing!
    return processEventsActive(count, storage);
  return processEventsExhausted(count, storage);
}

inline void ecs::EntityManager::sendQueuedEvents(uint32_t top_send_count)
{
  if (!deferredEventsCount)
    return;
  TIME_PROFILE_DEV(ecs_send_queued_events);
  uint32_t processed = processEventsAnyway(top_send_count, eventsStorage);
  // it can be less than that, if we face data race, which is assume to be covered by mutex OR if we process 'end markers' events
  deferredEventsCount = std::max((int)deferredEventsCount - (int)processed, (int)0); // event left
  current_tick_events += processed;
}

} // namespace ecs
