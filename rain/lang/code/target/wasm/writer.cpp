// clang-format off
/**
 * This is a copy of lld/wasm/Writer.cpp.
 *
 * Unfortunately that class requires going through the terminal interface, so all inputs and outputs are files, which
 * simply does not work in a wasm environment, so this tweaked copy is required.
 * 
 * All modifications are denoted by
 * // <change>
 * ...
 * // </change>
 * where the commented lines of code in between are from the original implementation.
 */


//===- Writer.cpp ---------------------------------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// <change>
// #include "Writer.h"
// #include "Config.h"
// #include "InputChunks.h"
// #include "InputElement.h"
// #include "MapFile.h"
// #include "OutputSections.h"
// #include "OutputSegment.h"
// #include "Relocations.h"
// #include "SymbolTable.h"
// #include "SyntheticSections.h"
// #include "WriterUtils.h"
#include "lld/wasm/Writer.h"
#include "lld/wasm/Config.h"
#include "lld/wasm/InputChunks.h"
#include "lld/wasm/InputElement.h"
#include "lld/wasm/MapFile.h"
#include "lld/wasm/OutputSections.h"
#include "lld/wasm/OutputSegment.h"
#include "lld/wasm/Relocations.h"
#include "lld/wasm/SymbolTable.h"
#include "lld/wasm/SyntheticSections.h"
#include "lld/wasm/WriterUtils.h"
// </change>
#include "lld/Common/Arrays.h"
#include "lld/Common/CommonLinkerContext.h"
#include "lld/Common/Strings.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/BinaryFormat/Wasm.h"
#include "llvm/BinaryFormat/WasmTraits.h"
#include "llvm/Support/FileOutputBuffer.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/Parallel.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/Support/SHA1.h"
#include "llvm/Support/xxhash.h"

#include <cstdarg>
#include <map>
#include <optional>

#define DEBUG_TYPE "lld"

using namespace llvm;
using namespace llvm::wasm;

namespace lld::wasm {
static constexpr int stackAlignment = 16;
static constexpr int heapAlignment = 16;

namespace {

// <change>
// A FileOutputBuffer which keeps data in memory and DOES NOT write to the final output file on commit().
class InMemoryBuffer : public FileOutputBuffer {
public:
  InMemoryBuffer(llvm::MemoryBuffer* buffer) : FileOutputBuffer("-"), _buffer{buffer} {}

  uint8_t *getBufferStart() const override { return (uint8_t *)_buffer->getBufferStart(); }

  uint8_t *getBufferEnd() const override { return (uint8_t *)_buffer->getBufferEnd(); }

  size_t getBufferSize() const override { return _buffer->getBufferSize(); }

  Error commit() override { return Error::success(); }

private:
    llvm::MemoryBuffer* _buffer;
};
// </change>

// The writer writes a SymbolTable result to a file.
class Writer {
public:
// <change>
//   void run();
  void run(std::unique_ptr<llvm::MemoryBuffer>& out_buffer);
// </change>

private:
// <change>
//   void openFile();
  void openFile(std::unique_ptr<llvm::MemoryBuffer>& out_buffer);
// </change>

  bool needsPassiveInitialization(const OutputSegment *segment);
  bool hasPassiveInitializedSegments();

  void createSyntheticInitFunctions();
  void createInitMemoryFunction();
  void createStartFunction();
  void createApplyDataRelocationsFunction();
  void createApplyGlobalRelocationsFunction();
  void createApplyTLSRelocationsFunction();
  void createApplyGlobalTLSRelocationsFunction();
  void createCallCtorsFunction();
  void createInitTLSFunction();
  void createCommandExportWrappers();
  void createCommandExportWrapper(uint32_t functionIndex, DefinedFunction *f);

  void assignIndexes();
  void populateSymtab();
  void populateProducers();
  void populateTargetFeatures();
  // populateTargetFeatures happens early on so some checks are delayed
  // until imports and exports are finalized.  There are run unstead
  // in checkImportExportTargetFeatures
  void checkImportExportTargetFeatures();
  void calculateInitFunctions();
  void calculateImports();
  void calculateExports();
  void calculateCustomSections();
  void calculateTypes();
  void createOutputSegments();
  OutputSegment *createOutputSegment(StringRef name);
  void combineOutputSegments();
  void layoutMemory();
  void createHeader();

  void addSection(OutputSection *sec);

  void addSections();

  void createCustomSections();
  void createSyntheticSections();
  void createSyntheticSectionsPostLayout();
  void finalizeSections();

  // Custom sections
  void createRelocSections();

  void writeHeader();
  void writeSections();
  void writeBuildId();

  uint64_t fileSize = 0;

  std::vector<WasmInitEntry> initFunctions;
  llvm::MapVector<StringRef, std::vector<InputChunk *>> customSectionMapping;

  // Stable storage for command export wrapper function name strings.
  std::list<std::string> commandExportWrapperNames;

  // Elements that are used to construct the final output
  std::string header;
  std::vector<OutputSection *> outputSections;

  std::unique_ptr<FileOutputBuffer> buffer;

