#pragma once

#include <cstdint>

namespace mm {

// What a single output channel of a light carries. A light is a run of channels, and this
// names the role of each one — the colour roles a strip/panel needs (Red/Green/Blue/White)
// plus the fixture roles a moving head adds (Pan/Tilt/…). It is the shared vocabulary two
// sides use: Correction describes a light's wiring as an array of these (roles[i] = what
// channel i is), and the effect-side role writers (setRGB/setPan/…) name the role they drive.
// One enum, referenced everywhere, so the wiring description and the writers can't drift.
//
// None marks a channel that carries no role we drive (a spacer, or a fixture channel set
// elsewhere). The colour roles come first so a plain RGB(W) light only ever uses the low
// values; the fixture roles extend the list without disturbing them.
//
// APPEND-ONLY: a persisted preset stores each channel's role as this enum's byte value, so a role
// must NEVER be renumbered — a new one is appended within its group (colour roles before the
// fixture roles) and existing values keep their index. White = a normal/cold white; WarmWhite is
// the second white a CCT fixture adds. Yellow/UV are the extra par-can colours (a 6-channel
// RGBWYP lightbar). Extending here is safe because the fixture roles below carry no persisted
// data yet (no seeded built-in uses them, no effect writes them), so shifting them is a no-op; a
// future persisted fixture role would force new roles to the very end instead.
enum class ChannelRole : uint8_t {
    None,
    Red, Green, Blue, White, WarmWhite, Yellow, UV,  // colour roles — strips/panels/PARs (White = cold)
    Pan, Tilt, Zoom, Rotate, Gobo, Dimmer,           // fixture roles — moving heads
};

// Option strings for a Select bound to a ChannelRole, index-aligned with the enum values so a
// control's uint8 casts straight to ChannelRole. Kept beside the enum (one home) so a new role
// adds its name here and nowhere else.
inline constexpr const char* kChannelRoleOptions[] = {
    "—", "R", "G", "B", "W", "WW", "Y", "UV", "Pan", "Tilt", "Zoom", "Rotate", "Gobo", "Dimmer",
};
inline constexpr uint8_t kChannelRoleCount =
    sizeof(kChannelRoleOptions) / sizeof(kChannelRoleOptions[0]);

// The option array is index-aligned with the enum (a control's uint8 casts straight to a
// ChannelRole and indexes this array), so their lengths MUST match. Dimmer is the last enum value,
// so its index + 1 is the enum count; a missed string (or a missed enum entry) breaks the build
// here rather than silently mis-labelling a role at runtime. Update both together.
static_assert(kChannelRoleCount == static_cast<uint8_t>(ChannelRole::Dimmer) + 1,
              "kChannelRoleOptions must have one string per ChannelRole value (index-aligned)");

} // namespace mm
