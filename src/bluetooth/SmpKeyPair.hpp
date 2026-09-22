/**
 * Copyright (C) 2026 MK124 and contributors
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include "BleBytes.hpp"

namespace m5emulator::bluetooth {

class SmpKeyPair
{
	struct Impl;
	
public:
	SmpKeyPair();
	~SmpKeyPair();
	
	SmpKeyPair(const SmpKeyPair&) = delete;
	SmpKeyPair& operator=(const SmpKeyPair&) = delete;
	
	std::array<uint8_t, 64> publicKey() const;
	std::array<uint8_t, 32> sharedSecret(ByteView peerPublicKey) const;
	
private:
	std::unique_ptr<Impl> _impl;
};

} // namespace m5emulator::bluetooth