  std::vector<OutputSegment *> segments;
  llvm::SmallDenseMap<StringRef, OutputSegment *> segmentMap;
};

} // anonymous namespace

void Writer::calculateCustomSections() {
  log("calculateCustomSections");
  bool stripDebug = config->stripDebug || config->stripAll;
  for (ObjFile *file : symtab->objectFiles) {
    for (InputChunk *section : file->customSections) {
      // Exclude COMDAT sections that are not selected for inclusion
      if (section->discarded)
        continue;
      StringRef name = section->name;
      // These custom sections are known the linker and synthesized rather than
      // blindly copied.
      if (name == "linking" || name == "name" || name == "producers" ||
          name == "target_features" || name.starts_with("reloc."))
        continue;
      // These custom sections are generated by `clang -fembed-bitcode`.
      // These are used by the rust toolchain to ship LTO data along with
      // compiled object code, but they don't want this included in the linker
      // output.
      if (name == ".llvmbc" || name == ".llvmcmd")
        continue;
      // Strip debug section in that option was specified.
      if (stripDebug && name.starts_with(".debug_"))
        continue;
      // Otherwise include custom sections by default and concatenate their
      // contents.
      customSectionMapping[name].push_back(section);
    }
  }
}

void Writer::createCustomSections() {
  log("createCustomSections");
  for (auto &pair : customSectionMapping) {
    StringRef name = pair.first;
    LLVM_DEBUG(dbgs() << "createCustomSection: " << name << "\n");

    OutputSection *sec = make<CustomSection>(std::string(name), pair.second);
    if (config->relocatable || config->emitRelocs) {
      auto *sym = make<OutputSectionSymbol>(sec);
      out.linkingSec->addToSymtab(sym);
      sec->sectionSym = sym;
    }
    addSection(sec);
  }
}

// Create relocations sections in the final output.
// These are only created when relocatable output is requested.
void Writer::createRelocSections() {
  log("createRelocSections");
  // Don't use iterator here since we are adding to OutputSection
  size_t origSize = outputSections.size();
  for (size_t i = 0; i < origSize; i++) {
    LLVM_DEBUG(dbgs() << "check section " << i << "\n");
    OutputSection *sec = outputSections[i];

    // Count the number of needed sections.
    uint32_t count = sec->getNumRelocations();
    if (!count)
      continue;

    StringRef name;
    if (sec->type == WASM_SEC_DATA)
      name = "reloc.DATA";
    else if (sec->type == WASM_SEC_CODE)
      name = "reloc.CODE";
    else if (sec->type == WASM_SEC_CUSTOM)
      name = saver().save("reloc." + sec->name);
    else
      llvm_unreachable(
          "relocations only supported for code, data, or custom sections");

    addSection(make<RelocSection>(name, sec));
  }
}

void Writer::populateProducers() {
  for (ObjFile *file : symtab->objectFiles) {
    const WasmProducerInfo &info = file->getWasmObj()->getProducerInfo();
    out.producersSec->addInfo(info);
  }
}

void Writer::writeHeader() {
  memcpy(buffer->getBufferStart(), header.data(), header.size());
}

void Writer::writeSections() {
  uint8_t *buf = buffer->getBufferStart();
  parallelForEach(outputSections, [buf](OutputSection *s) {
    assert(s->isNeeded());
    s->writeTo(buf);
  });
}

// Computes a hash value of Data using a given hash function.
// In order to utilize multiple cores, we first split data into 1MB
// chunks, compute a hash for each chunk, and then compute a hash value
// of the hash values.

static void
computeHash(llvm::MutableArrayRef<uint8_t> hashBuf,
            llvm::ArrayRef<uint8_t> data,
            std::function<void(uint8_t *dest, ArrayRef<uint8_t> arr)> hashFn) {
  std::vector<ArrayRef<uint8_t>> chunks = split(data, 1024 * 1024);
  std::vector<uint8_t> hashes(chunks.size() * hashBuf.size());

  // Compute hash values.
  parallelFor(0, chunks.size(), [&](size_t i) {
    hashFn(hashes.data() + i * hashBuf.size(), chunks[i]);
  });

  // Write to the final output buffer.
  hashFn(hashBuf.data(), hashes);
}

static void makeUUID(unsigned version, llvm::ArrayRef<uint8_t> fileHash,
                     llvm::MutableArrayRef<uint8_t> output) {
  assert((version == 4 || version == 5) && "Unknown UUID version");
  assert(output.size() == 16 && "Wrong size for UUID output");
  if (version == 5) {
    // Build a valid v5 UUID from a hardcoded (randomly-generated) namespace
    // UUID, and the computed hash of the output.
    std::array<uint8_t, 16> namespaceUUID{0xA1, 0xFA, 0x48, 0x2D, 0x0E, 0x22,
                                          0x03, 0x8D, 0x33, 0x8B, 0x52, 0x1C,
                                          0xD6, 0xD2, 0x12, 0xB2};
    SHA1 sha;
    sha.update(namespaceUUID);
    sha.update(fileHash);
    auto s = sha.final();
    std::copy(s.data(), &s.data()[output.size()], output.data());
  } else if (version == 4) {
    if (auto ec = llvm::getRandomBytes(output.data(), output.size()))
      error("entropy source failure: " + ec.message());
  }
  // Set the UUID version and variant fields.
  // The version is the upper nibble of byte 6 (0b0101xxxx or 0b0100xxxx)
  output[6] = (static_cast<uint8_t>(version) << 4) | (output[6] & 0xF);

  // The variant is DCE 1.1/ISO 11578 (0b10xxxxxx)
  output[8] &= 0xBF;
  output[8] |= 0x80;
}

void Writer::writeBuildId() {
  if (!out.buildIdSec->isNeeded())
    return;
  if (config->buildId == BuildIdKind::Hexstring) {
    out.buildIdSec->writeBuildId(config->buildIdVector);
    return;
  }

  // Compute a hash of all sections of the output file.
  size_t hashSize = out.buildIdSec->hashSize;
  std::vector<uint8_t> buildId(hashSize);
  llvm::ArrayRef<uint8_t> buf{buffer->getBufferStart(), size_t(fileSize)};

  switch (config->buildId) {
  case BuildIdKind::Fast: {
    std::vector<uint8_t> fileHash(8);
    computeHash(fileHash, buf, [](uint8_t *dest, ArrayRef<uint8_t> arr) {
      support::endian::write64le(dest, xxh3_64bits(arr));
    });
    makeUUID(5, fileHash, buildId);
    break;
  }
  case BuildIdKind::Sha1:
    computeHash(buildId, buf, [&](uint8_t *dest, ArrayRef<uint8_t> arr) {
      memcpy(dest, SHA1::hash(arr).data(), hashSize);
    });
    break;
  case BuildIdKind::Uuid:
    makeUUID(4, {}, buildId);
    break;
  default:
    llvm_unreachable("unknown BuildIdKind");
  }
  out.buildIdSec->writeBuildId(buildId);
}

static void setGlobalPtr(DefinedGlobal *g, uint64_t memoryPtr) {
  LLVM_DEBUG(dbgs() << "setGlobalPtr " << g->getName() << " -> " << memoryPtr << "\n");
  g->global->setPointerValue(memoryPtr);
}

// Fix the memory layout of the output binary.  This assigns memory offsets
// to each of the input data sections as well as the explicit stack region.
// The default memory layout is as follows, from low to high.
//
//  - initialized data (starting at config->globalBase)
//  - BSS data (not currently implemented in llvm)
//  - explicit stack (config->ZStackSize)
//  - heap start / unallocated
//
// The --stack-first option means that stack is placed before any static data.
// This can be useful since it means that stack overflow traps immediately
// rather than overwriting global data, but also increases code size since all
// static data loads and stores requires larger offsets.
void Writer::layoutMemory() {
  uint64_t memoryPtr = 0;

  auto placeStack = [&]() {
    if (config->relocatable || config->isPic)
      return;
    memoryPtr = alignTo(memoryPtr, stackAlignment);
    if (WasmSym::stackLow)
      WasmSym::stackLow->setVA(memoryPtr);
    if (config->zStackSize != alignTo(config->zStackSize, stackAlignment))
      error("stack size must be " + Twine(stackAlignment) + "-byte aligned");
    log("mem: stack size  = " + Twine(config->zStackSize));
    log("mem: stack base  = " + Twine(memoryPtr));
    memoryPtr += config->zStackSize;
    setGlobalPtr(cast<DefinedGlobal>(WasmSym::stackPointer), memoryPtr);
    if (WasmSym::stackHigh)
      WasmSym::stackHigh->setVA(memoryPtr);
    log("mem: stack top   = " + Twine(memoryPtr));
  };

  if (config->stackFirst) {
    placeStack();
    if (config->globalBase) {
      if (config->globalBase < memoryPtr) {
        error("--global-base cannot be less than stack size when --stack-first is used");
        return;
      }
      memoryPtr = config->globalBase;
    }
  } else {
    if (!config->globalBase && !config->relocatable && !config->isPic) {
      // The default offset for static/global data, for when --global-base is
      // not specified on the command line.  The precise value of 1024 is
      // somewhat arbitrary, and pre-dates wasm-ld (Its the value that
      // emscripten used prior to wasm-ld).
      config->globalBase = 1024;
    }
    memoryPtr = config->globalBase;
  }

  log("mem: global base = " + Twine(memoryPtr));
  if (WasmSym::globalBase)
    WasmSym::globalBase->setVA(memoryPtr);

  uint64_t dataStart = memoryPtr;

  // Arbitrarily set __dso_handle handle to point to the start of the data
  // segments.
  if (WasmSym::dsoHandle)
    WasmSym::dsoHandle->setVA(dataStart);

  out.dylinkSec->memAlign = 0;
  for (OutputSegment *seg : segments) {
    out.dylinkSec->memAlign = std::max(out.dylinkSec->memAlign, seg->alignment);
    memoryPtr = alignTo(memoryPtr, 1ULL << seg->alignment);
    seg->startVA = memoryPtr;
    log(formatv("mem: {0,-15} offset={1,-8} size={2,-8} align={3}", seg->name,
                memoryPtr, seg->size, seg->alignment));

    if (!config->relocatable && seg->isTLS()) {
      if (WasmSym::tlsSize) {
        auto *tlsSize = cast<DefinedGlobal>(WasmSym::tlsSize);
        setGlobalPtr(tlsSize, seg->size);
      }
      if (WasmSym::tlsAlign) {
        auto *tlsAlign = cast<DefinedGlobal>(WasmSym::tlsAlign);
        setGlobalPtr(tlsAlign, int64_t{1} << seg->alignment);
      }
      if (!config->sharedMemory && WasmSym::tlsBase) {
        auto *tlsBase = cast<DefinedGlobal>(WasmSym::tlsBase);
        setGlobalPtr(tlsBase, memoryPtr);
      }
    }

    memoryPtr += seg->size;
  }

  // Make space for the memory initialization flag
  if (config->sharedMemory && hasPassiveInitializedSegments()) {
    memoryPtr = alignTo(memoryPtr, 4);
    WasmSym::initMemoryFlag = symtab->addSyntheticDataSymbol(
        "__wasm_init_memory_flag", WASM_SYMBOL_VISIBILITY_HIDDEN);
    WasmSym::initMemoryFlag->markLive();
    WasmSym::initMemoryFlag->setVA(memoryPtr);
    log(formatv("mem: {0,-15} offset={1,-8} size={2,-8} align={3}",
                "__wasm_init_memory_flag", memoryPtr, 4, 4));
    memoryPtr += 4;
  }

  if (WasmSym::dataEnd)
    WasmSym::dataEnd->setVA(memoryPtr);

  uint64_t staticDataSize = memoryPtr - dataStart;
  log("mem: static data = " + Twine(staticDataSize));
  if (config->isPic)
    out.dylinkSec->memSize = staticDataSize;

  if (!config->stackFirst)
    placeStack();

  if (WasmSym::heapBase) {
    // Set `__heap_base` to follow the end of the stack or global data. The
    // fact that this comes last means that a malloc/brk implementation can
    // grow the heap at runtime.
    // We'll align the heap base here because memory allocators might expect
    // __heap_base to be aligned already.
    memoryPtr = alignTo(memoryPtr, heapAlignment);
    log("mem: heap base   = " + Twine(memoryPtr));
    WasmSym::heapBase->setVA(memoryPtr);
  }

  uint64_t maxMemorySetting = 1ULL << 32;
  if (config->is64.value_or(false)) {
    // TODO: Update once we decide on a reasonable limit here:
    // https://github.com/WebAssembly/memory64/issues/33
    maxMemorySetting = 1ULL << 34;
  }

  if (config->initialMemory != 0) {
    if (config->initialMemory != alignTo(config->initialMemory, WasmPageSize))
      error("initial memory must be " + Twine(WasmPageSize) + "-byte aligned");
    if (memoryPtr > config->initialMemory)
      error("initial memory too small, " + Twine(memoryPtr) + " bytes needed");
    if (config->initialMemory > maxMemorySetting)
      error("initial memory too large, cannot be greater than " +
            Twine(maxMemorySetting));
    memoryPtr = config->initialMemory;
  }

  memoryPtr = alignTo(memoryPtr, WasmPageSize);

  out.memorySec->numMemoryPages = memoryPtr / WasmPageSize;
  log("mem: total pages = " + Twine(out.memorySec->numMemoryPages));

  if (WasmSym::heapEnd) {
    // Set `__heap_end` to follow the end of the statically allocated linear
    // memory. The fact that this comes last means that a malloc/brk
    // implementation can grow the heap at runtime.
    log("mem: heap end    = " + Twine(memoryPtr));
    WasmSym::heapEnd->setVA(memoryPtr);
  }

  if (config->maxMemory != 0) {
    if (config->maxMemory != alignTo(config->maxMemory, WasmPageSize))
      error("maximum memory must be " + Twine(WasmPageSize) + "-byte aligned");
    if (memoryPtr > config->maxMemory)
      error("maximum memory too small, " + Twine(memoryPtr) + " bytes needed");
    if (config->maxMemory > maxMemorySetting)
      error("maximum memory too large, cannot be greater than " +
            Twine(maxMemorySetting));
  }

  // Check max if explicitly supplied or required by shared memory
  if (config->maxMemory != 0 || config->sharedMemory) {
    uint64_t max = config->maxMemory;
    if (max == 0) {
      // If no maxMemory config was supplied but we are building with
      // shared memory, we need to pick a sensible upper limit.
      if (config->isPic)
        max = maxMemorySetting;
      else
        max = memoryPtr;
    }
    out.memorySec->maxMemoryPages = max / WasmPageSize;
    log("mem: max pages   = " + Twine(out.memorySec->maxMemoryPages));
  }
}

void Writer::addSection(OutputSection *sec) {
  if (!sec->isNeeded())
    return;
  log("addSection: " + toString(*sec));
  sec->sectionIndex = outputSections.size();
  outputSections.push_back(sec);
}

// If a section name is valid as a C identifier (which is rare because of
// the leading '.'), linkers are expected to define __start_<secname> and
// __stop_<secname> symbols. They are at beginning and end of the section,
// respectively. This is not requested by the ELF standard, but GNU ld and
// gold provide the feature, and used by many programs.
static void addStartStopSymbols(const OutputSegment *seg) {
  StringRef name = seg->name;
  if (!isValidCIdentifier(name))
    return;
  LLVM_DEBUG(dbgs() << "addStartStopSymbols: " << name << "\n");
  uint64_t start = seg->startVA;
  uint64_t stop = start + seg->size;
  symtab->addOptionalDataSymbol(saver().save("__start_" + name), start);
  symtab->addOptionalDataSymbol(saver().save("__stop_" + name), stop);
}

void Writer::addSections() {
  addSection(out.dylinkSec);
  addSection(out.typeSec);
  addSection(out.importSec);
  addSection(out.functionSec);
  addSection(out.tableSec);
  addSection(out.memorySec);
  addSection(out.tagSec);
  addSection(out.globalSec);
  addSection(out.exportSec);
  addSection(out.startSec);
  addSection(out.elemSec);
  addSection(out.dataCountSec);

  addSection(make<CodeSection>(out.functionSec->inputFunctions));
  addSection(make<DataSection>(segments));

  createCustomSections();

  addSection(out.linkingSec);
  if (config->emitRelocs || config->relocatable) {
    createRelocSections();
  }

  addSection(out.nameSec);
  addSection(out.producersSec);
  addSection(out.targetFeaturesSec);
  addSection(out.buildIdSec);
}

void Writer::finalizeSections() {
  for (OutputSection *s : outputSections) {
    s->setOffset(fileSize);
    s->finalizeContents();
    fileSize += s->getSize();
  }
}

void Writer::populateTargetFeatures() {
  StringMap<std::string> used;
  StringMap<std::string> required;
  StringMap<std::string> disallowed;
  SmallSet<std::string, 8> &allowed = out.targetFeaturesSec->features;
  bool tlsUsed = false;

  if (config->isPic) {
    // This should not be necessary because all PIC objects should
    // contain the mutable-globals feature.
    // TODO (https://github.com/llvm/llvm-project/issues/51681)
    allowed.insert("mutable-globals");
  }

  if (config->extraFeatures.has_value()) {
    auto &extraFeatures = *config->extraFeatures;
    allowed.insert(extraFeatures.begin(), extraFeatures.end());
  }

  // Only infer used features if user did not specify features
  bool inferFeatures = !config->features.has_value();

  if (!inferFeatures) {
    auto &explicitFeatures = *config->features;
    allowed.insert(explicitFeatures.begin(), explicitFeatures.end());
    if (!config->checkFeatures)
      goto done;
  }

  // Find the sets of used, required, and disallowed features
  for (ObjFile *file : symtab->objectFiles) {
    StringRef fileName(file->getName());
    for (auto &feature : file->getWasmObj()->getTargetFeatures()) {
      switch (feature.Prefix) {
      case WASM_FEATURE_PREFIX_USED:
        used.insert({feature.Name, std::string(fileName)});
        break;
      case WASM_FEATURE_PREFIX_REQUIRED:
        used.insert({feature.Name, std::string(fileName)});
        required.insert({feature.Name, std::string(fileName)});
        break;
      case WASM_FEATURE_PREFIX_DISALLOWED:
        disallowed.insert({feature.Name, std::string(fileName)});
        break;
      default:
        error("Unrecognized feature policy prefix " +
              std::to_string(feature.Prefix));
      }
    }

    // Find TLS data segments
    auto isTLS = [](InputChunk *segment) {
      return segment->live && segment->isTLS();
    };
    tlsUsed = tlsUsed || llvm::any_of(file->segments, isTLS);
  }

  if (inferFeatures)
    for (const auto &key : used.keys())
      allowed.insert(std::string(key));

  if (!config->checkFeatures)
    goto done;

  if (config->sharedMemory) {
    if (disallowed.count("shared-mem"))
      error("--shared-memory is disallowed by " + disallowed["shared-mem"] +
            " because it was not compiled with 'atomics' or 'bulk-memory' "
            "features.");

    for (auto feature : {"atomics", "bulk-memory"})
      if (!allowed.count(feature))
        error(StringRef("'") + feature +
              "' feature must be used in order to use shared memory");
  }

  if (tlsUsed) {
    for (auto feature : {"atomics", "bulk-memory"})
      if (!allowed.count(feature))
        error(StringRef("'") + feature +
              "' feature must be used in order to use thread-local storage");
  }

  // Validate that used features are allowed in output
  if (!inferFeatures) {
    for (const auto &feature : used.keys()) {
      if (!allowed.count(std::string(feature)))
        error(Twine("Target feature '") + feature + "' used by " +
              used[feature] + " is not allowed.");
    }
  }

  // Validate the required and disallowed constraints for each file
  for (ObjFile *file : symtab->objectFiles) {
    StringRef fileName(file->getName());
    SmallSet<std::string, 8> objectFeatures;
    for (const auto &feature : file->getWasmObj()->getTargetFeatures()) {
      if (feature.Prefix == WASM_FEATURE_PREFIX_DISALLOWED)
        continue;
      objectFeatures.insert(feature.Name);
      if (disallowed.count(feature.Name))
        error(Twine("Target feature '") + feature.Name + "' used in " +
              fileName + " is disallowed by " + disallowed[feature.Name] +
              ". Use --no-check-features to suppress.");
    }
    for (const auto &feature : required.keys()) {
      if (!objectFeatures.count(std::string(feature)))
        error(Twine("Missing target feature '") + feature + "' in " + fileName +
              ", required by " + required[feature] +
              ". Use --no-check-features to suppress.");
    }
  }

done:
  // Normally we don't include bss segments in the binary.  In particular if
  // memory is not being imported then we can assume its zero initialized.
  // In the case the memory is imported, and we can use the memory.fill
  // instruction, then we can also avoid including the segments.
  if (config->memoryImport.has_value() && !allowed.count("bulk-memory"))
    config->emitBssSegments = true;

  if (allowed.count("extended-const"))
    config->extendedConst = true;

  for (auto &feature : allowed)
    log("Allowed feature: " + feature);
}

void Writer::checkImportExportTargetFeatures() {
  if (config->relocatable || !config->checkFeatures)
    return;

  if (out.targetFeaturesSec->features.count("mutable-globals") == 0) {
    for (const Symbol *sym : out.importSec->importedSymbols) {
      if (auto *global = dyn_cast<GlobalSymbol>(sym)) {
        if (global->getGlobalType()->Mutable) {
          error(Twine("mutable global imported but 'mutable-globals' feature "
                      "not present in inputs: `") +
                toString(*sym) + "`. Use --no-check-features to suppress.");
        }
      }
    }
    for (const Symbol *sym : out.exportSec->exportedSymbols) {
      if (isa<GlobalSymbol>(sym)) {
        error(Twine("mutable global exported but 'mutable-globals' feature "
                    "not present in inputs: `") +
              toString(*sym) + "`. Use --no-check-features to suppress.");
      }
    }
  }
}

static bool shouldImport(Symbol *sym) {
  // We don't generate imports for data symbols. They however can be imported
  // as GOT entries.
  if (isa<DataSymbol>(sym))
    return false;
  if (!sym->isLive())
    return false;
  if (!sym->isUsedInRegularObj)
    return false;

  // When a symbol is weakly defined in a shared library we need to allow
  // it to be overridden by another module so need to both import
  // and export the symbol.
  if (config->shared && sym->isWeak() && !sym->isUndefined() &&
      !sym->isHidden())
    return true;
  if (!sym->isUndefined())
    return false;
  if (sym->isWeak() && !config->relocatable && !config->isPic)
    return false;

  // In PIC mode we only need to import functions when they are called directly.
  // Indirect usage all goes via GOT imports.
  if (config->isPic) {
    if (auto *f = dyn_cast<UndefinedFunction>(sym))
      if (!f->isCalledDirectly)
        return false;
  }

  if (config->isPic || config->relocatable || config->importUndefined ||
      config->unresolvedSymbols == UnresolvedPolicy::ImportDynamic)
    return true;
  if (config->allowUndefinedSymbols.count(sym->getName()) != 0)
    return true;

  return sym->isImported();
}

void Writer::calculateImports() {
  // Some inputs require that the indirect function table be assigned to table
  // number 0, so if it is present and is an import, allocate it before any
  // other tables.
  if (WasmSym::indirectFunctionTable &&
      shouldImport(WasmSym::indirectFunctionTable))
    out.importSec->addImport(WasmSym::indirectFunctionTable);

  for (Symbol *sym : symtab->symbols()) {
    if (!shouldImport(sym))
      continue;
    if (sym == WasmSym::indirectFunctionTable)
      continue;
    LLVM_DEBUG(dbgs() << "import: " << sym->getName() << "\n");
    out.importSec->addImport(sym);
  }
}

void Writer::calculateExports() {
  if (config->relocatable)
    return;

  if (!config->relocatable && config->memoryExport.has_value()) {
    out.exportSec->exports.push_back(
        WasmExport{*config->memoryExport, WASM_EXTERNAL_MEMORY, 0});
  }

  unsigned globalIndex =
      out.importSec->getNumImportedGlobals() + out.globalSec->numGlobals();

  for (Symbol *sym : symtab->symbols()) {
    if (!sym->isExported())
      continue;
    if (!sym->isLive())
      continue;

    StringRef name = sym->getName();
    WasmExport export_;
    if (auto *f = dyn_cast<DefinedFunction>(sym)) {
      if (std::optional<StringRef> exportName = f->function->getExportName()) {
        name = *exportName;
      }
      export_ = {name, WASM_EXTERNAL_FUNCTION, f->getExportedFunctionIndex()};
    } else if (auto *g = dyn_cast<DefinedGlobal>(sym)) {
      if (g->getGlobalType()->Mutable && !g->getFile() && !g->forceExport) {
        // Avoid exporting mutable globals are linker synthesized (e.g.
        // __stack_pointer or __tls_base) unless they are explicitly exported
        // from the command line.
        // Without this check `--export-all` would cause any program using the
        // stack pointer to export a mutable global even if none of the input
        // files were built with the `mutable-globals` feature.
        continue;
      }
      export_ = {name, WASM_EXTERNAL_GLOBAL, g->getGlobalIndex()};
    } else if (auto *t = dyn_cast<DefinedTag>(sym)) {
      export_ = {name, WASM_EXTERNAL_TAG, t->getTagIndex()};
    } else if (auto *d = dyn_cast<DefinedData>(sym)) {
      out.globalSec->dataAddressGlobals.push_back(d);
      export_ = {name, WASM_EXTERNAL_GLOBAL, globalIndex++};
    } else {
      auto *t = cast<DefinedTable>(sym);
      export_ = {name, WASM_EXTERNAL_TABLE, t->getTableNumber()};
    }

    LLVM_DEBUG(dbgs() << "Export: " << name << "\n");
    out.exportSec->exports.push_back(export_);
    out.exportSec->exportedSymbols.push_back(sym);
  }
}

void Writer::populateSymtab() {
  if (!config->relocatable && !config->emitRelocs)
    return;

  for (Symbol *sym : symtab->symbols())
    if (sym->isUsedInRegularObj && sym->isLive())
      out.linkingSec->addToSymtab(sym);

  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "Local symtab entries: " << file->getName() << "\n");
    for (Symbol *sym : file->getSymbols())
      if (sym->isLocal() && !isa<SectionSymbol>(sym) && sym->isLive())
        out.linkingSec->addToSymtab(sym);
  }
}

