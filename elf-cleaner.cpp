/* termux-elf-cleaner

Copyright (C) 2017 Fredrik Fornwall
Copyright (C) 2019-2022 Termux

This file is part of termux-elf-cleaner.

termux-elf-cleaner is free software: you can redistribute it and/or
modify it under the terms of the GNU General Public License as
published by the Free Software Foundation, either version 3 of the
License, or (at your option) any later version.

termux-elf-cleaner is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with termux-elf-cleaner.  If not, see
<https://www.gnu.org/licenses/>.  */

#include <algorithm>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef __ANDROID_API__
#define __ANDROID_API__ 21
#endif

// Include a local elf.h copy as not all platforms have it.
#include "elf.h"

#define DT_GNU_HASH 0x6ffffef5
#define DT_VERSYM 0x6ffffff0
#define DT_FLAGS_1 0x6ffffffb
#define DT_VERNEEDED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff

#define DT_AARCH64_BTI_PLT 0x70000001
#define DT_AARCH64_PAC_PLT 0x70000003
#define DT_AARCH64_VARIANT_PCS 0x70000005

#define DF_1_NOW	0x00000001	/* Set RTLD_NOW for this object.  */
#define DF_1_GLOBAL	0x00000002	/* Set RTLD_GLOBAL for this object.  */
#define DF_1_NODELETE	0x00000008	/* Set RTLD_NODELETE for this object.*/

#if __ANDROID_API__ < 23
#define SUPPORTED_DT_FLAGS_1 (DF_1_NOW | DF_1_GLOBAL)
#else
// The supported DT_FLAGS_1 values as of Android 6.0.
#define SUPPORTED_DT_FLAGS_1 (DF_1_NOW | DF_1_GLOBAL | DF_1_NODELETE)
#endif

template<typename ElfHeaderType /*Elf{32,64}_Ehdr*/,
	 typename ElfSectionHeaderType /*Elf{32,64}_Shdr*/,
	 typename ElfDynamicSectionEntryType /* Elf{32,64}_Dyn */>
bool process_elf(uint8_t* bytes, size_t elf_file_size, char const* file_name)
{
	if (sizeof(ElfSectionHeaderType) > elf_file_size) {
		fprintf(stderr, "elf-cleaner: Elf header for '%s' would end at %zu but file size only %zu\n",
			file_name, sizeof(ElfSectionHeaderType), elf_file_size);
		return false;
	}
	ElfHeaderType* elf_hdr = reinterpret_cast<ElfHeaderType*>(bytes);

	size_t last_section_header_byte = elf_hdr->e_shoff + sizeof(ElfSectionHeaderType) * elf_hdr->e_shnum;
	if (last_section_header_byte > elf_file_size) {
		fprintf(stderr, "elf-cleaner: Section header for '%s' would end at %zu but file size only %zu\n",
			file_name, last_section_header_byte, elf_file_size);
		return false;
	}
	ElfSectionHeaderType* section_header_table = reinterpret_cast<ElfSectionHeaderType*>(bytes + elf_hdr->e_shoff);

	bool is_aarch64 = (elf_hdr->e_machine == 183); /* EM_AARCH64 */

	for (unsigned int i = 1; i < elf_hdr->e_shnum; i++) {
		ElfSectionHeaderType* section_header_entry = section_header_table + i;
		if (section_header_entry->sh_type == SHT_DYNAMIC) {
			size_t const last_dynamic_section_byte = section_header_entry->sh_offset + section_header_entry->sh_size;
			if (last_dynamic_section_byte > elf_file_size) {
				fprintf(stderr, "elf-cleaner: Dynamic section for '%s' would end at %zu but file size only %zu\n",
					file_name, last_dynamic_section_byte, elf_file_size);
				return false;
			}

			size_t const dynamic_section_entries = section_header_entry->sh_size / sizeof(ElfDynamicSectionEntryType);
			ElfDynamicSectionEntryType* const dynamic_section =
				reinterpret_cast<ElfDynamicSectionEntryType*>(bytes + section_header_entry->sh_offset);

			unsigned int last_nonnull_entry_idx = 0;
			for (unsigned int j = dynamic_section_entries - 1; j > 0; j--) {
				ElfDynamicSectionEntryType* dynamic_section_entry = dynamic_section + j;
				if (dynamic_section_entry->d_tag != DT_NULL) {
					last_nonnull_entry_idx = j;
					break;
				}
			}

			for (unsigned int j = 0; j < dynamic_section_entries; j++) {
				ElfDynamicSectionEntryType* dynamic_section_entry = dynamic_section + j;
				char const* removed_name = nullptr;
				switch (dynamic_section_entry->d_tag) {
#if __ANDROID_API__ <= 21
					case DT_GNU_HASH: removed_name = "DT_GNU_HASH"; break;
#endif
#if __ANDROID_API__ < 23
					case DT_VERSYM: removed_name = "DT_VERSYM"; break;
					case DT_VERNEEDED: removed_name = "DT_VERNEEDED"; break;
					case DT_VERNEEDNUM: removed_name = "DT_VERNEEDNUM"; break;
					case DT_VERDEF: removed_name = "DT_VERDEF"; break;
					case DT_VERDEFNUM: removed_name = "DT_VERDEFNUM"; break;
#endif
					case DT_RPATH: removed_name = "DT_RPATH"; break;
#if __ANDROID_API__ < 24
					case DT_RUNPATH: removed_name = "DT_RUNPATH"; break;
#endif
#if __ANDROID_API__ < 31
					case DT_AARCH64_BTI_PLT: if(is_aarch64) removed_name = "DT_AARCH64_BTI_PLT"; break;
					case DT_AARCH64_PAC_PLT: if(is_aarch64) removed_name = "DT_AARCH64_PAC_PLT"; break;
					case DT_AARCH64_VARIANT_PCS: if(is_aarch64) removed_name = "DT_AARCH64_VARIANT_PCS"; break;
#endif
				}
				if (removed_name != nullptr) {
					printf("elf-cleaner: Removing the %s dynamic section entry from '%s'\n",
					       removed_name, file_name);
					// Tag the entry with DT_NULL and put it last:
					dynamic_section_entry->d_tag = DT_NULL;
					// Decrease j to process new entry index:
					std::swap(dynamic_section[j--], dynamic_section[last_nonnull_entry_idx--]);
				} else if (dynamic_section_entry->d_tag == DT_FLAGS_1) {
					// Remove unsupported DF_1_* flags to avoid linker warnings.
					decltype(dynamic_section_entry->d_un.d_val) orig_d_val =
						dynamic_section_entry->d_un.d_val;
					decltype(dynamic_section_entry->d_un.d_val) new_d_val =
						(orig_d_val & SUPPORTED_DT_FLAGS_1);
					if (new_d_val != orig_d_val) {
						printf("elf-cleaner: Replacing unsupported DF_1_* flags %llu with %llu in '%s'\n",
						       (unsigned long long) orig_d_val,
						       (unsigned long long) new_d_val,
						       file_name);
						dynamic_section_entry->d_un.d_val = new_d_val;
					}
				}
			}
		}
#if __ANDROID_API__ < 23
		else if (section_header_entry->sh_type == SHT_GNU_verdef ||
			   section_header_entry->sh_type == SHT_GNU_verneed ||
			   section_header_entry->sh_type == SHT_GNU_versym) {
			printf("elf-cleaner: Removing version section from '%s'\n", file_name);
			section_header_entry->sh_type = SHT_NULL;
		}
#endif
	}
	return true;
}


