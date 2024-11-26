// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#include <daECS/core/baseIo.h>
#include <daECS/core/entityManager.h>
#include <daECS/core/internal/trackComponentAccess.h>
namespace ecs
{

bool can_serialize_type(ecs::type_index_t typeId)
{
  ecs::ComponentType componentTypeInfo = g_entity_mgr->getComponentTypes().getTypeInfo(typeId);
  // todo: check for component serializer
  return ecs::is_pod(componentTypeInfo.flags) || (componentTypeInfo.flags & COMPONENT_TYPE_HAS_IO);
}

bool can_serialize(const ecs::EntityComponentRef &comp) { return can_serialize_type(comp.getTypeId()); }

bool should_replicate(const ecs::EntityComponentRef &comp)
{
  if (!can_serialize(comp) || comp.getUserType() == ComponentTypeInfo<ecs::Tag>::type)
    return false;
  DataComponent component = g_entity_mgr->getDataComponents().getComponentById(comp.getComponentId());
  return !(component.flags & (DataComponent::DONT_REPLICATE | DataComponent::IS_COPY));
}

bool should_replicate(component_index_t cidx)
{
  DataComponent component = g_entity_mgr->getDataComponents().getComponentById(cidx);
  if (!can_serialize_type(component.componentType) || component.componentTypeName == ComponentTypeInfo<ecs::Tag>::type)
    return false;
  return !(component.flags & (DataComponent::DONT_REPLICATE | DataComponent::IS_COPY));
}

void write_string(ecs::SerializerCb &cb, const char *pStr, uint32_t max_string_len)
{
  for (const char *str = pStr; *str && str - pStr < max_string_len; str++)
    cb.write(str, sizeof(str[0]) * CHAR_BIT, 0);
  const char c = 0;
  cb.write(&c, sizeof(c) * CHAR_BIT, 0);
}

int read_string(const ecs::DeserializerCb &cb, char *buf, uint32_t buf_size)
{
  buf_size--;
  char *str = buf;
  do
  {
    if (!cb.read(str, 8, 0))
      return -1;
    if (str == buf + buf_size)
      *str = 0;
  } while (*(str++));
  return str - buf;
}

void serialize_entity_component_ref_typeless(const void *comp_data, component_index_t cidx, component_type_t type_name,
  type_index_t type_id, SerializerCb &serializer)
{
  auto &componentTypes = g_entity_mgr->getComponentTypes();
  ComponentSerializer *typeIO = nullptr;
  if (cidx != ecs::INVALID_COMPONENT_INDEX)
    typeIO = g_entity_mgr->getDataComponents().getComponentIO(cidx);
  ComponentType componentTypeInfo = componentTypes.getTypeInfo(type_id);

  const bool isPod = is_pod(componentTypeInfo.flags);
  if (!typeIO && has_io(componentTypeInfo.flags))
    typeIO = componentTypes.getTypeIO(type_id);
  NAU_ASSERT(typeIO || isPod);
  if (!typeIO && isPod) // pod data can be just readed as-is
  {
    serializer.write(comp_data, componentTypeInfo.size * 8, type_name);
  }
  else if (typeIO)
  {
    const bool isBoxed = (componentTypeInfo.flags & COMPONENT_TYPE_BOXED) != 0;
    typeIO->serialize(serializer, isBoxed ? *(void **)comp_data : comp_data, componentTypeInfo.size, type_name);
  }
}

void serialize_entity_component_ref_typeless(const void *comp_data, component_type_t type_name, SerializerCb &serializer)
{
  serialize_entity_component_ref_typeless(comp_data, ecs::INVALID_COMPONENT_INDEX, type_name,
    g_entity_mgr->getComponentTypes().findType(type_name), serializer);
}

void serialize_entity_component_ref_typeless(const EntityComponentRef &comp, SerializerCb &serializer)
{
  NAU_ASSERT(comp.getComponentId() != INVALID_COMPONENT_INDEX);
  serialize_entity_component_ref_typeless(comp.getRawData(), comp.getComponentId(), comp.getUserType(), comp.getTypeId(), serializer);
}

bool deserialize_component_typeless(EntityComponentRef &comp, const DeserializerCb &deserializer)
{
  auto &componentTypes = g_entity_mgr->getComponentTypes();
  ecs::component_index_t cidx = comp.getComponentId();
  type_index_t typeId = comp.getTypeId();
  component_type_t userType = comp.getUserType();
  ComponentSerializer *typeIO = nullptr;
  if (cidx != ecs::INVALID_COMPONENT_INDEX)
    typeIO = g_entity_mgr->getDataComponents().getComponentIO(cidx);
  ComponentType componentTypeInfo = componentTypes.getTypeInfo(typeId);
  if (!typeIO && has_io(componentTypeInfo.flags))
    typeIO = componentTypes.getTypeIO(typeId);
  ecsdebug::track_ecs_component_by_index(comp.getComponentId(), ecsdebug::TRACK_WRITE, "deserialize");
  if (typeIO)
  {
    const bool isBoxed = (componentTypeInfo.flags & COMPONENT_TYPE_BOXED) != 0;
    return typeIO->deserialize(deserializer, isBoxed ? *(void **)comp.getRawData() : comp.getRawData(), componentTypeInfo.size,
      userType);
  }
  else if (is_pod(componentTypeInfo.flags))
    return deserializer.read(comp.getRawData(), componentTypeInfo.size * CHAR_BIT, userType);
  else
  {
    logerr("Attempt to deserialize type 0x{} {}<{}>, which has no typeIO and not pod", userType, typeId,
      componentTypes.getTypeNameById(typeId));
    return false;
  }
}

bool deserialize_component_typeless(void *raw_data, component_type_t type_name, const DeserializerCb &deserializer)
{
  auto &componentTypes = g_entity_mgr->getComponentTypes();
  type_index_t typeId = g_entity_mgr->getComponentTypes().findType(type_name);
  ComponentSerializer *typeIO = nullptr;
  ComponentType componentTypeInfo = componentTypes.getTypeInfo(typeId);
  if (has_io(componentTypeInfo.flags))
    typeIO = componentTypes.getTypeIO(typeId);
  if (typeIO)
  {
    const bool isBoxed = (componentTypeInfo.flags & COMPONENT_TYPE_BOXED) != 0;
    return typeIO->deserialize(deserializer, isBoxed ? *(void **)raw_data : raw_data, componentTypeInfo.size, type_name);
  }
  else if (is_pod(componentTypeInfo.flags))
    return deserializer.read(raw_data, componentTypeInfo.size * CHAR_BIT, type_name);
  else
  {
    logerr("Attempt to deserialize type 0x{} {}<{}>, which has no typeIO and not pod", type_name, typeId,
      componentTypes.getTypeNameById(typeId));
    return false;
  }
}

// in network deserializing of entity component we should not use this codepath
// instead of always deserializing to list of components, we should directly deserialize components to already created components
// ChildComponent should be only for case of initial/template serialization (or object types, obviously)
// if replication packet arrive before Entity is created, just store it is _packet_, no need to deserialize it
// current version always make a lot of copies/allocations for no reason
eastl::optional<ChildComponent> deserialize_init_component_typeless(component_type_t userType, ecs::component_index_t cidx,
  const DeserializerCb &deserializer)
{
  if (userType == 0)
    return ChildComponent(); // Assume that null type is not an error
  auto &componentTypes = g_entity_mgr->getComponentTypes();
  ComponentSerializer *typeIO = nullptr;
  type_index_t typeId;
  if (cidx != ecs::INVALID_COMPONENT_INDEX)
  {
    auto &components = g_entity_mgr->getDataComponents();
    DataComponent dataComp = components.getComponentById(cidx);
    typeId = dataComp.componentType;
    DAECS_EXT_ASSERT(dataComp.componentTypeName == userType);
    if (dataComp.flags & DataComponent::HAS_SERIALIZER)
    {
      typeIO = components.getComponentIO(cidx);
      DAECS_EXT_FAST_ASSERT(typeIO != nullptr);
    }
  }
  else
  {
    typeId = componentTypes.findType(userType); // hash lookup
    if (EASTL_UNLIKELY(typeId == INVALID_COMPONENT_TYPE_INDEX))
    {
      logerr("Attempt to deserialize component of invalid/unknown type 0x{}", userType);
      return MaybeChildComponent(); // We can't read unknown type of unknown size
    }
  }
  ComponentType componentTypeInfo = componentTypes.getTypeInfo(typeId);
  if (has_io(componentTypeInfo.flags) && !typeIO)
    typeIO = g_entity_mgr->getComponentTypes().getTypeIO(typeId);
  ecsdebug::track_ecs_component_by_index(cidx, ecsdebug::TRACK_WRITE, "deserialize_init");
  const bool isPod = is_pod(componentTypeInfo.flags);
  const bool allocatedMem = ChildComponent::is_child_comp_boxed_by_size(componentTypeInfo.size);
  alignas(void *) char compData[ChildComponent::value_size]; // actually, should be sizeof(ChildComponent::Value), but it is protected
  void *tempData;
  if (allocatedMem)
      *(void**)compData = tempData = malloc(componentTypeInfo.size /*, tmpmem  TODO Allocators.*/);
  else
    tempData = compData;
  if (typeIO)
  {
    const bool isBoxed = (componentTypeInfo.flags & COMPONENT_TYPE_BOXED) != 0;
    ComponentTypeManager *ctm = NULL;
    if (need_constructor(componentTypeInfo.flags))
    {
      // yes, this const cast is ugly. It yet has to be here, there is no other way (besides explicit method in entity manager of
      // course).
      ctm = const_cast<ComponentTypes &>(componentTypes).createTypeManager(typeId);
      NAU_ASSERT(ctm, "type manager for type 0x{} ({}) missing", userType, typeId);
    }
    if (ctm)
      ctm->create(tempData, *g_entity_mgr.get(), INVALID_ENTITY_ID, ComponentsMap(), cidx);
    else if (!isPod)
      memset(tempData, 0, componentTypeInfo.size);
    if (typeIO->deserialize(deserializer, isBoxed ? *(void **)tempData : tempData, componentTypeInfo.size, userType))
      return ChildComponent(componentTypeInfo.size, typeId, userType, compData);
  }
  else if (isPod) // pod data can be just readed as-is
  {
    if (deserializer.read(tempData, componentTypeInfo.size * CHAR_BIT, userType))
      return ChildComponent(componentTypeInfo.size, typeId, userType, compData);
  }
  else
    logerr("Attempt to deserialize type 0x{} {}<{}>, which has no typeIO and not pod", userType, typeId,
      componentTypes.getTypeNameById(typeId));
  if (allocatedMem)
      free(tempData);/*Look 185. TODO Allocators.*/
  return MaybeChildComponent();
}

void serialize_child_component(const ChildComponent &comp, SerializerCb &serializer)
{
  component_type_t userType = comp.getUserType();
  serializer.write(&userType, sizeof(userType) * CHAR_BIT, 0);
  serialize_entity_component_ref_typeless(comp.getRawData(), ecs::INVALID_COMPONENT_INDEX, comp.getUserType(), comp.getTypeId(),
    serializer);
}

MaybeChildComponent deserialize_child_component(const DeserializerCb &deserializer)
{
  component_type_t userType = 0;
  if (deserializer.read(&userType, sizeof(userType) * CHAR_BIT, 0))
    return deserialize_init_component_typeless(userType, ecs::INVALID_COMPONENT_INDEX, deserializer);
  else
    return MaybeChildComponent();
}

} // namespace ecs