void Writer::calculateTypes() {
  // The output type section is the union of the following sets:
  // 1. Any signature used in the TYPE relocation
  // 2. The signatures of all imported functions
  // 3. The signatures of all defined functions
  // 4. The signatures of all imported tags
  // 5. The signatures of all defined tags

  for (ObjFile *file : symtab->objectFiles) {
    ArrayRef<WasmSignature> types = file->getWasmObj()->types();
    for (uint32_t i = 0; i < types.size(); i++)
      if (file->typeIsUsed[i])
        file->typeMap[i] = out.typeSec->registerType(types[i]);
  }

  for (const Symbol *sym : out.importSec->importedSymbols) {
    if (auto *f = dyn_cast<FunctionSymbol>(sym))
      out.typeSec->registerType(*f->signature);
    else if (auto *t = dyn_cast<TagSymbol>(sym))
      out.typeSec->registerType(*t->signature);
  }

  for (const InputFunction *f : out.functionSec->inputFunctions)
    out.typeSec->registerType(f->signature);

  for (const InputTag *t : out.tagSec->inputTags)
    out.typeSec->registerType(t->signature);
}

// In a command-style link, create a wrapper for each exported symbol
// which calls the constructors and destructors.
void Writer::createCommandExportWrappers() {
  // This logic doesn't currently support Emscripten-style PIC mode.
  assert(!config->isPic);

  // If there are no ctors and there's no libc `__wasm_call_dtors` to
  // call, don't wrap the exports.
  if (initFunctions.empty() && WasmSym::callDtors == nullptr)
    return;

  std::vector<DefinedFunction *> toWrap;

  for (Symbol *sym : symtab->symbols())
    if (sym->isExported())
      if (auto *f = dyn_cast<DefinedFunction>(sym))
        toWrap.push_back(f);

  for (auto *f : toWrap) {
    auto funcNameStr = (f->getName() + ".command_export").str();
    commandExportWrapperNames.push_back(funcNameStr);
    const std::string &funcName = commandExportWrapperNames.back();

    auto func = make<SyntheticFunction>(*f->getSignature(), funcName);
    if (f->function->getExportName())
      func->setExportName(f->function->getExportName()->str());
    else
      func->setExportName(f->getName().str());

    DefinedFunction *def =
        symtab->addSyntheticFunction(funcName, f->flags, func);
    def->markLive();

    def->flags |= WASM_SYMBOL_EXPORTED;
    def->flags &= ~WASM_SYMBOL_VISIBILITY_HIDDEN;
    def->forceExport = f->forceExport;

    f->flags |= WASM_SYMBOL_VISIBILITY_HIDDEN;
    f->flags &= ~WASM_SYMBOL_EXPORTED;
    f->forceExport = false;

    out.functionSec->addFunction(func);

    createCommandExportWrapper(f->getFunctionIndex(), def);
  }
}

