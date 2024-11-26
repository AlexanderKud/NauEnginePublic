// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.

#pragma once

#include "usd_translator/usd_translator_api.h"
#include "usd_translator/usd_prim_adapter.h"

#include "nau/math/math.h"
#include "nau/assets/animation_asset_accessor.h"
#include "nau/animation/components/animation_component.h"
#include "nau/animation/playback/animation_instance.h"

#include <pxr/usd/usdGeom/primvarsAPI.h>


namespace UsdTranslator
{
    class USD_TRANSLATOR_API AnimationClipComposer
    {
    public:
        AnimationClipComposer(const pxr::UsdPrim& prim);

        const eastl::vector<nau::AnimationDataDescriptor>& getAnimationDataDescriptors() const noexcept;
        nau::animation::PlayMode getPlayMode() const noexcept;

        template <typename T>
        void getTrackData(const nau::AnimationDataDescriptor& descriptor, eastl::vector<float>& times, eastl::vector<T>& data) const
        {
            const auto pred = [&descriptor](const nau::AnimationDataDescriptor& desc) { return descriptor == desc; };
            if (std::none_of(m_descriptors.begin(), m_descriptors.end(), pred))
            {
                return;
            }
            times = m_trackList[descriptor.animationIndex].first;
            if constexpr (std::is_same_v<T, nau::math::quat>)
            {
                data = std::move(convertToQuatVec(descriptor.animationIndex));
            } else
            {
                data = m_trackList[descriptor.animationIndex].second;
            }
        }

    private:
        void loadData(const nau::AnimationDataDescriptor& descriptor, const pxr::UsdPrim& trackPrim);
        eastl::vector<nau::math::quat> convertToQuatVec(size_t animationIndex) const;

    private:
        using TransformTrackPair = std::pair<eastl::vector<float>, eastl::vector<nau::math::vec3>>;

        nau::animation::PlayMode m_playMode{};
        eastl::vector<nau::AnimationDataDescriptor> m_descriptors;
        eastl::vector<TransformTrackPair> m_trackList;
    };
}