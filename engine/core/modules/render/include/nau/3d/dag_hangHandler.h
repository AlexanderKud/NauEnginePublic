// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

namespace d3dhang
{

using GPUHanger = void (*)();

void register_gpu_hanger(GPUHanger newHanger);
void hang_gpu_on(const char *event);
void hang_if_requested(const char *event);

} // namespace d3dhang