static void finalizeIndirectFunctionTable() {
  if (!WasmSym::indirectFunctionTable)
    return;

  if (shouldImport(WasmSym::indirectFunctionTable) &&
      !WasmSym::indirectFunctionTable->hasTableNumber()) {
    // Processing -Bsymbolic relocations resulted in a late requirement that the
    // indirect function table be present, and we are running in --import-table
    // mode.  Add the table now to the imports section.  Otherwise it will be
    // added to the tables section later in assignIndexes.
    out.importSec->addImport(WasmSym::indirectFunctionTable);
  }

  uint32_t tableSize = config->tableBase + out.elemSec->numEntries();
  WasmLimits limits = {0, tableSize, 0};
  if (WasmSym::indirectFunctionTable->isDefined() && !config->growableTable) {
    limits.Flags |= WASM_LIMITS_FLAG_HAS_MAX;
    limits.Maximum = limits.Minimum;
  }
  WasmSym::indirectFunctionTable->setLimits(limits);
}

static void scanRelocations() {
  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "scanRelocations: " << file->getName() << "\n");
    for (InputChunk *chunk : file->functions)
      scanRelocations(chunk);
    for (InputChunk *chunk : file->segments)
      scanRelocations(chunk);
    for (auto &p : file->customSections)
      scanRelocations(p);
  }
}

