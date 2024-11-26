// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#include "nau/3d/dag_nvLowLatency.h"
#include "nvLowLatency.h"

void nvlowlatency::init(void * /*device*/, bool /*deferred_render_submission*/) {}

void nvlowlatency::close() {}

bool nvlowlatency::is_available() { return false; }

void nvlowlatency::set_latency_mode(lowlatency::LatencyMode /*mode*/, float /*min_interval_ms*/) {}

void nvlowlatency::sleep() {}

void nvlowlatency::set_marker(uint32_t /*frame_id*/, lowlatency::LatencyMarkerType /*marker_type*/) {}

lowlatency::LatencyData nvlowlatency::get_statistics_since(uint32_t /*frame_id*/, uint32_t /*max_count*/) { return {}; }

bool nvlowlatency::feed_latency_input(uint32_t /*frame_id*/, unsigned int /*msg*/) { return false; }
