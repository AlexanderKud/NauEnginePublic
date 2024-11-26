// Copyright 2024 N-GINN LLC. All rights reserved.
// Use of this source code is governed by a BSD-3 Clause license that can be found in the LICENSE file.


#pragma once

#include <nau/core_defines.h>

#ifdef Yield
#undef Yield
#endif

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-copy"
#endif

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <gtest/gtest-param-test.h>


#ifdef __clang__
#pragma clang diagnostic pop
#endif
