#pragma once

// First-boot WiFi setup: opens an access point with a captive web page for
// entering the network credentials. Verifies them, stores them in NVS and
// restarts. Never returns.
namespace Portal {

[[noreturn]] void run();

}  // namespace Portal
