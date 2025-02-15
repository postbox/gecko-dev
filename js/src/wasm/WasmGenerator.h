/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=8 sts=4 et sw=4 tw=99:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef wasm_generator_h
#define wasm_generator_h

#include "jit/MacroAssembler.h"
#include "wasm/WasmModule.h"
#include "wasm/WasmValidate.h"

namespace js {
namespace wasm {

struct ModuleEnvironment;

typedef Vector<jit::MIRType, 8, SystemAllocPolicy> MIRTypeVector;
typedef jit::ABIArgIter<MIRTypeVector> ABIArgMIRTypeIter;
typedef jit::ABIArgIter<ValTypeVector> ABIArgValTypeIter;

struct CompileArgs;

class FunctionGenerator;

typedef Vector<UniqueBytes, 0, SystemAllocPolicy> UniqueBytesVector;

// The FuncBytes class represents a single, concurrently-compilable function.
// A FuncBytes object is composed of the wasm function body bytes along with the
// ambient metadata describing the function necessary to compile it.

class FuncBytes
{
    UniqueBytes      bytes_;
    uint32_t         index_;
    const SigWithId& sig_;
    uint32_t         lineOrBytecode_;
    Uint32Vector     callSiteLineNums_;

  public:
    FuncBytes(UniqueBytes bytes,
              uint32_t index,
              const SigWithId& sig,
              uint32_t lineOrBytecode,
              Uint32Vector&& callSiteLineNums)
      : bytes_(Move(bytes)),
        index_(index),
        sig_(sig),
        lineOrBytecode_(lineOrBytecode),
        callSiteLineNums_(Move(callSiteLineNums))
    {}

    Bytes& bytes() { return *bytes_; }
    const Bytes& bytes() const { return *bytes_; }
    UniqueBytes recycle() { return Move(bytes_); }
    uint32_t index() const { return index_; }
    const SigWithId& sig() const { return sig_; }
    uint32_t lineOrBytecode() const { return lineOrBytecode_; }
    const Uint32Vector& callSiteLineNums() const { return callSiteLineNums_; }
};

typedef UniquePtr<FuncBytes> UniqueFuncBytes;

// The FuncCompileResults class contains the results of compiling a single
// function body, ready to be merged into the whole-module MacroAssembler.

class FuncCompileResults
{
    jit::TempAllocator alloc_;
    jit::MacroAssembler masm_;
    FuncOffsets offsets_;

    FuncCompileResults(const FuncCompileResults&) = delete;
    FuncCompileResults& operator=(const FuncCompileResults&) = delete;

  public:
    explicit FuncCompileResults(LifoAlloc& lifo)
      : alloc_(&lifo),
        masm_(jit::MacroAssembler::WasmToken(), alloc_)
    {}

    jit::TempAllocator& alloc() { return alloc_; }
    jit::MacroAssembler& masm() { return masm_; }
    FuncOffsets& offsets() { return offsets_; }
};

// A CompileTask represents the task of compiling a single function body. An
// CompileTask is filled with the wasm code to be compiled on the main
// validation thread, sent off to a compilation helper thread which creates
// the FuncCompileResults, and finally sent back to the validation thread. To
// save time allocating and freeing memory, CompileTasks are reset() and
// reused.

class CompileTask
{
  public:
    enum class CompileMode { None, Baseline, Ion };

  private:
    const ModuleEnvironment&  env_;
    LifoAlloc                 lifo_;
    UniqueFuncBytes           func_;
    CompileMode               mode_;
    Maybe<FuncCompileResults> results_;

    CompileTask(const CompileTask&) = delete;
    CompileTask& operator=(const CompileTask&) = delete;

