/* Copyright 2021 Aristocratos (jakob@qvantnet.com)

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

	   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.

   Modified for topo (fork of btop for Apple Silicon), 2026: see NOTICE.
   Lettura delle chiavi SMC numeriche (flt e altri tipi) ed elenco delle chiavi
   come in macmon (MIT, Copyright (c) 2024 vladkens).

indent = tab
tab-size = 4
*/

#include "smc.hpp"
#include "apple_soc.hpp"

#include <span>

static constexpr size_t MaxIndexCount = sizeof("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ") - 1;
static constexpr const char *KeyIndexes = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static UInt32 _strtoul(char *str, int size, int base) {
	UInt32 total = 0;
	int i;

	for (i = 0; i < size; i++) {
		if (base == 16) {
			total += str[i] << (size - 1 - i) * 8;
		} else {
			total += (unsigned char)(str[i] << (size - 1 - i) * 8);
		}
	}
	return total;
}

static void _ultostr(char *str, UInt32 val) {
	str[0] = '\0';
	snprintf(str, 5, "%c%c%c%c",
			(unsigned int)val >> 24,
			(unsigned int)val >> 16,
			(unsigned int)val >> 8,
			(unsigned int)val);
}

namespace Cpu {

	SMCConnection::SMCConnection() {
		CFMutableDictionaryRef matchingDictionary = IOServiceMatching("AppleSMC");
		result = IOServiceGetMatchingServices(0, matchingDictionary, &iterator);
		if (result != kIOReturnSuccess) {
			throw std::runtime_error("failed to get AppleSMC");
		}

		//? topo: su Apple Silicon le chiavi stanno nel servizio "AppleSMCKeysEndpoint"; altrimenti il primo
		device = 0;
		io_object_t candidate;
		while ((candidate = IOIteratorNext(iterator)) != 0) {
			io_name_t name;
			bool keys_endpoint = IORegistryEntryGetName(candidate, name) == kIOReturnSuccess
				and strcmp(name, "AppleSMCKeysEndpoint") == 0;
			if (device == 0 or keys_endpoint) {
				if (device != 0) IOObjectRelease(device);
				device = candidate;
				if (keys_endpoint) break;
			} else {
				IOObjectRelease(candidate);
			}
		}
		IOObjectRelease(iterator);
		if (device == 0) {
			throw std::runtime_error("failed to get SMC device");
		}

		result = IOServiceOpen(device, mach_task_self(), 0, &conn);
		IOObjectRelease(device);
		if (result != kIOReturnSuccess) {
			throw std::runtime_error("failed to get SMC connection");
		}
	}
	SMCConnection::~SMCConnection() {
		IOServiceClose(conn);
	}

	long long SMCConnection::getSMCTemp(char *key) {
		SMCVal_t val;
		kern_return_t result;
		result = SMCReadKey(key, &val);
		if (result == kIOReturnSuccess) {
			if (val.dataSize > 0) {
				if (strcmp(val.dataType, DATATYPE_SP78) == 0) {
					// convert sp78 value to temperature
					int intValue = val.bytes[0] * 256 + (unsigned char)val.bytes[1];
					return static_cast<long long>(intValue / 256.0);
				}
			}
		}
		return -1;
	}

	// core means physical core in SMC, while in core map it's cpu threads :-/ Only an issue on hackintosh?
	// this means we can only get the T per physical core
	// another issue with the SMC API is that the key is always 4 chars -> what with systems with more than 9 physical cores?
	// no Mac models with more than 18 threads are released, so no problem so far
	// according to VirtualSMC docs (hackintosh fake SMC) the enumeration follows with alphabetic chars - not implemented yet here (nor in VirtualSMC)
	long long SMCConnection::getTemp(int core) {
		char key[] = SMC_KEY_CPU_TEMP;
		if (core >= 0) {
			if ((size_t)core > MaxIndexCount) {
				return -1;
			}
			snprintf(key, 5, "TC%1cc", KeyIndexes[core]);
		}
		long long result = getSMCTemp(key);
		//? topo: con core == -1 (sensore di package) non c'e' un indice da riprovare: KeyIndexes[-1] era fuori limiti
		if (result == -1 and core >= 0) {
			// try again with C
			snprintf(key, 5, "TC%1cC", KeyIndexes[core]);
			result = getSMCTemp(key);
		}
		return result;
	}