void Writer::assignIndexes() {
  // Seal the import section, since other index spaces such as function and
  // global are effected by the number of imports.
  out.importSec->seal();

  for (InputFunction *func : symtab->syntheticFunctions)
    out.functionSec->addFunction(func);

  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "Functions: " << file->getName() << "\n");
    for (InputFunction *func : file->functions)
      out.functionSec->addFunction(func);
  }

  for (InputGlobal *global : symtab->syntheticGlobals)
    out.globalSec->addGlobal(global);

  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "Globals: " << file->getName() << "\n");
    for (InputGlobal *global : file->globals)
      out.globalSec->addGlobal(global);
  }

  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "Tags: " << file->getName() << "\n");
    for (InputTag *tag : file->tags)
      out.tagSec->addTag(tag);
  }

  for (ObjFile *file : symtab->objectFiles) {
    LLVM_DEBUG(dbgs() << "Tables: " << file->getName() << "\n");
    for (InputTable *table : file->tables)
      out.tableSec->addTable(table);
  }

  for (InputTable *table : symtab->syntheticTables)
    out.tableSec->addTable(table);

  out.globalSec->assignIndexes();
  out.tableSec->assignIndexes();
}

static StringRef getOutputDataSegmentName(const InputChunk &seg) {
  // We always merge .tbss and .tdata into a single TLS segment so all TLS
  // symbols are be relative to single __tls_base.
  if (seg.isTLS())
    return ".tdata";
  if (!config->mergeDataSegments)
    return seg.name;
  if (seg.name.starts_with(".text."))
    return ".text";
  if (seg.name.starts_with(".data."))
    return ".data";
  if (seg.name.starts_with(".bss."))
    return ".bss";
  if (seg.name.starts_with(".rodata."))
    return ".rodata";
  return seg.name;
}

OutputSegment *Writer::createOutputSegment(StringRef name) {
  LLVM_DEBUG(dbgs() << "new segment: " << name << "\n");
  OutputSegment *s = make<OutputSegment>(name);
  if (config->sharedMemory)
    s->initFlags = WASM_DATA_SEGMENT_IS_PASSIVE;
  if (!config->relocatable && name.starts_with(".bss"))
    s->isBss = true;
  segments.push_back(s);
  return s;
}

void Writer::createOutputSegments() {
  for (ObjFile *file : symtab->objectFiles) {
    for (InputChunk *segment : file->segments) {
      if (!segment->live)
        continue;
      StringRef name = getOutputDataSegmentName(*segment);
      OutputSegment *s = nullptr;
      // When running in relocatable mode we can't merge segments that are part
      // of comdat groups since the ultimate linker needs to be able exclude or
      // include them individually.
      if (config->relocatable && !segment->getComdatName().empty()) {
        s = createOutputSegment(name);
      } else {
        if (segmentMap.count(name) == 0)
          segmentMap[name] = createOutputSegment(name);
        s = segmentMap[name];
      }
      s->addInputSegment(segment);
    }
  }

  // Sort segments by type, placing .bss last
  std::stable_sort(segments.begin(), segments.end(),
                   [](const OutputSegment *a, const OutputSegment *b) {
                     auto order = [](StringRef name) {
                       return StringSwitch<int>(name)
                           .StartsWith(".tdata", 0)
                           .StartsWith(".rodata", 1)
                           .StartsWith(".data", 2)
                           .StartsWith(".bss", 4)
                           .Default(3);
                     };
                     return order(a->name) < order(b->name);
                   });

  for (size_t i = 0; i < segments.size(); ++i)
    segments[i]->index = i;

  // Merge MergeInputSections into a single MergeSyntheticSection.
  LLVM_DEBUG(dbgs() << "-- finalize input semgments\n");
  for (OutputSegment *seg : segments)
    seg->finalizeInputSegments();
}

void Writer::combineOutputSegments() {
  // With PIC code we currently only support a single active data segment since
  // we only have a single __memory_base to use as our base address.  This pass
  // combines all data segments into a single .data segment.
  // This restriction does not apply when the extended const extension is
  // available: https://github.com/WebAssembly/extended-const
  assert(!config->extendedConst);
  assert(config->isPic && !config->sharedMemory);
  if (segments.size() <= 1)
    return;
  OutputSegment *combined = make<OutputSegment>(".data");
  combined->startVA = segments[0]->startVA;
  for (OutputSegment *s : segments) {
    bool first = true;
    for (InputChunk *inSeg : s->inputSegments) {
      if (first)
        inSeg->alignment = std::max(inSeg->alignment, s->alignment);
      first = false;
#ifndef NDEBUG
      uint64_t oldVA = inSeg->getVA();
#endif
      combined->addInputSegment(inSeg);
#ifndef NDEBUG
      uint64_t newVA = inSeg->getVA();
      LLVM_DEBUG(dbgs() << "added input segment. name=" << inSeg->name
                        << " oldVA=" << oldVA << " newVA=" << newVA << "\n");
      assert(oldVA == newVA);
#endif
    }
  }

  segments = {combined};
}

static void createFunction(DefinedFunction *func, StringRef bodyContent) {
  std::string functionBody;
  {
    raw_string_ostream os(functionBody);
    writeUleb128(os, bodyContent.size(), "function size");
    os << bodyContent;
  }
  ArrayRef<uint8_t> body = arrayRefFromStringRef(saver().save(functionBody));
  cast<SyntheticFunction>(func->function)->setBody(body);
}

bool Writer::needsPassiveInitialization(const OutputSegment *segment) {
  // If bulk memory features is supported then we can perform bss initialization
  // (via memory.fill) during `__wasm_init_memory`.
  if (config->memoryImport.has_value() && !segment->requiredInBinary())
    return true;
  return segment->initFlags & WASM_DATA_SEGMENT_IS_PASSIVE;
}

bool Writer::hasPassiveInitializedSegments() {
  return llvm::any_of(segments, [this](const OutputSegment *s) {
    return this->needsPassiveInitialization(s);
  });
}

void Writer::createSyntheticInitFunctions() {
  if (config->relocatable)
    return;

  static WasmSignature nullSignature = {{}, {}};

  // Passive segments are used to avoid memory being reinitialized on each
  // thread's instantiation. These passive segments are initialized and
  // dropped in __wasm_init_memory, which is registered as the start function
  // We also initialize bss segments (using memory.fill) as part of this
  // function.
  if (hasPassiveInitializedSegments()) {
    WasmSym::initMemory = symtab->addSyntheticFunction(
        "__wasm_init_memory", WASM_SYMBOL_VISIBILITY_HIDDEN,
        make<SyntheticFunction>(nullSignature, "__wasm_init_memory"));
    WasmSym::initMemory->markLive();
    if (config->sharedMemory) {
      // This global is assigned during  __wasm_init_memory in the shared memory
      // case.
      WasmSym::tlsBase->markLive();
    }
  }

  if (config->sharedMemory) {
    if (out.globalSec->needsTLSRelocations()) {
      WasmSym::applyGlobalTLSRelocs = symtab->addSyntheticFunction(
          "__wasm_apply_global_tls_relocs", WASM_SYMBOL_VISIBILITY_HIDDEN,
          make<SyntheticFunction>(nullSignature,
                                  "__wasm_apply_global_tls_relocs"));
      WasmSym::applyGlobalTLSRelocs->markLive();
      // TLS relocations depend on  the __tls_base symbols
      WasmSym::tlsBase->markLive();
    }

    auto hasTLSRelocs = [](const OutputSegment *segment) {
      if (segment->isTLS())
        for (const auto* is: segment->inputSegments)
          if (is->getRelocations().size())
            return true;
      return false;
    };
    if (llvm::any_of(segments, hasTLSRelocs)) {
      WasmSym::applyTLSRelocs = symtab->addSyntheticFunction(
          "__wasm_apply_tls_relocs", WASM_SYMBOL_VISIBILITY_HIDDEN,
          make<SyntheticFunction>(nullSignature,
                                  "__wasm_apply_tls_relocs"));
      WasmSym::applyTLSRelocs->markLive();
    }
  }

  if (config->isPic && out.globalSec->needsRelocations()) {
    WasmSym::applyGlobalRelocs = symtab->addSyntheticFunction(
        "__wasm_apply_global_relocs", WASM_SYMBOL_VISIBILITY_HIDDEN,
        make<SyntheticFunction>(nullSignature, "__wasm_apply_global_relocs"));
    WasmSym::applyGlobalRelocs->markLive();
  }

  // If there is only one start function we can just use that function
  // itself as the Wasm start function, otherwise we need to synthesize
  // a new function to call them in sequence.
  if (WasmSym::applyGlobalRelocs && WasmSym::initMemory) {
    WasmSym::startFunction = symtab->addSyntheticFunction(
        "__wasm_start", WASM_SYMBOL_VISIBILITY_HIDDEN,
        make<SyntheticFunction>(nullSignature, "__wasm_start"));
    WasmSym::startFunction->markLive();
  }
}

