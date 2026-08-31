#pragma once

#include "core/auth/authentication.h"

namespace pi::auth {

// TODO(taxonomy-phase-10): remove. AuthResolver is the migration-era name;
// all new code must use the Authentication aggregate.
using AuthResolver = Authentication;

} // namespace pi::auth
