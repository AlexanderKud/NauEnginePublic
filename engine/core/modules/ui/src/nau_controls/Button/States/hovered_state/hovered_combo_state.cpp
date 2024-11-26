// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#include "hovered_combo_state.h"
#include "hovered_state.h"
#include "hovered_state_color.h"
#include "hovered_state_size.h"
#include "../button_state_animation.h"

namespace nau::ui
{

    bool HoveredComboState::initialize(nau::ui::NauButton* button, nau::ui::NauButtonData& data)
    {
        auto state = eastl::make_unique<HoveredState>();
        if (state && state->initialize(button, data))
        {
            m_includedStates.push_back(std::move(state));
        }

        if (data.hoveredAnimation)
        {
            auto stateAnimation = eastl::make_unique<ButtonStateHoveredAnimation>();
            if (stateAnimation->initialize(button, data))
            {
                m_includedStates.push_back(std::move(stateAnimation));
            }
        }

        auto stateColor = eastl::make_unique<HoveredStateColor>();
        if (stateColor && stateColor->initialize(button, data))
        {
            m_includedStates.push_back(std::move(stateColor));
        }

        auto stateSize = eastl::make_unique<HoveredStateSize>();
        if (stateSize && stateSize->initialize(button, data))
        {
            m_includedStates.push_back(std::move(stateSize));
        }

        return true;
    }

    void HoveredComboState::enter(nau::ui::NauButton* button)
    {
        for (auto& state : m_includedStates)
        {
            state->enter(button);
        }
    }

    void HoveredComboState::handleEvent(nau::ui::NauButton* button, nau::ui::EventType eventType)
    {
        for (auto& state : m_includedStates)
        {
            state->handleEvent(button, eventType);
        }
    }

    void HoveredComboState::update(nau::ui::NauButton* button)
    {
        for (auto& state : m_includedStates)
        {
            state->update(button);
        }
    }

    void HoveredComboState::exit(nau::ui::NauButton* button)
    {
        for (auto& state : m_includedStates)
        {
            state->exit(button);
        }
    }

} // namespace nau::ui