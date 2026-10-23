// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

std::string sha256_hex(const std::uint8_t* data, std::size_t len);
