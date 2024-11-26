// Copyright 2024 N-GINN LLC. All rights reserved.

// Copyright (C) 2024  Gaijin Games KFT.  All rights reserved
#pragma once

// forward declarations for external classes
struct TexImage32;
class IGenLoad;


// load TIFF file as 8bit 4channel image (RGBX/RGBA)
TexImage32 *load_tiff32(const char *fn, eastl::allocator *mem, bool *out_used_alpha = NULL);
TexImage32 *load_tiff32(IGenLoad &crd, eastl::allocator *mem, bool *out_used_alpha = NULL);

// save TIFF file as 8bit 4(3)channel image (RGBA/RGB)
bool save_tiff32(const char *fn, TexImage32 *, unsigned char *app_data = 0, unsigned int app_data_len = 0);
bool save_tiff24(const char *fn, TexImage32 *, unsigned char *app_data = 0, unsigned int app_data_len = 0);