int main(int argc, char const** argv)
{
	if (argc < 2 || (argc == 2 && strcmp(argv[1], "-h")==0)) {
		fprintf(stderr, "usage: %s <filenames>\n", argv[0]);
		fprintf(stderr, "\nProcesses ELF files to remove unsupported section types\n"
				"and dynamic section entries which the Android linker (API %d)\nwarns about.\n",
				__ANDROID_API__);
		return 1;
	}

	for (int i = 1; i < argc; i++) {
		char const* file_name = argv[i];
		int fd = open(file_name, O_RDWR);
		if (fd < 0) {
			char* error_message;
			if (asprintf(&error_message, "open(\"%s\")", file_name) == -1)
				error_message = (char*) "open()";
			perror(error_message);
			return 1;
		}

		struct stat st;
		if (fstat(fd, &st) < 0) { perror("fstat()"); return 1; }

		if (st.st_size < (long long) sizeof(Elf32_Ehdr)) {
			close(fd);
			continue;
		}

		void* mem = mmap(0, st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
		if (mem == MAP_FAILED) { perror("mmap()"); return 1; }

		uint8_t* bytes = reinterpret_cast<uint8_t*>(mem);
		if (!(bytes[0] == 0x7F && bytes[1] == 'E' && bytes[2] == 'L' && bytes[3] == 'F')) {
			// Not the ELF magic number.
			munmap(mem, st.st_size);
			close(fd);
			continue;
		}

		if (bytes[/*EI_DATA*/5] != 1) {
			fprintf(stderr, "elf-cleaner: Not little endianness in '%s'\n", file_name);
			munmap(mem, st.st_size);
			close(fd);
			continue;
		}

		uint8_t const bit_value = bytes[/*EI_CLASS*/4];
		if (bit_value == 1) {
			if (!process_elf<Elf32_Ehdr, Elf32_Shdr, Elf32_Dyn>(bytes, st.st_size, file_name))
				return 1;
		} else if (bit_value == 2) {
			if (!process_elf<Elf64_Ehdr, Elf64_Shdr, Elf64_Dyn>(bytes, st.st_size, file_name))
				return 1;
		} else {
			fprintf(stderr, "elf-cleaner: Incorrect bit value %d in '%s'\n", bit_value, file_name);
			return 1;
		}

		if (msync(mem, st.st_size, MS_SYNC) < 0) {
			perror("msync()");
			return 1;
		}

		munmap(mem, st.st_size);
		close(fd);
	}
	return 0;
}