  public:
    CompileTask(const ModuleEnvironment& env, size_t defaultChunkSize)
      : env_(env), lifo_(defaultChunkSize), func_(nullptr), mode_(CompileMode::None)
    {}
    LifoAlloc& lifo() {
        return lifo_;
    }
    const ModuleEnvironment& env() const {
        return env_;
    }
    void init(UniqueFuncBytes func, CompileMode mode) {
        MOZ_ASSERT(!func_);
        func_ = Move(func);
        results_.emplace(lifo_);
        mode_ = mode;
    }
    CompileMode mode() const {
        return mode_;
    }
    const FuncBytes& func() const {
        MOZ_ASSERT(func_);
        return *func_;
    }
    FuncCompileResults& results() {
        return *results_;
    }
    void reset(UniqueBytes* recycled) {
        if (func_) {
            *recycled = Move(func_->recycle());
            (*recycled)->clear();
        }
        func_.reset(nullptr);
        results_.reset();
        lifo_.releaseAll();
        mode_ = CompileMode::None;
    }
};

// A ModuleGenerator encapsulates the creation of a wasm module. During the
// lifetime of a ModuleGenerator, a sequence of FunctionGenerators are created
// and destroyed to compile the individual function bodies. After generating all
// functions, ModuleGenerator::finish() must be called to complete the
// compilation and extract the resulting wasm module.

class MOZ_STACK_CLASS ModuleGenerator
{
    typedef HashSet<uint32_t, DefaultHasher<uint32_t>, SystemAllocPolicy> Uint32Set;
    typedef Vector<CompileTask, 0, SystemAllocPolicy> CompileTaskVector;
    typedef Vector<CompileTask*, 0, SystemAllocPolicy> CompileTaskPtrVector;
    typedef EnumeratedArray<Trap, Trap::Limit, ProfilingOffsets> TrapExitOffsetArray;

    // Constant parameters
    bool                            alwaysBaseline_;

    // Data that is moved into the result of finish()
    Assumptions                     assumptions_;
    LinkData                        linkData_;
    MutableMetadata                 metadata_;

    // Data scoped to the ModuleGenerator's lifetime
    UniqueModuleEnvironment         env_;
    uint32_t                        numSigs_;
    uint32_t                        numTables_;
    LifoAlloc                       lifo_;
    jit::JitContext                 jcx_;
    jit::TempAllocator              masmAlloc_;
    jit::MacroAssembler             masm_;
    Uint32Vector                    funcToCodeRange_;
    Uint32Set                       exportedFuncs_;
    uint32_t                        lastPatchedCallsite_;
    uint32_t                        startOfUnpatchedCallsites_;

    // Parallel compilation
    bool                            parallel_;
    uint32_t                        outstanding_;
    CompileTaskVector               tasks_;
    CompileTaskPtrVector            freeTasks_;
    UniqueBytesVector               freeBytes_;

    // Assertions
    DebugOnly<FunctionGenerator*>   activeFuncDef_;
    DebugOnly<bool>                 startedFuncDefs_;
    DebugOnly<bool>                 finishedFuncDefs_;
    DebugOnly<uint32_t>             numFinishedFuncDefs_;

    bool funcIsCompiled(uint32_t funcIndex) const;
    const CodeRange& funcCodeRange(uint32_t funcIndex) const;
    MOZ_MUST_USE bool patchCallSites(TrapExitOffsetArray* maybeTrapExits = nullptr);
    MOZ_MUST_USE bool patchFarJumps(const TrapExitOffsetArray& trapExits);
    MOZ_MUST_USE bool finishTask(CompileTask* task);
    MOZ_MUST_USE bool finishOutstandingTask();
    MOZ_MUST_USE bool finishFuncExports();
    MOZ_MUST_USE bool finishCodegen();
    MOZ_MUST_USE bool finishLinkData(Bytes& code);
    MOZ_MUST_USE bool addFuncImport(const Sig& sig, uint32_t globalDataOffset);
    MOZ_MUST_USE bool allocateGlobalBytes(uint32_t bytes, uint32_t align, uint32_t* globalDataOff);
    MOZ_MUST_USE bool allocateGlobal(GlobalDesc* global);

    MOZ_MUST_USE bool initAsmJS(Metadata* asmJSMetadata);
    MOZ_MUST_USE bool initWasm();

    // Functions declarations:
    uint32_t numFuncImports() const;

  public:
    explicit ModuleGenerator();
    ~ModuleGenerator();

