#pragma once

#ifdef RECOMP_FFL_TRIAL
#include <gx2/texture.h>

#include "mii_select.h"

namespace wiiu::mii_face {
bool prepare(uint32_t layout, const mii_select::Request::Entry *const *entries, int count);
const GX2Texture *texture(int cell);
void release();
}
#endif
