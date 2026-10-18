#pragma once

namespace xbox360 {

// Initializes the platform networking stack in the order required by XDK:
// XNet first, then Winsock. Returns false and leaves the stack stopped on error.
bool initNetwork();

// Stops Winsock and XNet after all network users have finished.
void shutdownNetwork();

bool networkInitialized();

}  // namespace xbox360