	bool SMCConnection::readKeyInfo(UInt32 key, SMCKeyData_keyInfo_t& info) {
		if (auto it = key_info_cache.find(key); it != key_info_cache.end()) {
			info = it->second;
			return true;
		}
		SMCKeyData_t in, out;
		memset(&in, 0, sizeof in);
		memset(&out, 0, sizeof out);
		in.key = key;
		in.data8 = SMC_CMD_READ_KEYINFO;
		if (SMCCall(KERNEL_INDEX_SMC, &in, &out) != kIOReturnSuccess or out.result != 0) return false;
		info = out.keyInfo;
		key_info_cache[key] = info;
		return true;
	}

	std::optional<std::string> SMCConnection::keyType(const std::string& key) {
		if (key.size() != 4) return std::nullopt;
		char k[5] = {key[0], key[1], key[2], key[3], 0};
		SMCKeyData_keyInfo_t info;
		if (not readKeyInfo(_strtoul(k, 4, 16), info)) return std::nullopt;
		char type[5];
		_ultostr(type, info.dataType);
		return std::string(type, 4);
	}

	std::optional<double> SMCConnection::readValue(const std::string& key) {
		if (key.size() != 4) return std::nullopt;
		char k[5] = {key[0], key[1], key[2], key[3], 0};
		const UInt32 id = _strtoul(k, 4, 16);
		SMCKeyData_keyInfo_t info;
		if (not readKeyInfo(id, info)) return std::nullopt;
		SMCKeyData_t in, out;
		memset(&in, 0, sizeof in);
		memset(&out, 0, sizeof out);
		in.key = id;
		in.keyInfo.dataSize = info.dataSize;
		in.data8 = SMC_CMD_READ_BYTES;
		if (SMCCall(KERNEL_INDEX_SMC, &in, &out) != kIOReturnSuccess or out.result != 0) return std::nullopt;
		char type[5];
		_ultostr(type, info.dataType);
		const size_t size = std::min<size_t>(info.dataSize, sizeof(out.bytes));
		return AppleSoc::Pure::decode_smc(std::string_view(type, 4),
			std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(out.bytes), size));
	}

	std::vector<std::string> SMCConnection::listKeys() {
		std::vector<std::string> keys;
		auto count = readValue("#KEY");
		if (not count or *count <= 0) return keys;
		const auto n = static_cast<UInt32>(*count);
		keys.reserve(n);
		for (UInt32 i = 0; i < n; i++) {
			SMCKeyData_t in, out;
			memset(&in, 0, sizeof in);
			memset(&out, 0, sizeof out);
			in.data8 = SMC_CMD_READ_INDEX;
			in.data32 = i;
			if (SMCCall(KERNEL_INDEX_SMC, &in, &out) != kIOReturnSuccess or out.result != 0) continue;
			char key[5];
			_ultostr(key, out.key);
			keys.emplace_back(key, 4);
		}
		return keys;
	}

	kern_return_t SMCConnection::SMCReadKey(UInt32Char_t key, SMCVal_t *val) {
		kern_return_t result;
		SMCKeyData_t inputStructure;
		SMCKeyData_t outputStructure;

		memset(&inputStructure, 0, sizeof(SMCKeyData_t));
		memset(&outputStructure, 0, sizeof(SMCKeyData_t));
		memset(val, 0, sizeof(SMCVal_t));

		inputStructure.key = _strtoul(key, 4, 16);
		inputStructure.data8 = SMC_CMD_READ_KEYINFO;

		result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
		if (result != kIOReturnSuccess)
			return result;

		val->dataSize = outputStructure.keyInfo.dataSize;
		_ultostr(val->dataType, outputStructure.keyInfo.dataType);
		inputStructure.keyInfo.dataSize = val->dataSize;
		inputStructure.data8 = SMC_CMD_READ_BYTES;

		result = SMCCall(KERNEL_INDEX_SMC, &inputStructure, &outputStructure);
		if (result != kIOReturnSuccess)
			return result;

		memcpy(val->bytes, outputStructure.bytes, sizeof(outputStructure.bytes));

		return kIOReturnSuccess;
	}

	kern_return_t SMCConnection::SMCCall(int index, SMCKeyData_t *inputStructure, SMCKeyData_t *outputStructure) {
		size_t structureInputSize;
		size_t structureOutputSize;

		structureInputSize = sizeof(SMCKeyData_t);
		structureOutputSize = sizeof(SMCKeyData_t);

		return IOConnectCallStructMethod(conn, index,
										 // inputStructure
										 inputStructure, structureInputSize,
										 // outputStructure
										 outputStructure, &structureOutputSize);
	}

}  // namespace Cpu
