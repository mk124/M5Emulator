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

#include "../SmpKeyPair.hpp"

#include <algorithm>
#include <stdexcept>

#include <Security/SecItem.h>
#include <Security/SecKey.h>

namespace m5emulator::bluetooth {

static CFDictionaryRef keyAttributes(CFStringRef keyClass)
{
	const int bits = 256;
	CFNumberRef size = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
	const void* keys[] { kSecAttrKeyType, kSecAttrKeySizeInBits, kSecAttrKeyClass };
	const void* values[] { kSecAttrKeyTypeECSECPrimeRandom, size, keyClass };
	CFDictionaryRef result = CFDictionaryCreate(nullptr, keys, values, 3, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(size);
	return result;
}

struct SmpKeyPair::Impl
{
	SecKeyRef key = nullptr;
	
	~Impl() { if (key != nullptr) CFRelease(key); }
};

SmpKeyPair::SmpKeyPair() : _impl(std::make_unique<Impl>())
{
	CFDictionaryRef attributes = keyAttributes(kSecAttrKeyClassPrivate);
	_impl->key = SecKeyCreateRandomKey(attributes, nullptr);
	CFRelease(attributes);
	require(_impl->key != nullptr, "cannot generate SMP P-256 key");
}

SmpKeyPair::~SmpKeyPair() = default;

std::array<uint8_t, 64> SmpKeyPair::publicKey() const
{
	SecKeyRef publicKey = SecKeyCopyPublicKey(_impl->key);
	require(publicKey != nullptr, "cannot export SMP public key");
	
	CFDataRef encoded = SecKeyCopyExternalRepresentation(publicKey, nullptr);
	CFRelease(publicKey);
	require(encoded != nullptr, "cannot encode SMP public key");
	if (CFDataGetLength(encoded) != 65 || CFDataGetBytePtr(encoded)[0] != 4)
	{
		CFRelease(encoded);
		throw std::runtime_error("unexpected P-256 public key encoding");
	}
	
	std::array<uint8_t, 64> result;
	const uint8_t* bytes = CFDataGetBytePtr(encoded) + 1;
	std::reverse_copy(bytes, bytes + 32, result.begin());
	std::reverse_copy(bytes + 32, bytes + 64, result.begin() + 32);
	CFRelease(encoded);
	return result;
}

std::array<uint8_t, 32> SmpKeyPair::sharedSecret(ByteView peerPublicKey) const
{
	require(peerPublicKey.size() == 64, "invalid SMP public key length");
	
	Bytes external { 4 };
	appendReverse(external, peerPublicKey.first(32));
	appendReverse(external, peerPublicKey.last(32));
	CFDataRef encoded = CFDataCreate(nullptr, external.data(), external.size());
	CFDictionaryRef attributes = keyAttributes(kSecAttrKeyClassPublic);
	SecKeyRef peer = SecKeyCreateWithData(encoded, attributes, nullptr);
	CFRelease(attributes);
	CFRelease(encoded);
	require(peer != nullptr, "invalid SMP P-256 point");
	
	CFDictionaryRef parameters = CFDictionaryCreate(nullptr, nullptr, nullptr, 0, nullptr, nullptr);
	CFDataRef secret = SecKeyCopyKeyExchangeResult(_impl->key, kSecKeyAlgorithmECDHKeyExchangeStandard, peer, parameters, nullptr);
	CFRelease(parameters);
	CFRelease(peer);
	require(secret != nullptr, "SMP ECDH failed");
	if (CFDataGetLength(secret) != 32)
	{
		CFRelease(secret);
		throw std::runtime_error("unexpected SMP ECDH length");
	}
	
	std::array<uint8_t, 32> result;
	std::reverse_copy(CFDataGetBytePtr(secret), CFDataGetBytePtr(secret) + 32, result.begin());
	CFRelease(secret);
	return result;
}

} // namespace m5emulator::bluetooth
