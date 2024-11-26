// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once
#include "render_list.h"
#include "instance_group.h"

namespace nau
{

    class IRenderManager : public IRefCounted
    {
        NAU_CLASS_(nau::IRenderManager, IRefCounted);

    public:
        virtual void update() = 0;
        virtual RenderList::Ptr getRenderList(const nau::math::Vector3& viewerPosition,
            eastl::function<bool(const InstanceInfo&)>& filterFunc,
            eastl::function<bool(const nau::MaterialAssetView::Ptr)>& materialFilter) = 0;
    };

} // namespace nau

