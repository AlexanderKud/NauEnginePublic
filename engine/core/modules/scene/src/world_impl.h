// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "nau/scene/world.h"
#include "nau/scene/scene_object.h"

namespace nau::scene
{
    class WorldImpl final : public IWorld
    {
        NAU_OBJECT(nau::scene::WorldImpl, IWorld)

    public:
        WorldImpl();

        eastl::string_view getName() const override;

        void setName(eastl::string_view name) override;

        Vector<IScene::WeakRef> getScenes() const override;

        async::Task<IScene::WeakRef> addScene(IScene::Ptr&& scene) override;

        void removeScene(IScene::WeakRef sceneRef) override;

        void setSimulationPause(bool pause) override;

        bool isSimulationPaused() const override;

    private:

        class SceneManagerImpl& getSceneManager() const;

        eastl::string m_name;
        bool m_isPaused = false;
    };

}  // namespace nau