void Writer::createInitMemoryFunction() {
  LLVM_DEBUG(dbgs() << "createInitMemoryFunction\n");
  assert(WasmSym::initMemory);
  assert(hasPassiveInitializedSegments());
  uint64_t flagAddress;
  if (config->sharedMemory) {
    assert(WasmSym::initMemoryFlag);
    flagAddress = WasmSym::initMemoryFlag->getVA();
  }
  bool is64 = config->is64.value_or(false);
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    // Initialize memory in a thread-safe manner. The thread that successfully
    // increments the flag from 0 to 1 is responsible for performing the memory
    // initialization. Other threads go sleep on the flag until the first thread
    // finishing initializing memory, increments the flag to 2, and wakes all
    // the other threads. Once the flag has been set to 2, subsequently started
    // threads will skip the sleep. All threads unconditionally drop their
    // passive data segments once memory has been initialized. The generated
    // code is as follows:
    //
    // (func $__wasm_init_memory
    //  (block $drop
    //   (block $wait
    //    (block $init
    //     (br_table $init $wait $drop
    //      (i32.atomic.rmw.cmpxchg align=2 offset=0
    //       (i32.const $__init_memory_flag)
    //       (i32.const 0)
    //       (i32.const 1)
    //      )
    //     )
    //    ) ;; $init
    //    ( ... initialize data segments ... )
    //    (i32.atomic.store align=2 offset=0
    //     (i32.const $__init_memory_flag)
    //     (i32.const 2)
    //    )
    //    (drop
    //     (i32.atomic.notify align=2 offset=0
    //      (i32.const $__init_memory_flag)
    //      (i32.const -1u)
    //     )
    //    )
    //    (br $drop)
    //   ) ;; $wait
    //   (drop
    //    (i32.atomic.wait align=2 offset=0
    //     (i32.const $__init_memory_flag)
    //     (i32.const 1)
    //     (i32.const -1)
    //    )
    //   )
    //  ) ;; $drop
    //  ( ... drop data segments ... )
    // )
    //
    // When we are building with PIC, calculate the flag location using:
    //
    //    (global.get $__memory_base)
    //    (i32.const $__init_memory_flag)
    //    (i32.const 1)

    auto writeGetFlagAddress = [&]() {
      if (config->isPic) {
        writeU8(os, WASM_OPCODE_LOCAL_GET, "local.get");
        writeUleb128(os, 0, "local 0");
      } else {
        writePtrConst(os, flagAddress, is64, "flag address");
      }
    };

    if (config->sharedMemory) {
      // With PIC code we cache the flag address in local 0
      if (config->isPic) {
        writeUleb128(os, 1, "num local decls");
        writeUleb128(os, 2, "local count");
        writeU8(os, is64 ? WASM_TYPE_I64 : WASM_TYPE_I32, "address type");
        writeU8(os, WASM_OPCODE_GLOBAL_GET, "GLOBAL_GET");
        writeUleb128(os, WasmSym::memoryBase->getGlobalIndex(), "memory_base");
        writePtrConst(os, flagAddress, is64, "flag address");
        writeU8(os, is64 ? WASM_OPCODE_I64_ADD : WASM_OPCODE_I32_ADD, "add");
        writeU8(os, WASM_OPCODE_LOCAL_SET, "local.set");
        writeUleb128(os, 0, "local 0");
      } else {
        writeUleb128(os, 0, "num locals");
      }

      // Set up destination blocks
      writeU8(os, WASM_OPCODE_BLOCK, "block $drop");
      writeU8(os, WASM_TYPE_NORESULT, "block type");
      writeU8(os, WASM_OPCODE_BLOCK, "block $wait");
      writeU8(os, WASM_TYPE_NORESULT, "block type");
      writeU8(os, WASM_OPCODE_BLOCK, "block $init");
      writeU8(os, WASM_TYPE_NORESULT, "block type");

      // Atomically check whether we win the race.
      writeGetFlagAddress();
      writeI32Const(os, 0, "expected flag value");
      writeI32Const(os, 1, "new flag value");
      writeU8(os, WASM_OPCODE_ATOMICS_PREFIX, "atomics prefix");
      writeUleb128(os, WASM_OPCODE_I32_RMW_CMPXCHG, "i32.atomic.rmw.cmpxchg");
      writeMemArg(os, 2, 0);

      // Based on the value, decide what to do next.
      writeU8(os, WASM_OPCODE_BR_TABLE, "br_table");
      writeUleb128(os, 2, "label vector length");
      writeUleb128(os, 0, "label $init");
      writeUleb128(os, 1, "label $wait");
      writeUleb128(os, 2, "default label $drop");

      // Initialize passive data segments
      writeU8(os, WASM_OPCODE_END, "end $init");
    } else {
      writeUleb128(os, 0, "num local decls");
    }

    for (const OutputSegment *s : segments) {
      if (needsPassiveInitialization(s)) {
        // For passive BSS segments we can simple issue a memory.fill(0).
        // For non-BSS segments we do a memory.init.  Both these
        // instructions take as their first argument the destination
        // address.
        writePtrConst(os, s->startVA, is64, "destination address");
        if (config->isPic) {
          writeU8(os, WASM_OPCODE_GLOBAL_GET, "GLOBAL_GET");
          writeUleb128(os, WasmSym::memoryBase->getGlobalIndex(),
                       "__memory_base");
          writeU8(os, is64 ? WASM_OPCODE_I64_ADD : WASM_OPCODE_I32_ADD,
                  "i32.add");
        }

        // When we initialize the TLS segment we also set the `__tls_base`
        // global.  This allows the runtime to use this static copy of the
        // TLS data for the first/main thread.
        if (config->sharedMemory && s->isTLS()) {
          if (config->isPic) {
            // Cache the result of the addionion in local 0
            writeU8(os, WASM_OPCODE_LOCAL_TEE, "local.tee");
            writeUleb128(os, 1, "local 1");
          } else {
            writePtrConst(os, s->startVA, is64, "destination address");
          }
          writeU8(os, WASM_OPCODE_GLOBAL_SET, "GLOBAL_SET");
          writeUleb128(os, WasmSym::tlsBase->getGlobalIndex(),
                       "__tls_base");
          if (config->isPic) {
            writeU8(os, WASM_OPCODE_LOCAL_GET, "local.tee");
            writeUleb128(os, 1, "local 1");
          }
        }

        if (s->isBss) {
          writeI32Const(os, 0, "fill value");
          writePtrConst(os, s->size, is64, "memory region size");
          writeU8(os, WASM_OPCODE_MISC_PREFIX, "bulk-memory prefix");
          writeUleb128(os, WASM_OPCODE_MEMORY_FILL, "memory.fill");
          writeU8(os, 0, "memory index immediate");
        } else {
          writeI32Const(os, 0, "source segment offset");
          writeI32Const(os, s->size, "memory region size");
          writeU8(os, WASM_OPCODE_MISC_PREFIX, "bulk-memory prefix");
          writeUleb128(os, WASM_OPCODE_MEMORY_INIT, "memory.init");
          writeUleb128(os, s->index, "segment index immediate");
          writeU8(os, 0, "memory index immediate");
        }
      }
    }

    if (config->sharedMemory) {
      // Set flag to 2 to mark end of initialization
      writeGetFlagAddress();
      writeI32Const(os, 2, "flag value");
      writeU8(os, WASM_OPCODE_ATOMICS_PREFIX, "atomics prefix");
      writeUleb128(os, WASM_OPCODE_I32_ATOMIC_STORE, "i32.atomic.store");
      writeMemArg(os, 2, 0);

      // Notify any waiters that memory initialization is complete
      writeGetFlagAddress();
      writeI32Const(os, -1, "number of waiters");
      writeU8(os, WASM_OPCODE_ATOMICS_PREFIX, "atomics prefix");
      writeUleb128(os, WASM_OPCODE_ATOMIC_NOTIFY, "atomic.notify");
      writeMemArg(os, 2, 0);
      writeU8(os, WASM_OPCODE_DROP, "drop");

      // Branch to drop the segments
      writeU8(os, WASM_OPCODE_BR, "br");
      writeUleb128(os, 1, "label $drop");

      // Wait for the winning thread to initialize memory
      writeU8(os, WASM_OPCODE_END, "end $wait");
      writeGetFlagAddress();
      writeI32Const(os, 1, "expected flag value");
      writeI64Const(os, -1, "timeout");

      writeU8(os, WASM_OPCODE_ATOMICS_PREFIX, "atomics prefix");
      writeUleb128(os, WASM_OPCODE_I32_ATOMIC_WAIT, "i32.atomic.wait");
      writeMemArg(os, 2, 0);
      writeU8(os, WASM_OPCODE_DROP, "drop");

      // Unconditionally drop passive data segments
      writeU8(os, WASM_OPCODE_END, "end $drop");
    }

    for (const OutputSegment *s : segments) {
      if (needsPassiveInitialization(s) && !s->isBss) {
        // The TLS region should not be dropped since its is needed
        // during the initialization of each thread (__wasm_init_tls).
        if (config->sharedMemory && s->isTLS())
          continue;
        // data.drop instruction
        writeU8(os, WASM_OPCODE_MISC_PREFIX, "bulk-memory prefix");
        writeUleb128(os, WASM_OPCODE_DATA_DROP, "data.drop");
        writeUleb128(os, s->index, "segment index immediate");
      }
    }

    // End the function
    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::initMemory, bodyContent);
}

