#include "platform/xbox360/xbox360_network_runtime.hpp"

#define NONET
#include <xtl.h>
#include <winsockx.h>

namespace xbox360 {
namespace {
bool xnetStarted = false;
bool wsaStarted = false;
}

bool initNetwork() {
    if (networkInitialized()) return true;

    XNetStartupParams params{};
    params.cfgSizeOfStruct = sizeof(params);
    params.cfgFlags = XNET_STARTUP_BYPASS_SECURITY;
    if (XNetStartup(&params) != 0) return false;
    xnetStarted = true;

    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        XNetCleanup();
        xnetStarted = false;
        return false;
    }
    wsaStarted = true;
    return true;
}

void shutdownNetwork() {
    if (wsaStarted) {
        WSACleanup();
        wsaStarted = false;
    }
    if (xnetStarted) {
        XNetCleanup();
        xnetStarted = false;
    }
}

bool networkInitialized() {
    return xnetStarted && wsaStarted;
}

}  // namespace xbox360
