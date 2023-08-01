#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "distorm.h"

namespace fs = std::filesystem;

enum Result {
	SUCCESSFUL,
	FAILED,
	ALREADY_TRANSFORMED
};

int InstructionLength(unsigned char* addr)
{
	_DecodedInst inst;
	unsigned int a = 0;
	distorm_decode(0, addr, 0xFF, Decode64Bits, &inst, 1, &a);
	return inst.size;
}

Result MutateNextCall(std::byte* array, std::uintptr_t offset)
{
	unsigned char* addr = reinterpret_cast<unsigned char*>(array + offset);
	std::deque<unsigned char*> history;
	while (true) {
		if (addr[0] == 0x48 && addr[1] == 0x8b && addr[2] == 0x05 && addr[3] == 0x00 && addr[4] == 0x00 && addr[5] == 0x00 && addr[6] == 0x00) {
			// We found the mov 0x0(%rip),%rax

			if (history.size() < 2) {
				__asm("int3");
				return FAILED;
			}

			unsigned char* callInstruction = nullptr;
			size_t depth = 0;
			while (!callInstruction || !((callInstruction[0] == 0x41 && callInstruction[1] == 0xff) || callInstruction[0] == 0xff)) {
				if (depth > 2) {
					__asm("int3");
					return FAILED; // The call instruction shouldn't be that far away
				}
				callInstruction = history.front();
				history.pop_front();
				depth++;
			}

			unsigned char* firstNop = callInstruction;

			while (*firstNop != 0x90) {
				firstNop += InstructionLength(firstNop);
			}

			std::memcpy(firstNop, callInstruction, InstructionLength(callInstruction));
			*(firstNop + InstructionLength(firstNop) - 1) += 0x10;

			size_t offset = 0;

			bool secondDeref = callInstruction[InstructionLength(callInstruction) + 8] == 0x8b;

			unsigned char* pushSequence = callInstruction;
			while (true) {
				unsigned int a = 0;
				_DecodedInst inst;
				distorm_decode(0, pushSequence, 0xFF, Decode64Bits, &inst, 1, &a);
				if (std::strcmp("MOV", reinterpret_cast<const char*>(&inst.mnemonic.p[0])) == 0)
					break;
				pushSequence += inst.size;
			}

			if (*callInstruction != 0x41 && *(callInstruction + 1) == 0xd0) {

				pushSequence[0] = 0x4c;
				pushSequence[1] = 0x8b;
				pushSequence[2] = 0x15;

				if (secondDeref) {
					pushSequence[7] = 0x4d;
					pushSequence[8] = 0x8b;
					pushSequence[9] = 0x12;
				}

				size_t i = secondDeref ? 10 : 7;

				pushSequence[i] = 0x41;
				pushSequence[i + 1] = 0x52;
				offset++;
			}

			std::memset(callInstruction, 0x90, pushSequence - callInstruction);

			return SUCCESSFUL;
		}

		history.push_front(addr);
		addr += InstructionLength(addr);
		if (*addr == 0x90) {
			return ALREADY_TRANSFORMED;
		}
	}
	return FAILED; // ?
}

void ProcessObjectFile(const fs::path& file_path)
{
	static const std::string objdumpLine = ".text._ZN14RetAddrSpooferL6Invoke";

	std::string path = fs::absolute(file_path).string();
	std::cout << "Processing " << path << std::endl;

	FILE* objdump = popen(("objdump -h " + path).c_str(), "r");

	std::vector<std::uintptr_t> offsets;

	char buffer[4096];
	std::string line{};
	while (!feof(objdump)) {
		if (fgets(buffer, 4096, objdump) != nullptr) {
			line += buffer;

			char& last = line[line.size() - 1];

			if (last != '\n' && last != '\0') // Is there more?
				continue;

			struct A {
				std::string& line;
				~A() { line.clear(); } // poor mans defer
			} a{ line };

			if (line.empty())
				continue;

			if (line.find(objdumpLine) == std::string::npos) // Find the name of the function
				continue;

			auto offset = line.substr(line.find_first_not_of(' ')); // This is way more complicated than it should be:
			for (size_t i = 0; i < 5; i++) {
				offset = offset.substr(offset.find(' '));
				offset = offset.substr(offset.find_first_not_of(' '));
			}
			offset = offset.substr(0, offset.find(' '));
			offset = offset.substr(offset.find_first_not_of('0'));

			offsets.push_back(std::stoll(offset, nullptr, 16));
		}
	}

	pclose(objdump);

	std::fstream fs{ file_path, std::ios::in | std::ios::binary | std::ios::out };
	auto size = fs::file_size(file_path);
	std::byte array[size];
	fs.read(reinterpret_cast<char*>(array), size);

	size_t successful = 0;
	size_t failed = 0;
	size_t skipped = 0;

	for (auto offset : offsets) {
		switch (MutateNextCall(array, offset)) {
		case SUCCESSFUL: {
			std::cout << "Successfully mutated the function at " << std::hex << offset << std::dec << std::endl;
			successful++;
			break;
		}
		case FAILED: {
			std::cout << "Failed to mutate the function at " << std::hex << offset << std::dec << std::endl;
			failed++;
			break;
		}
		case ALREADY_TRANSFORMED: {
			std::cout << "Skipped the function at " << std::hex << offset << std::dec << ", because it was already mutated" << std::endl;
			skipped++;
			break;
		}
		}
	}

	std::cout << "Processed " << offsets.size() << " calls (" << successful << " successful, " << failed << " failed, " << skipped << " skipped)" << std::endl;

	assert(failed == 0);

	fs.seekg(0, std::ios::beg);
	fs.write(reinterpret_cast<char*>(array), size);
	fs.close();
}

void IterateFolder(std::string path)
{
	for (const auto& entry : fs::directory_iterator(path)) {
		const fs::path& file_path = entry.path();
		if (fs::is_regular_file(file_path) && file_path.extension() == ".o") {
			ProcessObjectFile(file_path);
		} else if (fs::is_directory(file_path)) {
			IterateFolder(file_path); // Recursively process subdirectories
		}
	}
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		std::cout << "Usage: ObjectFileRewriter <input folder>" << std::endl;
		return 1;
	}

	IterateFolder(argv[1]);
	return 0;
}