void Writer::createStartFunction() {
  // If the start function exists when we have more than one function to call.
  if (WasmSym::initMemory && WasmSym::applyGlobalRelocs) {
    assert(WasmSym::startFunction);
    std::string bodyContent;
    {
      raw_string_ostream os(bodyContent);
      writeUleb128(os, 0, "num locals");
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, WasmSym::applyGlobalRelocs->getFunctionIndex(),
                   "function index");
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, WasmSym::initMemory->getFunctionIndex(),
                   "function index");
      writeU8(os, WASM_OPCODE_END, "END");
    }
    createFunction(WasmSym::startFunction, bodyContent);
  } else if (WasmSym::initMemory) {
    WasmSym::startFunction = WasmSym::initMemory;
  } else if (WasmSym::applyGlobalRelocs) {
    WasmSym::startFunction = WasmSym::applyGlobalRelocs;
  }
}

// For -shared (PIC) output, we create create a synthetic function which will
// apply any relocations to the data segments on startup.  This function is
// called `__wasm_apply_data_relocs` and is expected to be called before
// any user code (i.e. before `__wasm_call_ctors`).
void Writer::createApplyDataRelocationsFunction() {
  LLVM_DEBUG(dbgs() << "createApplyDataRelocationsFunction\n");
  // First write the body's contents to a string.
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");
    for (const OutputSegment *seg : segments)
      if (!config->sharedMemory || !seg->isTLS())
        for (const InputChunk *inSeg : seg->inputSegments)
          inSeg->generateRelocationCode(os);

    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::applyDataRelocs, bodyContent);
}

void Writer::createApplyTLSRelocationsFunction() {
  LLVM_DEBUG(dbgs() << "createApplyTLSRelocationsFunction\n");
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");
    for (const OutputSegment *seg : segments)
      if (seg->isTLS())
        for (const InputChunk *inSeg : seg->inputSegments)
          inSeg->generateRelocationCode(os);

    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::applyTLSRelocs, bodyContent);
}

// Similar to createApplyDataRelocationsFunction but generates relocation code
// for WebAssembly globals. Because these globals are not shared between threads
// these relocation need to run on every thread.
void Writer::createApplyGlobalRelocationsFunction() {
  // First write the body's contents to a string.
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");
    out.globalSec->generateRelocationCode(os, false);
    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::applyGlobalRelocs, bodyContent);
}

// Similar to createApplyGlobalRelocationsFunction but for
// TLS symbols.  This cannot be run during the start function
// but must be delayed until __wasm_init_tls is called.
void Writer::createApplyGlobalTLSRelocationsFunction() {
  // First write the body's contents to a string.
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");
    out.globalSec->generateRelocationCode(os, true);
    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::applyGlobalTLSRelocs, bodyContent);
}

// Create synthetic "__wasm_call_ctors" function based on ctor functions
// in input object.
void Writer::createCallCtorsFunction() {
  // If __wasm_call_ctors isn't referenced, there aren't any ctors, don't
  // define the `__wasm_call_ctors` function.
  if (!WasmSym::callCtors->isLive() && initFunctions.empty())
    return;

  // First write the body's contents to a string.
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");

    // Call constructors
    for (const WasmInitEntry &f : initFunctions) {
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, f.sym->getFunctionIndex(), "function index");
      for (size_t i = 0; i < f.sym->signature->Returns.size(); i++) {
        writeU8(os, WASM_OPCODE_DROP, "DROP");
      }
    }

    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(WasmSym::callCtors, bodyContent);
}

// Create a wrapper around a function export which calls the
// static constructors and destructors.
void Writer::createCommandExportWrapper(uint32_t functionIndex,
                                        DefinedFunction *f) {
  // First write the body's contents to a string.
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);
    writeUleb128(os, 0, "num locals");

    // Call `__wasm_call_ctors` which call static constructors (and
    // applies any runtime relocations in Emscripten-style PIC mode)
    if (WasmSym::callCtors->isLive()) {
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, WasmSym::callCtors->getFunctionIndex(),
                   "function index");
    }

    // Call the user's code, leaving any return values on the operand stack.
    for (size_t i = 0; i < f->signature->Params.size(); ++i) {
      writeU8(os, WASM_OPCODE_LOCAL_GET, "local.get");
      writeUleb128(os, i, "local index");
    }
    writeU8(os, WASM_OPCODE_CALL, "CALL");
    writeUleb128(os, functionIndex, "function index");

    // Call the function that calls the destructors.
    if (DefinedFunction *callDtors = WasmSym::callDtors) {
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, callDtors->getFunctionIndex(), "function index");
    }

    // End the function, returning the return values from the user's code.
    writeU8(os, WASM_OPCODE_END, "END");
  }

  createFunction(f, bodyContent);
}

void Writer::createInitTLSFunction() {
  std::string bodyContent;
  {
    raw_string_ostream os(bodyContent);

    OutputSegment *tlsSeg = nullptr;
    for (auto *seg : segments) {
      if (seg->name == ".tdata") {
        tlsSeg = seg;
        break;
      }
    }

    writeUleb128(os, 0, "num locals");
    if (tlsSeg) {
      writeU8(os, WASM_OPCODE_LOCAL_GET, "local.get");
      writeUleb128(os, 0, "local index");

      writeU8(os, WASM_OPCODE_GLOBAL_SET, "global.set");
      writeUleb128(os, WasmSym::tlsBase->getGlobalIndex(), "global index");

      // FIXME(wvo): this local needs to be I64 in wasm64, or we need an extend op.
      writeU8(os, WASM_OPCODE_LOCAL_GET, "local.get");
      writeUleb128(os, 0, "local index");

      writeI32Const(os, 0, "segment offset");

      writeI32Const(os, tlsSeg->size, "memory region size");

      writeU8(os, WASM_OPCODE_MISC_PREFIX, "bulk-memory prefix");
      writeUleb128(os, WASM_OPCODE_MEMORY_INIT, "MEMORY.INIT");
      writeUleb128(os, tlsSeg->index, "segment index immediate");
      writeU8(os, 0, "memory index immediate");
    }

    if (WasmSym::applyTLSRelocs) {
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, WasmSym::applyTLSRelocs->getFunctionIndex(),
                   "function index");
    }

    if (WasmSym::applyGlobalTLSRelocs) {
      writeU8(os, WASM_OPCODE_CALL, "CALL");
      writeUleb128(os, WasmSym::applyGlobalTLSRelocs->getFunctionIndex(),
                   "function index");
    }
    writeU8(os, WASM_OPCODE_END, "end function");
  }

  createFunction(WasmSym::initTLS, bodyContent);
}

