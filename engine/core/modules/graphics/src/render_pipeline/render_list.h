// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <EASTL/vector.h>

#include "nau/3d/dag_drv3d.h"
#include "nau/math/dag_frustum.h"
#include "graphics_assets/material_asset.h"
#include "render_entity.h"

namespace nau
{
    class RenderList
    {
    public:

        using Ptr = eastl::shared_ptr<RenderList>;

        RenderList() = default;
        RenderList(eastl::vector<RenderList::Ptr>&& vec);

        inline eastl::vector<RenderEntity>& getEntities()
        {
            return m_entities;
        }

        inline RenderEntity& emplaceBack()
        {
            return m_entities.emplace_back();
        }

        inline void emplaceBack(RenderEntity&& entity)
        {
            m_entities.emplace_back(std::move(entity));
        }

        inline void pushBack(const RenderEntity& entity)
        {
            m_entities.push_back(entity);
        }

        inline RenderEntity& operator[](int ind)
        {
            return m_entities[ind];
        }

        inline uint32_t getEntitiesCount() const
        {
            return m_entities.size();
        }

    protected:
        eastl::vector<RenderEntity> m_entities;
    };

} // namespace nau
