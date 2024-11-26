// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "nau/assets/asset_view_factory.h"


namespace nau::physics::jolt
{
    class JoltAssetFactory final : public IAssetViewFactory
    {
        NAU_RTTI_CLASS(nau::physics::jolt::JoltAssetFactory, IAssetViewFactory)
        
        eastl::vector<const rtti::TypeInfo*> getAssetViewTypes() const override;
        async::Task<IAssetView::Ptr> createAssetView(nau::Ptr<> accessor, const rtti::TypeInfo& viewType) override;
    };
}