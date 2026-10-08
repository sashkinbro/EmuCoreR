// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <string>

namespace emucorer::shader_chain {

// Stores the preset requested by the Kotlin layer. Backends compare the
// generation to know when to rebuild their librashader filter chain.
void SetPreset(std::string path, bool enabled);
// Recreate temporal resources on the next graphics frame after a discontinuity.
void ResetHistory();
bool IsEnabled();
std::string PresetPath();
uint64_t Generation();

}  // namespace emucorer::shader_chain
