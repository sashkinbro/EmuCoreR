// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: LicenseRef-EmuCoreR-Proprietary

#pragma once

#include <cstdint>
#include <string>

namespace emucorer::shader_chain {

// Stores the preset requested by the Kotlin layer. Backends compare the
// generation to know when to rebuild their librashader filter chain.
void SetPreset(std::string path, bool enabled);
bool IsEnabled();
std::string PresetPath();
uint64_t Generation();

}  // namespace emucorer::shader_chain
