// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "../button_state_base.h"

#include <EASTL/unique_ptr.h>

namespace nau::ui
{
    class DisableComboState : public ButtonStateBase
    {
    public:
        bool initialize(nau::ui::NauButton* button, nau::ui::NauButtonData& data) override;
        void enter(nau::ui::NauButton* button) override;
        void handleEvent(nau::ui::NauButton* button, nau::ui::EventType eventType) override;
        void update(nau::ui::NauButton* button) override;
        void exit(nau::ui::NauButton* button) override;

    private:
        std::vector<eastl::unique_ptr<ButtonStateBase>> m_includedStates;
    };
} // namespace nau::ui
