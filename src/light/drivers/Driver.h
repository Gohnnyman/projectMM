#pragma once

// Umbrella header for writing a driver: include this one file and you have the
// base class plus the output-buffer, correction, and platform pieces a driver
// needs to push the finished image to hardware or the network. A new driver is
//
//   #pragma once
//   #include "light/drivers/Driver.h"
//   namespace mm {
//   class MyDriver : public DriverBase { ... };
//   }
//
// DriverBase already bundles Layer, Buffer, Correction, and the platform layer
// (a driver reads the source buffer and applies the output correction), so this
// umbrella is DriverBase plus the small standard headers every driver's status /
// control-name / buffer handling uses. A hardware driver adds its peripheral seam
// (platform::rmt* / platform::parlio* / a socket); a network driver adds its packet
// header — those stay per-driver, since they differ by transport.

#include "light/drivers/DriverBase.h"   // DriverBase + Layer + Buffer + Correction + platform

#include <cstring>                      // std::strcmp (onControlChanged) / memset (buffer clears)
#include <cstdint>                      // fixed-width ints
#include <cstdio>                       // std::snprintf for status strings
#include <algorithm>                    // std::min / max / clamp (chunk loops, size clamps)