    MOZ_MUST_USE bool init(UniqueModuleEnvironment env, const CompileArgs& args,
                           Metadata* maybeAsmJSMetadata = nullptr);

    const ModuleEnvironment& env() const { return *env_; }

    bool isAsmJS() const { return metadata_->kind == ModuleKind::AsmJS; }
    jit::MacroAssembler& masm() { return masm_; }

    // Memory:
    bool usesMemory() const { return env_->usesMemory(); }
    uint32_t minMemoryLength() const { return env_->minMemoryLength; }

    // Tables:
    uint32_t numTables() const { return numTables_; }
    const TableDescVector& tables() const { return env_->tables; }

    // Signatures:
    uint32_t numSigs() const { return numSigs_; }
    const SigWithId& sig(uint32_t sigIndex) const;
    const SigWithId& funcSig(uint32_t funcIndex) const;
    const SigWithIdPtrVector& funcSigs() const { return env_->funcSigs; }

    // Globals:
    const GlobalDescVector& globals() const { return env_->globals; }

    // Function definitions:
    MOZ_MUST_USE bool startFuncDefs();
    MOZ_MUST_USE bool startFuncDef(uint32_t lineOrBytecode, FunctionGenerator* fg);
    MOZ_MUST_USE bool finishFuncDef(uint32_t funcIndex, FunctionGenerator* fg);
    MOZ_MUST_USE bool finishFuncDefs();

    // asm.js lazy initialization:
    void initSig(uint32_t sigIndex, Sig&& sig);
    void initFuncSig(uint32_t funcIndex, uint32_t sigIndex);
    MOZ_MUST_USE bool initImport(uint32_t funcIndex, uint32_t sigIndex);
    MOZ_MUST_USE bool initSigTableLength(uint32_t sigIndex, uint32_t length);
    MOZ_MUST_USE bool initSigTableElems(uint32_t sigIndex, Uint32Vector&& elemFuncIndices);
    void initMemoryUsage(MemoryUsage memoryUsage);
    void bumpMinMemoryLength(uint32_t newMinMemoryLength);
    MOZ_MUST_USE bool addGlobal(ValType type, bool isConst, uint32_t* index);
    MOZ_MUST_USE bool addExport(CacheableChars&& fieldChars, uint32_t funcIndex);

    // Finish compilation, provided the list of imports and source bytecode.
    // Both these Vectors may be empty (viz., b/c asm.js does different things
    // for imports and source).
    SharedModule finish(const ShareableBytes& bytecode, DataSegmentVector&& dataSegments,
                        NameInBytecodeVector&& funcNames);
};

// A FunctionGenerator encapsulates the generation of a single function body.
// ModuleGenerator::startFunc must be called after construction and before doing
// anything else. After the body is complete, ModuleGenerator::finishFunc must
// be called before the FunctionGenerator is destroyed and the next function is
// started.

class MOZ_STACK_CLASS FunctionGenerator
{
    friend class ModuleGenerator;

    ModuleGenerator* m_;
    bool             usesSimd_;
    bool             usesAtomics_;

    // Data created during function generation, then handed over to the
    // FuncBytes in ModuleGenerator::finishFunc().
    UniqueBytes      bytes_;
    Uint32Vector     callSiteLineNums_;

    uint32_t lineOrBytecode_;

  public:
    FunctionGenerator()
      : m_(nullptr), usesSimd_(false), usesAtomics_(false), bytes_(nullptr), lineOrBytecode_(0)
    {}

    bool usesSimd() const {
        return usesSimd_;
    }
    void setUsesSimd() {
        usesSimd_ = true;
    }

    bool usesAtomics() const {
        return usesAtomics_;
    }
    void setUsesAtomics() {
        usesAtomics_ = true;
    }

    Bytes& bytes() {
        return *bytes_;
    }
    MOZ_MUST_USE bool addCallSiteLineNum(uint32_t lineno) {
        return callSiteLineNums_.append(lineno);
    }
};

} // namespace wasm
} // namespace js

#endif // wasm_generator_h