// Populate InitFunctions vector with init functions from all input objects.
// This is then used either when creating the output linking section or to
// synthesize the "__wasm_call_ctors" function.
void Writer::calculateInitFunctions() {
  if (!config->relocatable && !WasmSym::callCtors->isLive())
    return;

  for (ObjFile *file : symtab->objectFiles) {
    const WasmLinkingData &l = file->getWasmObj()->linkingData();
    for (const WasmInitFunc &f : l.InitFunctions) {
      FunctionSymbol *sym = file->getFunctionSymbol(f.Symbol);
      // comdat exclusions can cause init functions be discarded.
      if (sym->isDiscarded() || !sym->isLive())
        continue;
      if (sym->signature->Params.size() != 0)
        error("constructor functions cannot take arguments: " + toString(*sym));
      LLVM_DEBUG(dbgs() << "initFunctions: " << toString(*sym) << "\n");
      initFunctions.emplace_back(WasmInitEntry{sym, f.Priority});
    }
  }

  // Sort in order of priority (lowest first) so that they are called
  // in the correct order.
  llvm::stable_sort(initFunctions,
                    [](const WasmInitEntry &l, const WasmInitEntry &r) {
                      return l.priority < r.priority;
                    });
}

void Writer::createSyntheticSections() {
  out.dylinkSec = make<DylinkSection>();
  out.typeSec = make<TypeSection>();
  out.importSec = make<ImportSection>();
  out.functionSec = make<FunctionSection>();
  out.tableSec = make<TableSection>();
  out.memorySec = make<MemorySection>();
  out.tagSec = make<TagSection>();
  out.globalSec = make<GlobalSection>();
  out.exportSec = make<ExportSection>();
  out.startSec = make<StartSection>();
  out.elemSec = make<ElemSection>();
  out.producersSec = make<ProducersSection>();
  out.targetFeaturesSec = make<TargetFeaturesSection>();
  out.buildIdSec = make<BuildIdSection>();
}

void Writer::createSyntheticSectionsPostLayout() {
  out.dataCountSec = make<DataCountSection>(segments);
  out.linkingSec = make<LinkingSection>(initFunctions, segments);
  out.nameSec = make<NameSection>(segments);
}

// <change>
// void Writer::run() {
void Writer::run(std::unique_ptr<llvm::MemoryBuffer>& out_buffer) {
// </change>
  // For PIC code the table base is assigned dynamically by the loader.
  // For non-PIC, we start at 1 so that accessing table index 0 always traps.
  if (!config->isPic) {
    config->tableBase = 1;
    if (WasmSym::definedTableBase)
      WasmSym::definedTableBase->setVA(config->tableBase);
    if (WasmSym::definedTableBase32)
      WasmSym::definedTableBase32->setVA(config->tableBase);
  }

  log("-- createOutputSegments");
  createOutputSegments();
  log("-- createSyntheticSections");
  createSyntheticSections();
  log("-- layoutMemory");
  layoutMemory();

  if (!config->relocatable) {
    // Create linker synthesized __start_SECNAME/__stop_SECNAME symbols
    // This has to be done after memory layout is performed.
    for (const OutputSegment *seg : segments) {
      addStartStopSymbols(seg);
    }
  }

  for (auto &pair : config->exportedSymbols) {
    Symbol *sym = symtab->find(pair.first());
    if (sym && sym->isDefined())
      sym->forceExport = true;
  }

  // Delay reporting errors about explicit exports until after
  // addStartStopSymbols which can create optional symbols.
  for (auto &name : config->requiredExports) {
    Symbol *sym = symtab->find(name);
    if (!sym || !sym->isDefined()) {
      if (config->unresolvedSymbols == UnresolvedPolicy::ReportError)
        error(Twine("symbol exported via --export not found: ") + name);
      if (config->unresolvedSymbols == UnresolvedPolicy::Warn)
        warn(Twine("symbol exported via --export not found: ") + name);
    }
  }

  log("-- populateTargetFeatures");
  populateTargetFeatures();

  // When outputting PIC code each segment lives at at fixes offset from the
  // `__memory_base` import.  Unless we support the extended const expression we
  // can't do addition inside the constant expression, so we much combine the
  // segments into a single one that can live at `__memory_base`.
  if (config->isPic && !config->extendedConst && !config->sharedMemory) {
    // In shared memory mode all data segments are passive and initialized
    // via __wasm_init_memory.
    log("-- combineOutputSegments");
    combineOutputSegments();
  }

  log("-- createSyntheticSectionsPostLayout");
  createSyntheticSectionsPostLayout();
  log("-- populateProducers");
  populateProducers();
  log("-- calculateImports");
  calculateImports();
  log("-- scanRelocations");
  scanRelocations();
  log("-- finalizeIndirectFunctionTable");
  finalizeIndirectFunctionTable();
  log("-- createSyntheticInitFunctions");
  createSyntheticInitFunctions();
  log("-- assignIndexes");
  assignIndexes();
  log("-- calculateInitFunctions");
  calculateInitFunctions();

  if (!config->relocatable) {
    // Create linker synthesized functions
    if (WasmSym::applyDataRelocs)
      createApplyDataRelocationsFunction();
    if (WasmSym::applyGlobalRelocs)
      createApplyGlobalRelocationsFunction();
    if (WasmSym::applyTLSRelocs)
      createApplyTLSRelocationsFunction();
    if (WasmSym::applyGlobalTLSRelocs)
      createApplyGlobalTLSRelocationsFunction();
    if (WasmSym::initMemory)
      createInitMemoryFunction();
    createStartFunction();

    createCallCtorsFunction();

    // Create export wrappers for commands if needed.
    //
    // If the input contains a call to `__wasm_call_ctors`, either in one of
    // the input objects or an explicit export from the command-line, we
    // assume ctors and dtors are taken care of already.
    if (!config->relocatable && !config->isPic &&
        !WasmSym::callCtors->isUsedInRegularObj &&
        !WasmSym::callCtors->isExported()) {
      log("-- createCommandExportWrappers");
      createCommandExportWrappers();
    }
  }

  if (WasmSym::initTLS && WasmSym::initTLS->isLive()) {
    log("-- createInitTLSFunction");
    createInitTLSFunction();
  }

  if (errorCount())
    return;

  log("-- calculateTypes");
  calculateTypes();
  log("-- calculateExports");
  calculateExports();
  log("-- calculateCustomSections");
  calculateCustomSections();
  log("-- populateSymtab");
  populateSymtab();
  log("-- checkImportExportTargetFeatures");
  checkImportExportTargetFeatures();
  log("-- addSections");
  addSections();

  if (errorHandler().verbose) {
    log("Defined Functions: " + Twine(out.functionSec->inputFunctions.size()));
    log("Defined Globals  : " + Twine(out.globalSec->numGlobals()));
    log("Defined Tags     : " + Twine(out.tagSec->inputTags.size()));
    log("Defined Tables   : " + Twine(out.tableSec->inputTables.size()));
    log("Function Imports : " +
        Twine(out.importSec->getNumImportedFunctions()));
    log("Global Imports   : " + Twine(out.importSec->getNumImportedGlobals()));
    log("Tag Imports      : " + Twine(out.importSec->getNumImportedTags()));
    log("Table Imports    : " + Twine(out.importSec->getNumImportedTables()));
  }

  createHeader();
  log("-- finalizeSections");
  finalizeSections();

  log("-- writeMapFile");
  writeMapFile(outputSections);

  log("-- openFile");
// <change>
//   openFile();
  openFile(out_buffer);
// </change>
  if (errorCount())
    return;

  writeHeader();

  log("-- writeSections");
  writeSections();
  writeBuildId();
  if (errorCount())
    return;

  if (Error e = buffer->commit())
    fatal("failed to write output '" + buffer->getPath() +
          "': " + toString(std::move(e)));
}

// Open a result file.
// <change>
// void Writer::openFile() {
void Writer::openFile(std::unique_ptr<MemoryBuffer>& out_buffer) {
// </change>
  log("writing: " + config->outputFile);

// <change>
//  Expected<std::unique_ptr<FileOutputBuffer>> bufferOrErr =
//      FileOutputBuffer::create(config->outputFile, fileSize,
//                               FileOutputBuffer::F_executable);
//
//  if (!bufferOrErr)
//    error("failed to open " + config->outputFile + ": " +
//          toString(bufferOrErr.takeError()));
//  else
//    buffer = std::move(*bufferOrErr);

  out_buffer = WritableMemoryBuffer::getNewMemBuffer(fileSize);
  buffer = std::make_unique<InMemoryBuffer>(out_buffer.get());
// </change>
}

void Writer::createHeader() {
  raw_string_ostream os(header);
  writeBytes(os, WasmMagic, sizeof(WasmMagic), "wasm magic");
  writeU32(os, WasmVersion, "wasm version");
  os.flush();
  fileSize += header.size();
}

// <change>
// void writeResult() { Writer().run(); }

} // namespace wasm::lld

namespace rain::lang::code::wasm {

std::unique_ptr<llvm::MemoryBuffer> writeResult() {
    std::unique_ptr<llvm::MemoryBuffer> out_buffer;
    lld::wasm::Writer().run(out_buffer);
    return out_buffer;
}

} // namespace rain::lang::code::wasm
// </change>
