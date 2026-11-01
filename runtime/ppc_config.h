#pragma once

// The runtime itself is not a recompiled image: it only needs ppc_context.h's
// types. Indirect calls go through the shared table from ppc_overrides.h.
#define PPC_CONFIG_H_INCLUDED
