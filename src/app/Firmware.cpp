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

#include "Firmware.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <vector>

#include <SDL.h>

#ifdef __APPLE__
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif

namespace m5emulator {

static std::vector<uint8_t> readBinary(const std::filesystem::path& path, std::size_t maximumSize)
{
	if (!std::filesystem::is_regular_file(path)) throw std::runtime_error("BIN must be a regular file: " + path.string());
	const std::uintmax_t size = std::filesystem::file_size(path);
	if (size == 0 || size > maximumSize) throw std::runtime_error("BIN is empty or exceeds the available Flash space: " + path.string());
	std::vector<uint8_t> bytes(size);
	std::ifstream input(path, std::ios::binary);
	if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) throw std::runtime_error("Cannot read BIN: " + path.string());
	return bytes;
}

static std::array<uint8_t, 32> sha256(std::span<const uint8_t> bytes)
{
	std::array<uint8_t, 32> digest {};
	#ifdef __APPLE__
	CC_SHA256(bytes.data(), static_cast<CC_LONG>(bytes.size()), digest.data());
	#else
	unsigned int length = 0;
	if (EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) != 1 || length != digest.size())
	{
		throw std::runtime_error("Cannot compute Flash SHA-256");
	}
	#endif
	return digest;
}

static uint32_t readUint32(std::span<const uint8_t, 4> bytes)
{
	return uint32_t(bytes[0]) | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}

static bool isApplication(std::span<const uint8_t> bytes)
{
	// ESP-IDF puts esp_app_desc_t at the start of the first image segment.
	return bytes.size() >= 36 && readUint32(bytes.subspan<32, 4>()) == 0xABCD5432;
}

static void validateImage(std::span<const uint8_t> bytes, uint16_t chipId)
{
	if (bytes.size() < 24 || bytes[0] != 0xE9 || (bytes[12] | (uint16_t(bytes[13]) << 8)) != chipId)
	{
		throw std::runtime_error("BIN must contain an ESP image for the selected device with magic 0xE9");
	}
	if (bytes[1] == 0 || bytes[1] > 16 || bytes[23] > 1) throw std::runtime_error("Invalid ESP image header");
	std::size_t offset = 24;
	uint8_t checksum = 0xEF;
	for (unsigned int i = 0; i < bytes[1]; ++i)
	{
		if (bytes.size() - offset < 8) throw std::runtime_error("Truncated ESP image segment header");
		const uint32_t size = readUint32(bytes.subspan(offset + 4).first<4>());
		offset += 8;
		if (size > bytes.size() - offset) throw std::runtime_error("Truncated ESP image segment");
		for (const uint8_t byte : bytes.subspan(offset, size)) checksum ^= byte;
		offset += size;
	}
	
	const std::size_t checksumOffset = offset | 15;
	if (checksumOffset >= bytes.size()) throw std::runtime_error("Truncated ESP image checksum");
	if (bytes[checksumOffset] != checksum) throw std::runtime_error("ESP image checksum mismatch");
	if (bytes[23])
	{
		const std::size_t hashOffset = checksumOffset + 1;
		const auto digest = sha256(bytes.first(hashOffset));
		if (bytes.size() - hashOffset < digest.size()) throw std::runtime_error("Truncated ESP image SHA-256");
		if (!std::equal(digest.begin(), digest.end(), bytes.begin() + hashOffset)) throw std::runtime_error("ESP image SHA-256 mismatch");
	}
}

static std::vector<uint8_t> readFirmware(const std::filesystem::path& path, const FlashLayout& layout)
{
	auto bytes = readBinary(path, layout.size);
	if (isApplication(bytes))
	{
		if (bytes.size() > layout.applicationSize) throw std::runtime_error("Application BIN exceeds the selected device application partition");
	}
	else if (bytes.size() != layout.size)
	{
		throw std::runtime_error("BIN must be an ESP-IDF application or a complete Flash image for the selected device");
	}
	validateImage(std::span(bytes).subspan(isApplication(bytes) ? 0 : layout.bootloaderOffset), layout.chipId);
	return bytes;
}

void validateFirmware(const std::filesystem::path& path, const FlashLayout& layout) { readFirmware(path, layout); }

void validateFlash(const std::filesystem::path& path, const FlashLayout& layout)
{
	if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) != layout.size)
	{
		throw std::runtime_error("Flash size does not match the selected device: " + path.string());
	}
	
	std::ifstream input(path, std::ios::binary);
	if (!input) throw std::runtime_error("Cannot read flash: " + path.string());
	input.seekg(layout.bootloaderOffset);
	if (input.get() != 0xE9) throw std::runtime_error("Flash has no ESP boot image at the selected device bootloader offset");
}

void writeFlash(const std::filesystem::path& source, const std::filesystem::path& destination, const FlashLayout& layout)
{
	auto bytes = readFirmware(source, layout);
	if (isApplication(bytes))
	{
		const std::unique_ptr<char, decltype(&SDL_free)> directory(SDL_GetBasePath(), SDL_free);
		if (!directory) throw std::runtime_error("Cannot locate boot resources: " + std::string(SDL_GetError()));
		const auto bootloader = readBinary(std::filesystem::path(directory.get()) / layout.bootloader, layout.partitionOffset - layout.bootloaderOffset);
		validateImage(bootloader, layout.chipId);
		
		std::vector<uint8_t> flash(layout.size, 0xFF);
		std::copy(bootloader.begin(), bootloader.end(), flash.begin() + layout.bootloaderOffset);
		std::copy(layout.partitions.begin(), layout.partitions.end(), flash.begin() + layout.partitionOffset);
		std::copy(bytes.begin(), bytes.end(), flash.begin() + layout.applicationOffset);
		bytes.swap(flash);
	}
	
	std::ofstream output(destination, std::ios::binary | std::ios::trunc);
	output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
	output.close();
	if (!output) throw std::runtime_error("Cannot write working Flash: " + destination.string());
}

std::string flashDigest(const std::filesystem::path& path, const FlashLayout& layout)
{
	const auto digest = sha256(readFirmware(path, layout));
	static constexpr char HexDigits[] = "0123456789abcdef";
	std::string result;
	result.reserve(digest.size() * 2);
	for (const uint8_t byte : digest)
	{
		result += HexDigits[byte >> 4];
		result += HexDigits[byte & 0x0F];
	}
	return result;
}

} // namespace m5emulator
