// Copyright 2024 N-GINN LLC. All rights reserved.
// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved

#ifndef RENDER_LIGHTS_INCLUDED
#define RENDER_LIGHTS_INCLUDED 1

#include "renderLightsConsts.hlsli"

struct RenderOmniLight
{
  float4 posRadius;
  float4 colorFlags;
  float4 direction__tex_scale;
  float4 boxR0;
  float4 boxR1;
  float4 boxR2;
  float4 posRelToOrigin_cullRadius;
};

struct RenderSpotLight
{
  float4 lightPosRadius;
  float4 lightColorAngleScale; //AngleScale sign bit contains contact_shadow bit
  float4 lightDirectionAngleOffset;
  float4 texId_scale;
};

struct SpotlightShadowDescriptor
{
  float2 decodeDepth;
  float meterToUvAtZfar;
  float hasDynamic; //bool
  float4 uvMinMax;
};

//Implemented here, to avoid needing to pass these two values in a buffer
//inline needed to avoid duplicate symbol link error in c++
inline float2 get_light_shadow_zn_zf(float radius)
{
  return float2(0.001 * radius, radius);
}
#endif