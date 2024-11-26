// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once
#include "nau/diag/error.h"

namespace nau
{
    /**
     */
    class OperationCancelledError : public DefaultError<>
    {
        NAU_ERROR(nau::OperationCancelledError, DefaultError<>)
    public:
        OperationCancelledError(const diag::SourceInfo& sourceInfo, eastl::string_view description = "Operation was cancelled") :
            DefaultError<>(sourceInfo, eastl::string{description})
        {
        }
    };
}  // namespace nau
