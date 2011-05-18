// Copyright 2011 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "syzygy/relink/relinker.h"
#include <ctime>
#include "syzygy/pdb/pdb_util.h"
#include "syzygy/pe/decomposer.h"
#include "syzygy/pe/pe_data.h"
#include "syzygy/pe/pe_file_writer.h"

using core::BlockGraph;
using core::RelativeAddress;
using pe::Decomposer;
using pe::PEFileWriter;

namespace {

void AddOmapForBlockRange(
    const BlockGraph::AddressSpace::RangeMapConstIterPair& original,
    const BlockGraph::AddressSpace& remapped,
    std::vector<OMAP>* omap) {
  BlockGraph::AddressSpace::RangeMapConstIter it;

  for (it = original.first; it != original.second; ++it) {
    const BlockGraph::Block* block = it->second;
    DCHECK(block != NULL);

    RelativeAddress to_addr;
    if (remapped.GetAddressOf(block, &to_addr)) {
      OMAP entry = { it->first.start().value(), to_addr.value() };
      omap->push_back(entry);
    }
  }
}

void AddOmapForAllSections(size_t num_sections,
                           const IMAGE_SECTION_HEADER* sections,
                           const BlockGraph::AddressSpace& from,
                           const BlockGraph::AddressSpace& to,
                           std::vector<OMAP>* omap) {
  for (size_t i = 0; i < num_sections; ++i) {
    BlockGraph::AddressSpace::RangeMapConstIterPair range =
        from.GetIntersectingBlocks(RelativeAddress(sections[i].VirtualAddress),
                                   sections[i].Misc.VirtualSize);

    AddOmapForBlockRange(range, to, omap);
  }
}

}  // namespace

RelinkerBase::RelinkerBase(const BlockGraph::AddressSpace& original_addr_space,
                           BlockGraph* block_graph)
    : original_num_sections_(NULL),
      original_sections_(NULL),
      original_addr_space_(original_addr_space),
      builder_(block_graph) {
  DCHECK_EQ(block_graph, original_addr_space.graph());
}

RelinkerBase::~RelinkerBase() {
}

bool RelinkerBase::Initialize(const BlockGraph::Block* original_nt_headers) {
  // Retrieve the NT and image section headers.
  if (original_nt_headers == NULL ||
      original_nt_headers->size() < sizeof(IMAGE_NT_HEADERS) ||
      original_nt_headers->data_size() != original_nt_headers->size()) {
    LOG(ERROR) << "Missing or corrupt NT header in decomposed image.";
    return false;
  }
  const IMAGE_NT_HEADERS* nt_headers =
      reinterpret_cast<const IMAGE_NT_HEADERS*>(
          original_nt_headers->data());
  DCHECK(nt_headers != NULL);

  size_t num_sections = nt_headers->FileHeader.NumberOfSections;
  size_t nt_headers_size = sizeof(IMAGE_NT_HEADERS) +
      num_sections * sizeof(IMAGE_SECTION_HEADER);
  if (original_nt_headers->data_size() != nt_headers_size) {
    LOG(ERROR) << "Missing or corrupt image section headers "
        "in decomposed image.";
    return false;
  }

  // Grab the image characteristics, base and other properties from the
  // original image and propagate them to the new image headers.
  builder_.nt_headers().FileHeader.Characteristics =
      nt_headers->FileHeader.Characteristics;

  builder_.nt_headers().OptionalHeader.ImageBase =
      nt_headers->OptionalHeader.ImageBase;
  builder_.nt_headers().OptionalHeader.MajorOperatingSystemVersion =
      nt_headers->OptionalHeader.MajorOperatingSystemVersion;
  builder_.nt_headers().OptionalHeader.MinorOperatingSystemVersion =
      nt_headers->OptionalHeader.MinorOperatingSystemVersion;
  builder_.nt_headers().OptionalHeader.MajorImageVersion =
      nt_headers->OptionalHeader.MajorImageVersion;
  builder_.nt_headers().OptionalHeader.MinorImageVersion =
      nt_headers->OptionalHeader.MinorImageVersion;
  builder_.nt_headers().OptionalHeader.MajorSubsystemVersion =
      nt_headers->OptionalHeader.MajorSubsystemVersion;
  builder_.nt_headers().OptionalHeader.MinorSubsystemVersion =
      nt_headers->OptionalHeader.MinorSubsystemVersion;
  builder_.nt_headers().OptionalHeader.Win32VersionValue =
      nt_headers->OptionalHeader.Win32VersionValue;
  builder_.nt_headers().OptionalHeader.Subsystem =
      nt_headers->OptionalHeader.Subsystem;
  builder_.nt_headers().OptionalHeader.DllCharacteristics =
      nt_headers->OptionalHeader.DllCharacteristics;
  builder_.nt_headers().OptionalHeader.SizeOfStackReserve =
      nt_headers->OptionalHeader.SizeOfStackReserve;
  builder_.nt_headers().OptionalHeader.SizeOfStackCommit =
      nt_headers->OptionalHeader.SizeOfStackCommit;
  builder_.nt_headers().OptionalHeader.SizeOfHeapReserve =
      nt_headers->OptionalHeader.SizeOfHeapReserve;
  builder_.nt_headers().OptionalHeader.SizeOfHeapCommit =
      nt_headers->OptionalHeader.SizeOfHeapCommit;
  builder_.nt_headers().OptionalHeader.LoaderFlags =
      nt_headers->OptionalHeader.LoaderFlags;

  // Store the number of sections and the section headers in the original image.
  original_num_sections_ = num_sections;
  original_sections_ =
      reinterpret_cast<const IMAGE_SECTION_HEADER*>(nt_headers + 1);

  // Retrieve the original image's entry point.
  BlockGraph::Reference entry_point;
  size_t entrypoint_offset =
      FIELD_OFFSET(IMAGE_NT_HEADERS, OptionalHeader.AddressOfEntryPoint);
  if (!original_nt_headers->GetReference(entrypoint_offset, &entry_point)) {
    LOG(ERROR) << "Unable to get entrypoint.";
    return false;
  }
  builder_.set_entry_point(entry_point);

  return true;
}

