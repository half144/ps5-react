// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

namespace hui::ps5 {
class Pad;
}

// The pad that host::set_light_bar/reset_light_bar/vibrate drive. The host sets
// it after Pad::open() and clears it (nullptr) before Pad::close().
void host_platform_set_pad(hui::ps5::Pad* pad);
