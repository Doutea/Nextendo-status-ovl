#pragma once
#include "relay_route.hpp"
#if __has_include("relay_embedded.generated.hpp")
#include "relay_embedded.generated.hpp"
#endif
#ifndef NEXTENDO_EMBEDDED_RELAY_CONFIG
#define NEXTENDO_EMBEDDED_RELAY_CONFIG ""
#endif
namespace nextendo {
inline RelayRoute embeddedRelayRoute() {
    return parseRelayRoute(NEXTENDO_EMBEDDED_RELAY_CONFIG);
}
}
