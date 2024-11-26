// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

#include <daECS/utility/enum_registration.h>

namespace ecs
{
struct EnumLoaderDesc
{
  enum_parse_t parse = nullptr;
  enum_names_t getNames = nullptr;
  update_enum_t updateValue = nullptr;
  find_enum_idx_t findEnum = nullptr;
  component_type_t compType = 0;
};

const EnumLoaderDesc *find_enum_description(eastl::string_view tp_name);

bool is_type_ecs_enum(const char *type_name);

nau::ConstSpan<const char *> get_ecs_enum_values(const char *type_name);
void update_enum_value(const char *type_name, ecs::EntityComponentRef &enum_component, int enum_idx);
int find_enum_idx(const char *type_name, const ecs::EntityComponentRef &enum_component);
} // namespace ecs
