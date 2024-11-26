// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include "nau/ui/button.h"
#include "../button_state_base.h"

namespace nau::ui
{
    class NormalStateColor : public ButtonStateBase
    {
    public:
        bool initialize(nau::ui::NauButton* button, nau::ui::NauButtonData& data) override;
        void enter(nau::ui::NauButton* button) override;
        void handleEvent(nau::ui::NauButton* button, nau::ui::EventType eventType) override;
        void update(nau::ui::NauButton* button) override;
        void exit(nau::ui::NauButton* button) override;
    };
} // namespace nau::ui