bool RelinkerBase::CopyDataDirectory(
    const PEFileParser::PEHeader& original_header) {
  // Copy the data directory from the old image.
  for (size_t i = 0; i < arraysize(original_header.data_directory); ++i) {
    BlockGraph::Block* block = original_header.data_directory[i];

    // We don't want to copy the relocs entry over as the relocs are recreated.
    if (block != NULL && i != IMAGE_DIRECTORY_ENTRY_BASERELOC) {
      if (!builder_.SetDataDirectoryEntry(i, block)) {
        return false;
      }
    }
  }

  return true;
}

bool RelinkerBase::FinalizeImageHeaders(
    const PEFileParser::PEHeader& original_header) {
  if (!builder_.CreateRelocsSection())  {
    LOG(ERROR) << "Unable to create new relocations section";
    return false;
  }

  if (!builder_.FinalizeHeaders()) {
    LOG(ERROR) << "Unable to finalize header information";
    return false;
  }

  // Make sure everyone who previously referred the original
  // DOS header is redirected to the new one.
  if (!original_header.dos_header->TransferReferrers(0,
          builder_.dos_header_block())) {
    LOG(ERROR) << "Unable to redirect DOS header references.";
    return false;
  }

  // And ditto for the original NT headers.
  if (!original_header.nt_headers->TransferReferrers(0,
          builder_.nt_headers_block())) {
    LOG(ERROR) << "Unable to redirect NT headers references.";
    return false;
  }

  return true;
}

bool RelinkerBase::WriteImage(const FilePath& output_path) {
  PEFileWriter writer(builder_.address_space(),
                      &builder_.nt_headers(),
                      builder_.section_headers());

  if (!writer.WriteImage(output_path)) {
    LOG(ERROR) << "Unable to write new executable";
    return false;
  }

  return true;
}

bool RelinkerBase::CopySection(const IMAGE_SECTION_HEADER& section) {
  BlockGraph::AddressSpace::Range section_range(
      RelativeAddress(section.VirtualAddress), section.Misc.VirtualSize);
  const char* name = reinterpret_cast<const char*>(section.Name);
  std::string name_str(name, strnlen(name, arraysize(section.Name)));

  // Duplicate the section in the new image.
  RelativeAddress start = builder().AddSegment(name_str.c_str(),
                                               section.Misc.VirtualSize,
                                               section.SizeOfRawData,
                                               section.Characteristics);
  BlockGraph::AddressSpace::RangeMapConstIterPair section_blocks =
      original_addr_space().GetIntersectingBlocks(section_range.start(),
                                                  section_range.size());

  // Copy the blocks.
  if (!CopyBlocks(section_blocks, start)) {
    LOG(ERROR) << "Unable to copy blocks to new image";
    return false;
  }

  return true;
}

bool RelinkerBase::CopyBlocks(
    const AddressSpace::RangeMapConstIterPair& iter_pair,
    RelativeAddress insert_at) {
  AddressSpace::RangeMapConstIter it = iter_pair.first;
  const AddressSpace::RangeMapConstIter& end = iter_pair.second;
  for (; it != end; ++it) {
    BlockGraph::Block* block = it->second;
    if (!builder().address_space().InsertBlock(insert_at, block)) {
      LOG(ERROR) << "Failed to insert block '" << block->name() <<
          "' at " << insert_at;
      return false;
    }

    insert_at += block->size();
  }

  return true;
}

Relinker::Relinker(const BlockGraph::AddressSpace& original_addr_space,
                   BlockGraph* block_graph)
    : RelinkerBase(original_addr_space, block_graph) {
}

Relinker::~Relinker() {
}

