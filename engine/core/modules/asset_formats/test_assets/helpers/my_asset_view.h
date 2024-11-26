// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "nau/assets/asset_view.h"
#include "nau/rtti/rtti_impl.h"

namespace nau::test
{
    /**
     */
    class MyAssetView final : public IAssetView
    {
        NAU_CLASS_(MyAssetView, IAssetView)

    public:
        MyAssetView(eastl::string data) :
            m_data(std::move(data))
        {
        }

        eastl::string getData() const
        {
            return m_data;
        }

    private:
        eastl::string m_data;
    };
}  // namespace nau::test