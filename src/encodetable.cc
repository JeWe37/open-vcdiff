// Copyright 2008 The open-vcdiff Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <config.h>
#include <limits.h>  // UCHAR_MAX
#include <string>
#include "addrcache.h"
#include "codetable.h"
#include "google/encodetable.h"
#include "instruction_map.h"
#include "logging.h"
#include "google/output_string.h"
#include "varint_bigendian.h"
#include "vcdiff_defs.h"

namespace open_vcdiff {

static const DeltaFileHeader kHeaderStandardFormat = {
    0xD6,  // Header1: "V" | 0x80
    0xC3,  // Header2: "C" | 0x80
    0xC4,  // Header3: "D" | 0x80
    0x00,  // Header4: Draft standard format
    0x00 };  // Hdr_Indicator:
             // No compression, no custom code table

static const DeltaFileHeader kHeaderExtendedFormat = {
    0xD6,  // Header1: "V" | 0x80
    0xC3,  // Header2: "C" | 0x80
    0xC4,  // Header3: "D" | 0x80
    'S',   // Header4: VCDIFF/SDCH, extensions used
    0x00 };  // Hdr_Indicator:
             // No compression, no custom code table

// VCDiffCodeTableWriter members and methods

// If interleaved is true, the encoder writes each delta file window
// by interleaving instructions and sizes with their corresponding
// addresses and data, rather than placing these elements into three
// separate sections.  This facilitates providing partially
// decoded results when only a portion of a delta file window
// is received (e.g. when HTTP over TCP is used as the
// transmission protocol.)  The interleaved format is
// not consistent with the VCDIFF draft standard.
//
VCDiffCodeTableWriter::VCDiffCodeTableWriter(bool interleaved)
    : max_mode_(VCDiffAddressCache::DefaultLastMode()),
      dictionary_size_(0),
      target_length_(0),
      code_table_data_(&VCDiffCodeTableData::kDefaultCodeTableData),
      instruction_map_(NULL),
      last_opcode_index_(-1),
      add_checksum_(false),
      checksum_(0) {
  InitSectionPointers(interleaved);
}

VCDiffCodeTableWriter::VCDiffCodeTableWriter(
    bool interleaved,
    unsigned char near_cache_size,
    unsigned char same_cache_size,
    const VCDiffCodeTableData& code_table_data,
    unsigned char max_mode)
    : max_mode_(max_mode),
      address_cache_(near_cache_size, same_cache_size),
      dictionary_size_(0),
      target_length_(0),
      code_table_data_(&code_table_data),
      instruction_map_(NULL),
      last_opcode_index_(-1),
      add_checksum_(false),
      checksum_(0)  {
  InitSectionPointers(interleaved);
}

VCDiffCodeTableWriter::~VCDiffCodeTableWriter() {
  if (code_table_data_ != &VCDiffCodeTableData::kDefaultCodeTableData) {
    delete instruction_map_;
  }
}

void VCDiffCodeTableWriter::InitSectionPointers(bool interleaved) {
  if (interleaved) {
    data_for_add_and_run_ = &instructions_and_sizes_;
    addresses_for_copy_ = &instructions_and_sizes_;
  } else {
    data_for_add_and_run_ = &separate_data_for_add_and_run_;
    addresses_for_copy_ = &separate_addresses_for_copy_;
  }
}

bool VCDiffCodeTableWriter::Init(size_t dictionary_size) {
  dictionary_size_ = dictionary_size;
  if (!instruction_map_) {
    if (code_table_data_ == &VCDiffCodeTableData::kDefaultCodeTableData) {
      instruction_map_ = VCDiffInstructionMap::GetDefaultInstructionMap();
    } else {
      instruction_map_ = new VCDiffInstructionMap(*code_table_data_, max_mode_);
    }
    if (!instruction_map_) {
      return false;
    }
  }
  if (!address_cache_.Init()) {
    return false;
  }
  target_length_ = 0;
  last_opcode_index_ = -1;
  return true;
}

void VCDiffCodeTableWriter::WriteHeader(
    OutputStringInterface* out,
    VCDiffFormatExtensionFlags format_extensions) {
  if (format_extensions == VCD_STANDARD_FORMAT) {
    out->append(reinterpret_cast<const char*>(&kHeaderStandardFormat),
                sizeof(kHeaderStandardFormat));
  } else {
    out->append(reinterpret_cast<const char*>(&kHeaderExtendedFormat),
                sizeof(kHeaderExtendedFormat));
  }
  // If custom cache table sizes or a custom code table were used
  // for encoding, here is where they would be appended to *output.
  // This implementation of the encoder does not use those features,
  // although the decoder can understand and interpret them.
}

// The VCDiff format allows each opcode to represent either
// one or two delta instructions.  This function will first
// examine the opcode generated by the last call to EncodeInstruction.
// If that opcode was a single-instruction opcode, this function checks
// whether there is a compound (double-instruction) opcode that can
// combine that single instruction with the instruction that is now
// being added, and so save a byte of space.  In that case, the
// single-instruction opcode at position last_opcode_index_ will be
// overwritten with the new double-instruction opcode.
//
// In the majority of cases, no compound opcode will be possible,
// and a new single-instruction opcode will be appended to
// instructions_and_sizes_, followed by a representation of its size
// if the opcode does not implicitly give the instruction size.
//
// As an example, say instructions_and_sizes_ contains 10 bytes, the last
// of which contains the opcode 0x02 (ADD size 1).  Because that was the
// most recently added opcode, last_opcode_index_ has the value 10.
// EncodeInstruction is then called with inst = VCD_COPY, size = 4, mode = 0.
// The function will replace the old opcode 0x02 with the double-instruction
// opcode 0xA3 (ADD size 1 + COPY size 4 mode 0).
//
// All of the double-instruction opcodes in the standard code table
// have implicit sizes, meaning that the size of the instruction will not
// need to be written to the instructions_and_sizes_ string separately
// from the opcode.  If a custom code table were used that did not have
// this property, then instructions_and_sizes_ might contain a
// double-instruction opcode (say, COPY size 0 mode 0 + ADD size 0)
// followed by the size of the COPY, then by the size of the ADD.
// If using the SDCH interleaved format, the address of the COPY instruction
// would follow its size, so the ordering would be
// [Compound Opcode][Size of COPY][Address of COPY][Size of ADD]
//
void VCDiffCodeTableWriter::EncodeInstruction(VCDiffInstructionType inst,
                                              size_t size,
                                              unsigned char mode) {
  if (!instruction_map_) {
    VCD_DFATAL << "EncodeInstruction() called without calling Init()"
               << VCD_ENDL;
    return;
  }
  if (last_opcode_index_ >= 0) {
    const unsigned char last_opcode =
        instructions_and_sizes_[last_opcode_index_];
    // The encoding engine should not generate two ADD instructions in a row.
    // This won't cause a failure, but it's inefficient encoding and probably
    // represents a bug in the higher-level logic of the encoder.
    if ((inst == VCD_ADD) &&
        (code_table_data_->inst1[last_opcode] == VCD_ADD)) {
      VCD_WARNING << "EncodeInstruction() called for two ADD instructions"
                     " in a row" << VCD_ENDL;
    }
    OpcodeOrNone compound_opcode = kNoOpcode;
    if (size <= UCHAR_MAX) {
      compound_opcode = instruction_map_->LookupSecondOpcode(
          last_opcode, static_cast<unsigned char>(inst),
          static_cast<unsigned char>(size), mode);
      if (compound_opcode != kNoOpcode) {
        instructions_and_sizes_[last_opcode_index_] =
            static_cast<unsigned char>(compound_opcode);
        last_opcode_index_ = -1;
        return;
      }
    }
    // Try finding a compound opcode with size 0.
    compound_opcode = instruction_map_->LookupSecondOpcode(
        last_opcode, static_cast<unsigned char>(inst), 0, mode);
    if (compound_opcode != kNoOpcode) {
      instructions_and_sizes_[last_opcode_index_] =
          static_cast<unsigned char>(compound_opcode);
      last_opcode_index_ = -1;
      AppendSizeToString(size, &instructions_and_sizes_);
      return;
    }
  }
  OpcodeOrNone opcode = kNoOpcode;
  if (size <= UCHAR_MAX) {
    opcode = instruction_map_->LookupFirstOpcode(
        static_cast<unsigned char>(inst), static_cast<unsigned char>(size),
        mode);
    if (opcode != kNoOpcode) {
      instructions_and_sizes_.push_back(static_cast<char>(opcode));
      last_opcode_index_ = static_cast<int>(instructions_and_sizes_.size() - 1);
      return;
    }
  }
  // There should always be an opcode with size 0.
  opcode = instruction_map_->LookupFirstOpcode(
      static_cast<unsigned char>(inst), 0, mode);
  if (opcode == kNoOpcode) {
    VCD_DFATAL << "No matching opcode found for inst " << inst
               << ", mode " << mode << ", size 0" << VCD_ENDL;
    return;
  }
  instructions_and_sizes_.push_back(static_cast<char>(opcode));
  last_opcode_index_ = static_cast<int>(instructions_and_sizes_.size() - 1);
  AppendSizeToString(size, &instructions_and_sizes_);
}

void VCDiffCodeTableWriter::Add(const char* data, size_t size) {
  EncodeInstruction(VCD_ADD, size);
  data_for_add_and_run_->append(data, size);
  target_length_ += size;
}

void VCDiffCodeTableWriter::Copy(VCDAddress offset, size_t size) {
  if (!instruction_map_) {
    VCD_DFATAL << "VCDiffCodeTableWriter::Copy() called without calling Init()"
               << VCD_ENDL;
    return;
  }
  // If a single interleaved stream of encoded values is used
  // instead of separate sections for instructions, addresses, and data,
  // then the string instructions_and_sizes_ may be the same as
  // addresses_for_copy_.  The address should therefore be encoded
  // *after* the instruction and its size.
  VCDAddress encoded_addr = 0;
  const unsigned char mode = address_cache_.EncodeAddress(
      offset,
      static_cast<VCDAddress>(dictionary_size_ + target_length_),
      &encoded_addr);
  EncodeInstruction(VCD_COPY, size, mode);
  if (address_cache_.WriteAddressAsVarintForMode(mode)) {
    VarintBE<int32_t>::AppendToString(encoded_addr, addresses_for_copy_);
  } else {
    addresses_for_copy_->push_back(static_cast<unsigned char>(encoded_addr));
  }
  target_length_ += size;
}

void VCDiffCodeTableWriter::Run(size_t size, unsigned char byte) {
  EncodeInstruction(VCD_RUN, size);
  data_for_add_and_run_->push_back(byte);
  target_length_ += size;
}

size_t VCDiffCodeTableWriter::CalculateLengthOfSizeAsVarint(size_t size) {
  return VarintBE<int64_t>::Length(static_cast<int64_t>(size));
}

void VCDiffCodeTableWriter::AppendSizeToString(size_t size, string* out) {
  VarintBE<int64_t>::AppendToString(static_cast<int64_t>(size), out);
}

void VCDiffCodeTableWriter::AppendSizeToOutputString(
    size_t size,
    OutputStringInterface* out) {
  VarintBE<int64_t>::AppendToOutputString(size, out);
}

// This calculation must match the items added between "Start of Delta Encoding"
// and "End of Delta Encoding" in Output(), below.
size_t VCDiffCodeTableWriter::CalculateLengthOfTheDeltaEncoding() const {
  size_t length_of_the_delta_encoding =
    CalculateLengthOfSizeAsVarint(target_length_) +
    1 +  // Delta_Indicator
    CalculateLengthOfSizeAsVarint(separate_data_for_add_and_run_.size()) +
    CalculateLengthOfSizeAsVarint(instructions_and_sizes_.size()) +
    CalculateLengthOfSizeAsVarint(separate_addresses_for_copy_.size()) +
    separate_data_for_add_and_run_.size() +
    instructions_and_sizes_.size() +
    separate_addresses_for_copy_.size();
  if (add_checksum_) {
    length_of_the_delta_encoding +=
        VarintBE<int64_t>::Length(static_cast<int64_t>(checksum_));
  }
  return length_of_the_delta_encoding;
}

void VCDiffCodeTableWriter::Output(OutputStringInterface* out) {
  if (instructions_and_sizes_.empty()) {
    VCD_WARNING << "Empty input; no delta window produced" << VCD_ENDL;
  } else {
    const size_t length_of_the_delta_encoding =
        CalculateLengthOfTheDeltaEncoding();
    const size_t delta_window_size =
        length_of_the_delta_encoding +
        1 +  // Win_Indicator
        CalculateLengthOfSizeAsVarint(dictionary_size_) +
        CalculateLengthOfSizeAsVarint(0) +
        CalculateLengthOfSizeAsVarint(length_of_the_delta_encoding);
    // append() will be called many times on the output string; make sure
    // the output string is resized only once at most.
    out->ReserveAdditionalBytes(delta_window_size);

    // Add first element: Win_Indicator
    if (add_checksum_) {
      out->push_back(VCD_SOURCE | VCD_CHECKSUM);
    } else {
      out->push_back(VCD_SOURCE);
    }
    // Source segment size: dictionary size
    AppendSizeToOutputString(dictionary_size_, out);
    // Source segment position: 0 (start of dictionary)
    AppendSizeToOutputString(0, out);

    // [Here is where a secondary compressor would be used
    //  if the encoder and decoder supported that feature.]

    AppendSizeToOutputString(length_of_the_delta_encoding, out);
    // Start of Delta Encoding
    const size_t size_before_delta_encoding = out->size();
    AppendSizeToOutputString(target_length_, out);
    out->push_back(0x00);  // Delta_Indicator: no compression
    AppendSizeToOutputString(separate_data_for_add_and_run_.size(), out);
    AppendSizeToOutputString(instructions_and_sizes_.size(), out);
    AppendSizeToOutputString(separate_addresses_for_copy_.size(), out);
    if (add_checksum_) {
      // The checksum is a 32-bit *unsigned* integer.  VarintBE requires a
      // signed type, so use a 64-bit signed integer to store the checksum.
      VarintBE<int64_t>::AppendToOutputString(static_cast<int64_t>(checksum_),
                                              out);
    }
    out->append(separate_data_for_add_and_run_.data(),
                separate_data_for_add_and_run_.size());
    out->append(instructions_and_sizes_.data(),
                instructions_and_sizes_.size());
    out->append(separate_addresses_for_copy_.data(),
                separate_addresses_for_copy_.size());
    // End of Delta Encoding
    const size_t size_after_delta_encoding = out->size();
    if (length_of_the_delta_encoding !=
        (size_after_delta_encoding - size_before_delta_encoding)) {
      VCD_DFATAL << "Internal error: calculated length of the delta encoding ("
                 << length_of_the_delta_encoding
                 << ") does not match actual length ("
                 << (size_after_delta_encoding - size_before_delta_encoding)
                 << VCD_ENDL;
    }
    separate_data_for_add_and_run_.clear();
    instructions_and_sizes_.clear();
    separate_addresses_for_copy_.clear();
    if (target_length_ == 0) {
      VCD_WARNING << "Empty target window" << VCD_ENDL;
    }
  }

  // Reset state for next window; assume we are using same code table
  // and dictionary.  The caller will have to invoke Init() if a different
  // dictionary is used.
  //
  // Notably, Init() calls address_cache_.Init().  This resets the address
  // cache between delta windows, as required by RFC section 5.1.
  if (!Init(dictionary_size_)) {
    VCD_DFATAL << "Internal error: calling Init() to reset "
                  "VCDiffCodeTableWriter state failed" << VCD_ENDL;
  }
}

// Verifies dictionary is compatible with writer.
bool VCDiffCodeTableWriter::VerifyDictionary(const char * /*dictionary*/,
                                             size_t /*size*/) const {
  // Arbitrary dictionaries are allowed.
  return true;
}

// Verifies target chunk is compatible with writer.
bool VCDiffCodeTableWriter::VerifyChunk(const char * /*chunk*/,
                                        size_t /*size*/) const {
  // Arbitrary targets are allowed.
  return true;
}

};  // namespace open_vcdiff