bool Relinker::Relink(const PEFileParser::PEHeader& original_header,
                      const FilePath& input_pdb_path,
                      const FilePath& output_dll_path,
                      const FilePath& output_pdb_path) {
  DCHECK(!input_pdb_path.empty());
  DCHECK(!output_dll_path.empty());
  DCHECK(!output_pdb_path.empty());

  if (!Initialize(original_header.nt_headers)) {
    LOG(ERROR) << "Unable to initialize.";
    return false;
  }

  // Reorder code sections and copy non-code sections.
  for (size_t i = 0; i < original_num_sections() - 1; ++i) {
    const IMAGE_SECTION_HEADER& section = original_sections()[i];
    if (section.Characteristics & IMAGE_SCN_CNT_CODE) {
      if (!ReorderCode(section)) {
        LOG(ERROR) << "Unable to reorder code.";
      }
    } else {
      if (!CopySection(section)) {
        LOG(ERROR) << "Unable to copy section.";
        return false;
      }
    }
  }

  // Update the debug info and copy the data directory.
  if (!UpdateDebugInformation(
          original_header.data_directory[IMAGE_DIRECTORY_ENTRY_DEBUG])) {
    LOG(ERROR) << "Unable to update debug information.";
    return false;
  }
  if (!CopyDataDirectory(original_header)) {
    LOG(ERROR) << "Unable to copy the input image's data directory.";
    return false;
  }

  // Finalize the headers and write the image and pdb.
  if (!FinalizeImageHeaders(original_header)) {
    LOG(ERROR) << "Unable to finalize image headers.";
  }
  if (!WriteImage(output_dll_path)) {
    LOG(ERROR) << "Unable to write " << output_dll_path.value();
    return false;
  }
  if (!WritePDBFile(input_pdb_path, output_pdb_path)) {
    LOG(ERROR) << "Unable to write new PDB file.";
    return false;
  }

  return true;
}

bool Relinker::Initialize(const BlockGraph::Block* original_nt_headers) {
  if (!RelinkerBase::Initialize(original_nt_headers))
    return false;

  if (FAILED(::CoCreateGuid(&new_image_guid_))) {
    LOG(ERROR) << "Oh, no, we're fresh out of GUIDs! "
        "Quick, hand me an IPv6 address...";
    return false;
  }

  return true;
}

bool Relinker::UpdateDebugInformation(
    BlockGraph::Block* debug_directory_block) {
  // TODO(siggi): This is a bit of a hack, but in the interest of expediency
  //     we simply reallocate the data the existing debug directory references,
  //     and update the GUID and timestamp therein.
  //     It would be better to simply junk the debug info block, and replace it
  //     with a block that contains the new GUID, timestamp and PDB path.
  IMAGE_DEBUG_DIRECTORY debug_dir;
  if (debug_directory_block->data_size() != sizeof(debug_dir)) {
    LOG(ERROR) << "Debug directory is unexpected size.";
    return false;
  }
  memcpy(&debug_dir, debug_directory_block->data(), sizeof(debug_dir));
  if (debug_dir.Type != IMAGE_DEBUG_TYPE_CODEVIEW) {
    LOG(ERROR) << "Debug directory with unexpected type.";
    return false;
  }

  // Update the timestamp.
  debug_dir.TimeDateStamp = static_cast<uint32>(time(NULL));
  if (debug_directory_block->CopyData(sizeof(debug_dir), &debug_dir) == NULL) {
    LOG(ERROR) << "Unable to copy debug directory data";
    return false;
  }

  // Now get the contents.
  BlockGraph::Reference ref;
  if (!debug_directory_block->GetReference(
          FIELD_OFFSET(IMAGE_DEBUG_DIRECTORY, AddressOfRawData), &ref) ||
      ref.offset() != 0 ||
      ref.referenced()->size() < sizeof(pe::CvInfoPdb70)) {
    LOG(ERROR) << "Unexpected or no data in debug directory.";
    return false;
  }

  BlockGraph::Block* debug_info_block = ref.referenced();
  DCHECK(debug_info_block != NULL);

  // Copy the debug info data.
  pe::CvInfoPdb70* debug_info =
      reinterpret_cast<pe::CvInfoPdb70*>(
          debug_info_block->CopyData(debug_info_block->data_size(),
                                     debug_info_block->data()));

  if (debug_info == NULL) {
    LOG(ERROR) << "Unable to copy debug info";
    return false;
  }

  // Stash the new GUID.
  debug_info->signature = new_image_guid_;

  return true;
}

bool Relinker::WritePDBFile(const FilePath& input_path,
                            const FilePath& output_path) {
  // Generate the map data for both directions.
  std::vector<OMAP> omap_to;
  AddOmapForAllSections(builder().nt_headers().FileHeader.NumberOfSections - 1,
                        builder().section_headers(),
                        builder().address_space(),
                        original_addr_space(),
                        &omap_to);

  std::vector<OMAP> omap_from;
  AddOmapForAllSections(original_num_sections() - 1,
                        original_sections(),
                        original_addr_space(),
                        builder().address_space(),
                        &omap_from);

  if (!pdb::AddOmapStreamToPdbFile(input_path,
                                   output_path,
                                   new_image_guid_,
                                   omap_to,
                                   omap_from)) {
    LOG(ERROR) << "Unable to add OMAP data to PDB";
    return false;
  }

  return true;
}