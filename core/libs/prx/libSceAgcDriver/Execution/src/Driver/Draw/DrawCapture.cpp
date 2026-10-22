#include "prx/libSceAgcDriver/Execution/include/PerformanceTimer.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Shaders/ShaderRegistry.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include "prx/libSceAgcDriver/Execution/include/Pm4.hpp"
#include "Optimization/ResourceProgram.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <span>
#include <sstream>
#include <stdexcept>

namespace AgcDriver::DriverDetail {

namespace {

bool SameVertexShape(const ShaderRecompiler::ShaderVertexStageInfo* kept, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& now) {
    if (kept == nullptr || !now) return kept == nullptr && !now;
    if (kept->resourcesNum != now->resourcesNum || kept->fetchAttribReg != now->fetchAttribReg || kept->fetchBufferReg != now->fetchBufferReg || kept->fetchEmbedded != now->fetchEmbedded || kept->paClVsOutCntl != now->paClVsOutCntl || kept->depthRemapMode != now->depthRemapMode) return false;
    for (std::uint32_t i = 0; i < kept->resourcesNum && i < kept->resources.size(); ++i) {
        if (kept->resources[i].fields[3] != now->resources[i].fields[3]) return false;
        const auto& a = kept->resourcesDst[i];
        const auto& b = now->resourcesDst[i];
        if (a.registerStart != b.registerStart || a.registersNum != b.registersNum || a.attrId != b.attrId || a.fetchIndex != b.fetchIndex) return false;
    }
    return true;
}

bool WalkRegions(const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> validated, std::vector<ShaderRecompiler::MemoryRegion>& regions) {
    regions.clear();
    if (!variant.walkKnown) return false;
    for (const auto& [begin, end] : variant.walkSpans) {
        auto cursor = begin;
        while (cursor < end) {
            const auto holder = std::find_if(validated.begin(), validated.end(), [&](const ShaderRecompiler::MemoryRegion& region) { return cursor >= region.guestAddress && cursor < region.guestAddress + region.bytes.size(); });
            if (holder == validated.end()) return false;
            const auto to = std::min<std::uint64_t>(end, holder->guestAddress + holder->bytes.size());
            regions.push_back({cursor, holder->bytes.subspan(static_cast<std::size_t>(cursor - holder->guestAddress), static_cast<std::size_t>(to - cursor))});
            cursor = to;
        }
    }
    return true;
}

std::array<std::uint32_t, 3> BufferShape(std::span<const std::uint32_t> words) {
    const bool present = words[2] != 0u && (words[0] != 0u || (words[1] & 0xffffu) != 0u);
    return {words[1] & 0xffff0000u, words[3], present ? 4u | (words[0] & 3u) : 0u};
}

bool ImageShapeEqual(std::span<const std::uint32_t> a, std::span<const std::uint32_t> b) {
    const bool presentA = a[0] != 0u || (a[1] & 0xffu) != 0u;
    const bool presentB = b[0] != 0u || (b[1] & 0xffu) != 0u;
    return presentA == presentB && (a[1] & ~0xffu) == (b[1] & ~0xffu) && std::equal(a.begin() + 2, a.end(), b.begin() + 2);
}

bool SameBinding(const ShaderRecompiler::DescriptorBinding& a, const ShaderRecompiler::DescriptorBinding& b) {
    return a.kind == b.kind && a.role == b.role && a.descriptorSet == b.descriptorSet && a.binding == b.binding && a.count == b.count && a.guestDescriptor == b.guestDescriptor && a.readOnly == b.readOnly && a.samplerDepthCompare == b.samplerDepthCompare && a.imageWritten == b.imageWritten && a.imageDepthCompare == b.imageDepthCompare && a.imageAtomic == b.imageAtomic && a.imageAtomic64 == b.imageAtomic64 && a.bufferAtomic == b.bufferAtomic && a.bufferWritten == b.bufferWritten && a.samplerUnnormalized == b.samplerUnnormalized && a.samplerIntegerBorder == b.samplerIntegerBorder && a.samplerCoordinatesIgnored == b.samplerCoordinatesIgnored && a.imageUnnormalized == b.imageUnnormalized && a.imageUnnormalizedBaseLevel == b.imageUnnormalizedBaseLevel && a.imageSamplers == b.imageSamplers && a.bufferRead == b.bufferRead && a.bufferExtent == b.bufferExtent && a.imageShapes.size() == b.imageShapes.size() && a.imageShape.has_value() == b.imageShape.has_value();
}

const char* SharedResultDifference(const ShaderRecompiler::RecompileResult& shared, const ShaderRecompiler::RecompileResult& full) {
    if (shared.Identity() != full.Identity() || shared.specializationId != full.specializationId || shared.variantId != full.variantId) return "module";
    if (shared.specialization.size() != full.specialization.size()) return "specialization";
    for (std::size_t i = 0; i < shared.specialization.size(); ++i) {
        if (shared.specialization[i].id != full.specialization[i].id || shared.specialization[i].value != full.specialization[i].value) return "specialization";
    }
    if (shared.bindings.size() != full.bindings.size()) return "binding count";
    for (std::size_t i = 0; i < shared.bindings.size(); ++i) {
        if (!SameBinding(shared.bindings[i], full.bindings[i])) return "bindings";
    }
    if (shared.pushConstants != full.pushConstants) return "push constants";
    if (shared.vertexAttributes.size() != full.vertexAttributes.size()) return "vertex attribute count";
    for (std::size_t i = 0; i < shared.vertexAttributes.size(); ++i) {
        const auto& a = shared.vertexAttributes[i];
        const auto& b = full.vertexAttributes[i];
        if (a.location != b.location || a.components != b.components || a.resource.fields != b.resource.fields || a.fetchIndex != b.fetchIndex || a.formatComponents != b.formatComponents) return "vertex attributes";
    }
    if (shared.shaderDataUserWords != full.shaderDataUserWords || shared.userDataRegisterReads != full.userDataRegisterReads || shared.poisonedSrtReads != full.poisonedSrtReads || shared.readsDepthRemapPush != full.readsDepthRemapPush || shared.workgroupMemoryDwords != full.workgroupMemoryDwords) return "invocation fields";
    if (!shared.imageTablePoison.empty() || !full.imageTablePoison.empty() || shared.imageTableRanges != full.imageTableRanges || shared.imageTableShader != full.imageTableShader) return "image tables";
    return nullptr;
}

}

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::shareDrawStage(const DispatchVariant& variant, std::span<const ShaderRecompiler::MemoryRegion> validated, const DrawProgram& program, const std::optional<ShaderRecompiler::ShaderVertexStageInfo>& vertexInfo, std::uint32_t pushOffset, StageCapture& stageCapture) {
    const auto count = [&](std::uint64_t DrawEntryCounters::*counter) {
        std::lock_guard cacheLock(drawCacheMutex);
        ++(drawEntryCounters.*counter);
    };
    if (variant.compiled == nullptr) return nullptr;
    if (variant.pushOffset != pushOffset) {
        count(&DrawEntryCounters::sharedPushOffset);
        return nullptr;
    }
    if (!SameVertexShape(variant.vertexInfo.get(), vertexInfo)) {
        count(&DrawEntryCounters::sharedVertexShape);
        return nullptr;
    }
    const auto& kept = *variant.compiled;
    if (!kept.imageTablePoison.empty() || !kept.imageTableRanges.empty()) return nullptr;
    std::vector<ShaderRecompiler::MemoryRegion> walked;
    if (!WalkRegions(variant, validated, walked)) return nullptr;
    const auto& now = program.userData;
    auto shared = std::make_shared<ShaderRecompiler::RecompileResult>(kept);
    using Field = ShaderRecompiler::ValueField;
    const auto stride = [](const ShaderRecompiler::ValuePosition& position) { return position.field == Field::Buffer ? 4u : 8u; };
    const auto first = [&](const ShaderRecompiler::ValuePosition& position) { return position.field == Field::Buffer || position.field == Field::Image ? static_cast<std::size_t>(position.element) * stride(position) : static_cast<std::size_t>(position.element); };
    const auto element = [&](const ShaderRecompiler::RecompileResult& result, const ShaderRecompiler::ValuePosition& position, std::array<std::uint32_t, 8>& words) {
        const auto size = stride(position);
        const auto at = first(position);
        if (position.field == Field::PushImage) {
            if ((at + size) * sizeof(std::uint32_t) > result.pushConstants.size()) return false;
            std::memcpy(words.data(), result.pushConstants.data() + at * sizeof(std::uint32_t), size * sizeof(std::uint32_t));
            return true;
        }
        if (position.binding >= result.bindings.size() || at + size > result.bindings[position.binding].guestDescriptor.size()) return false;
        std::copy_n(result.bindings[position.binding].guestDescriptor.begin() + static_cast<std::ptrdiff_t>(at), size, words.begin());
        return true;
    };
    for (const auto& position : variant.valuePositions) {
        std::array<std::uint32_t, 8> words{};
        if (position.word >= now.size() || !element(*shared, position, words)) return nullptr;
        const auto at = first(position) + position.dword;
        const auto value = now[position.word];
        if (position.field == Field::PushImage) std::memcpy(shared->pushConstants.data() + at * sizeof(std::uint32_t), &value, sizeof(value));
        else shared->bindings[position.binding].guestDescriptor[at] = value;
    }
    for (const auto& position : variant.valuePositions) {
        std::array<std::uint32_t, 8> before{};
        std::array<std::uint32_t, 8> after{};
        if (!element(kept, position, before) || !element(*shared, position, after)) return nullptr;
        const auto size = stride(position);
        const bool same = position.field == Field::Buffer ? BufferShape(std::span<const std::uint32_t>(before).first(size)) == BufferShape(std::span<const std::uint32_t>(after).first(size)) : ImageShapeEqual(before, after);
        if (!same) {
            count(&DrawEntryCounters::sharedDescriptorShape);
            return nullptr;
        }
    }
    const auto& forwarded = kept.shaderDataUserWords;
    const auto data = std::find_if(shared->bindings.begin(), shared->bindings.end(), [](const ShaderRecompiler::DescriptorBinding& binding) { return binding.role == ShaderRecompiler::DescriptorRole::ShaderData; });
    for (std::uint32_t slot = 0; slot < forwarded.size(); ++slot) {
        if (forwarded[slot] >= now.size()) return nullptr;
        const auto value = now[forwarded[slot]];
        const auto at = static_cast<std::size_t>(slot) * sizeof(std::uint32_t);
        if (at + sizeof(std::uint32_t) <= shared->pushConstants.size()) std::memcpy(shared->pushConstants.data() + at, &value, sizeof(value));
        if (data != shared->bindings.end() && slot < data->guestDescriptor.size()) data->guestDescriptor[slot] = value;
    }
    if (vertexInfo) {
        for (auto& attribute : shared->vertexAttributes) {
            if (attribute.location >= vertexInfo->resourcesNum || attribute.location >= vertexInfo->resources.size()) return nullptr;
            attribute.resource = vertexInfo->resources[attribute.location];
        }
    }
    stageCapture.compiled = shared;
    stageCapture.capture = nullptr;
    stageCapture.regions = std::move(walked);
    stageCapture.supplied.clear();
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    stageCapture.userDataUse = {};
    stageCapture.userDataUse.asValue = variant.walkedValues;
    stageCapture.userDataUse.asBase = variant.walkedBases;
    stageCapture.userDataMask = variant.userDataMask;
    stageCapture.valuePositions = variant.valuePositions;
    stageCapture.shareWords = variant.shareWords;
    stageCapture.shared = true;
    count(&DrawEntryCounters::sharedStages);
    return shared;
}

void Driver::verifySharedStage(std::size_t stage, std::uint64_t program, const DispatchVariant& variant, const ShaderRecompiler::RecompileResult& shared, const ShaderRecompiler::RecompileResult& full) {
    if (const auto* what = SharedResultDifference(shared, full)) {
        std::string detail;
        const auto& kept = *variant.compiled;
        for (std::size_t index = 0; index < shared.bindings.size() && index < full.bindings.size() && detail.empty(); ++index) {
            const auto& a = shared.bindings[index];
            const auto& b = full.bindings[index];
            if (SameBinding(a, b)) continue;
            char text[512];
            if (a.guestDescriptor != b.guestDescriptor) {
                std::size_t word = 0;
                while (word < a.guestDescriptor.size() && word < b.guestDescriptor.size() && a.guestDescriptor[word] == b.guestDescriptor[word]) ++word;
                const bool patched = std::any_of(variant.valuePositions.begin(), variant.valuePositions.end(), [&](const ShaderRecompiler::ValuePosition& position) {
                    if (position.field == ShaderRecompiler::ValueField::PushImage || position.binding != index) return false;
                    const auto start = position.field == ShaderRecompiler::ValueField::ShaderDataImage ? position.element : position.element * (position.field == ShaderRecompiler::ValueField::Image ? 8u : 4u);
                    return start + position.dword == word;
                });
                const auto keptWord = index < kept.bindings.size() && word < kept.bindings[index].guestDescriptor.size() ? kept.bindings[index].guestDescriptor[word] : 0u;
                std::snprintf(text, sizeof(text), ": binding %zu (role %u, kind %u, count %u) guest words %zu / %zu differ first at word %zu: shared 0x%08x, full 0x%08x, kept 0x%08x, %s; %zu value positions, share words 0x%llx", index, static_cast<unsigned>(a.role), static_cast<unsigned>(a.kind), a.count, a.guestDescriptor.size(), b.guestDescriptor.size(), word, word < a.guestDescriptor.size() ? a.guestDescriptor[word] : 0u, word < b.guestDescriptor.size() ? b.guestDescriptor[word] : 0u, keptWord, patched ? "patched" : "not patched", variant.valuePositions.size(), static_cast<unsigned long long>(variant.shareWords));
            } else {
                std::snprintf(text, sizeof(text), ": binding %zu (role %u, kind %u, count %u / %u) differs outside its guest words", index, static_cast<unsigned>(a.role), static_cast<unsigned>(a.kind), a.count, b.count);
            }
            detail = text;
        }
        const auto message = "APS5_VERIFY_DRAW_DATA_HITS: stage " + std::to_string(stage) + " of a shared draw stage differs from its full build in its " + what + detail;
        char program_[64];
        std::snprintf(program_, sizeof(program_), " (program 0x%llx)", static_cast<unsigned long long>(program));
        throw std::runtime_error(message + program_);
    }
    std::lock_guard cacheLock(drawCacheMutex);
    ++drawEntryCounters.sharedVerified;
}

std::shared_ptr<const ShaderRecompiler::RecompileResult> Driver::materializeDrawStage(std::size_t i, std::uint32_t pushOffset, const QueueState& queue, const Submission& submission, const std::vector<DrawProgram>& programs, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, std::vector<ShaderRecompiler::MemoryRegion>& memory, const std::vector<ShaderRecompiler::LinkedProgram>& linked, const Pm4::DrawParameters& drawParameters, const std::shared_ptr<VulkanDevice>& localDevice, ShaderMemory& shaderMemory, std::vector<StageCapture>& stageCaptures, std::vector<bool>& recompiled, bool drawHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, const std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool profile, std::uint64_t dumpTarget, std::uint64_t dumpSlot1, std::uint64_t& captures, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, std::string& rejected, const std::optional<ShaderRecompiler::MeshBarycentricLayout>& meshBarycentrics) try {
    PerformanceTimer timing("Shader.DrawStage");
    using Stage = ShaderRecompiler::ShaderStage;
    phaseTiming.Phase(DrawRowVectors);
    const auto& program = programs[i];
    const auto waveSize = program.binary.stage == Stage::Fragment ? graphics.stages.fragmentWaveSize : graphics.stages.vertexWaveSize;
    ShaderRecompiler::RecompileRequest request{
        program.binary,
        {waveSize, program.firstUserSgpr, program.userData, std::nullopt, program.binary.stage == Stage::Fragment ? std::optional(pixel) : std::nullopt, vertexInfos[i], memory, RegisteredFloatMode(*program.snapshot)},
        localDevice->Target(),
        {0, 0, pushOffset, (graphics.stages.mesh ? ShaderRecompiler::MeshDrawPushOffsetBytes : graphics.stages.tessellation && pushOffset < Graphics::PipelinePushSlotBytes ? ShaderRecompiler::TessellationLevelPushOffsetBytes : graphics.depthRemap.mode != 0u ? ShaderRecompiler::DepthRemapPushOffsetBytes : Graphics::PipelinePushSlotBytes) - pushOffset % Graphics::PipelinePushSlotBytes},
        ShaderRecompiler::GraphicsCompileContext{program.firstUserSgpr, linked, graphics.stages.mesh, graphics.stages.tessellation, {drawParameters.indexAddress, drawParameters.indexCount, drawParameters.indexSize, drawParameters.instanceCount}, program.binary.stage == Stage::Mesh ? meshBarycentrics : std::nullopt}
    };
    phaseTiming.Phase(DrawRowCaptureRequest);
    const auto waitedBefore = traceCapSync() || profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
    auto invocation = InvocationFor(*program.snapshot, program.codeOffset, request);
    phaseTiming.Phase(DrawRowCaptureInvocation);
    timing.Mark("prepared_invocation");
    auto& stageCapture = stageCaptures[i];
    stageCapture.forgetSerial = GuestMemory::ForgetSerial();
    stageCapture.pushOffset = pushOffset;
    const auto named = [&](const std::exception& error, std::span<const ShaderRecompiler::MemoryRegion> regions) {
        static const bool dumpShaders = std::getenv("APS5_DUMP_SHADERS") != nullptr;
        std::string dump;
        if (dumpShaders) {
            try {
                auto failed = request;
                failed.context.memory = regions;
                dump = dumpRequest(program.binary.codeAddress, failed, "_failed");
            } catch (...) {}
        }
        std::ostringstream where;
        where << "shader 0x" << std::hex << program.binary.codeAddress << " stage " << std::dec << static_cast<unsigned>(program.binary.stage);
        if (!dump.empty()) where << " (" << dump << ")";
        where << ": ";
        return std::runtime_error(where.str() + error.what());
    };
    const auto capture = [&] {
        const SampledReadScope sampling(evidenceReads);
        try {
            return CaptureInvocation(shaderMemory, *program.snapshot, program.codeOffset, request, invocation);
        } catch (const std::exception& error) {
            throw named(error, shaderMemory.Regions());
        }
    }();
    phaseTiming.Phase(DrawRowCaptureWalk);

    stageCapture.regions = shaderMemory.TakeRecentRegions();
    stageCapture.supplied = shaderMemory.SuppliedBlocks();
    stageCapture.userDataUse = capture->userDataUse;
    recompiled[i] = true;
    memory = shaderMemory.Regions();

    for (std::size_t j = 0; j < programs.size(); ++j) {
        if (matched[j] != nullptr && !recompiled[j] && (drawHit || j < i)) memory.insert(memory.end(), matchedRegions[j].begin(), matchedRegions[j].end());
    }
    request.context.memory = memory;
    if (traceCapSync()) traceCapture("draw-capture", program.binary.codeAddress, submission.queue, memory, Graphics::Recorder::ThreadWaitedMs() - waitedBefore);
    if (profile) {
        ++captures;
        phaseTiming.Phase(DrawRowCapture);

        const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBefore, phaseMs[DrawRowCaptureWalk]);
        phaseMs[DrawRowCaptureWalk] -= waited;
        phaseMs[DrawRowCaptureHookWaits] += waited;
    }
    if (dumpTarget != 0) {

        const auto slot0 = (static_cast<std::uint64_t>(readRegister(queue.context, 0x390)) << 40u) | (static_cast<std::uint64_t>(readRegister(queue.context, 0x318)) << 8u);
        if ((graphics.hasColorTarget && graphics.color.address == dumpTarget) || slot0 == dumpTarget) static_cast<void>(dumpRequest(program.binary.codeAddress, request));
    }
    if (dumpSlot1 != 0) {
        const auto value = [&](std::uint32_t offset) -> std::uint64_t { const auto it = queue.context.find(offset); return it == queue.context.end() ? 0u : it->second; };
        const auto slot1 = (value(0x391) << 40u) | (value(0x327) << 8u);
        if (slot1 == dumpSlot1) {
            static_cast<void>(dumpRequest(program.binary.codeAddress, request));
            if (std::FILE* file = std::fopen("draw_slot1.regs", "w")) {
                for (const auto& [offset, value] : queue.context) std::fprintf(file, "context %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.userConfig) std::fprintf(file, "uconfig %x %08x\n", offset, value);
                for (const auto& [offset, value] : queue.shader) std::fprintf(file, "shader %x %08x\n", offset, value);
                std::fclose(file);
            }
        }
    }

    phaseTiming.Phase(DrawRowCapture);

    timing.Mark("capture_resources");
    try {
        stageCapture.compiled = invocation.Materialize(*capture);
        if (drawShared()) {
            phaseTiming.Phase(DrawRowRecompile);
            stageCapture.shareWords = ShaderRecompiler::ValuePositions(*capture, *stageCapture.compiled, stageCapture.valuePositions);
            phaseTiming.Phase(DrawRowPositions);
        }
    } catch (const std::exception& error) {
        throw named(error, request.context.memory);
    }
    stageCapture.capture = capture;
    timing.Mark("materialize");
    const auto walked = capture->userDataUse.asValue | capture->userDataUse.asBase;
    auto mask = walked == 0 ? ~std::uint64_t{0} : walked | stageCapture.compiled->userDataRegisterReads;
    if (vertexInfos[i]) {
        for (const auto reg : {static_cast<std::uint32_t>(vertexInfos[i]->fetchAttribReg), static_cast<std::uint32_t>(vertexInfos[i]->fetchBufferReg)}) {
            mask |= reg + 1u < 64u ? std::uint64_t{3} << reg : ~std::uint64_t{0};
        }
    }
    stageCapture.userDataMask = mask;
    phaseTiming.Phase(DrawRowRecompile);
    return stageCapture.compiled;
} catch (const std::exception& error) {
    if (!SkipRefusedDraws()) throw;
    throw RefusedDraw(programs[i].binary.codeAddress, error.what());
}

void Driver::recordStageDataPositions(DispatchVariant& variant, const ShaderRecompiler::ResourceCapture& capture, const std::vector<Graphics::DecodeRead>& decodeReads) {
    const auto& compiled = *variant.compiled;
    for (std::size_t b = 0; b < compiled.bindings.size(); ++b) {
        if (compiled.bindings[b].role != ShaderRecompiler::DescriptorRole::FlattenedSrt) continue;
        variant.flatBinding = static_cast<std::uint32_t>(b);
        static_cast<void>(DataWordPositions(variant.runs, capture.readTrace.leaves, capture.readTrace.otherReads, variant.words, compiled.bindings[b].guestDescriptor, variant.dataPositions, variant.dataSlots));
        break;
    }
    if (variant.dataPositions.empty() || decodeReads.empty()) return;
    std::vector<bool> decoded(variant.words.size(), false);
    std::size_t offset = 0;
    for (const auto& [begin, end] : variant.runs) {
        for (const auto& read : decodeReads) {
            const auto from = std::max(begin, read.address);
            const auto to = std::min(end, read.address + read.size);
            if (from >= to) continue;
            for (auto word = (from - begin) / sizeof(std::uint32_t); word < (to - begin + sizeof(std::uint32_t) - 1) / sizeof(std::uint32_t); ++word) decoded[offset + static_cast<std::size_t>(word)] = true;
        }
        offset += static_cast<std::size_t>((end - begin) / sizeof(std::uint32_t));
    }
    std::size_t kept = 0;
    for (std::size_t k = 0; k < variant.dataPositions.size(); ++k) {
        if (decoded[variant.dataPositions[k]]) continue;
        variant.dataPositions[kept] = variant.dataPositions[k];
        variant.dataSlots[kept] = variant.dataSlots[k];
        ++kept;
    }
    variant.dataPositions.resize(kept);
    variant.dataSlots.resize(kept);
}

void Driver::cacheDrawStages(bool useDrawEntries, bool drawHit, const Pm4::DrawParameters& drawParameters, const std::optional<Graphics::IndirectDrawPath>& indirectCpu, const std::vector<DrawProgram>& programs, const std::vector<StageCapture>& stageCaptures, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, const std::vector<std::vector<Graphics::DecodeRead>>& decodeReads, bool verifyHit, const std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::shared_ptr<DispatchVariant>>& fresh, std::uint64_t drawKey, bool registerKey, const std::shared_ptr<const DrawDecode>& decode, DrawPhaseTiming& phaseTiming, const std::vector<bool>& reused, const std::shared_ptr<DrawEntry>& entry, std::uint64_t shapeKey) {
    if (useDrawEntries && !drawHit && !(drawParameters.indirect && indirectCpu) && (verifyHit || admitDrawKey(drawKey, registerKey && drawDataHits() ? shapeKey : 0))) {
        phaseTiming.Phase(DrawRowVectors);
        std::uint64_t unstable = 0, mismatches = 0, differingSameRuns = 0, differingWords = 0, differingRunsChanged = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            const auto& stageCapture = stageCaptures[i];
            if (reused[i]) {
                fresh[i] = matched[i];
                continue;
            }
            if (stageCapture.compiled == nullptr || !CacheableResult(*stageCapture.compiled)) continue;
            auto variant = std::make_shared<DispatchVariant>();
            variant->compiled = stageCapture.compiled;
            if (programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh) {
                std::copy_n(programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord, variant->meshIndexDescriptor.size(), variant->meshIndexDescriptor.begin());
            }
            variant->shader = programs[i].snapshot;
            variant->forgetSerial = stageCapture.forgetSerial;
            variant->pushOffset = stageCapture.pushOffset;
            variant->userDataMask = stageCapture.userDataMask;
            variant->walkedValues = stageCapture.userDataUse.asValue;
            variant->walkedBases = stageCapture.userDataUse.asBase;
            variant->valuePositions = stageCapture.valuePositions;
            variant->shareWords = stageCapture.shareWords;
            if (drawShared()) {
                variant->walkKnown = true;
                variant->walkSpans.reserve(stageCapture.regions.size());
                for (const auto& region : stageCapture.regions) variant->walkSpans.emplace_back(region.guestAddress, region.guestAddress + region.bytes.size());
            }
            if (vertexInfos[i]) variant->vertexInfo = std::make_shared<const ShaderRecompiler::ShaderVertexStageInfo>(*vertexInfos[i]);

            std::vector<ShaderRecompiler::MemoryRegion> regions(stageCapture.regions.begin(), stageCapture.regions.end());
            for (const auto& read : decodeReads[i]) regions.push_back({read.address, read.Bytes()});
            std::stable_sort(regions.begin(), regions.end(), [](const ShaderRecompiler::MemoryRegion& a, const ShaderRecompiler::MemoryRegion& b) { return a.guestAddress < b.guestAddress; });
            tagVariantRuns(*variant, regions, stageCapture.userDataUse, !stageCapture.shared && decodeReads[i].empty() && (!vertexInfos[i] || vertexInfos[i]->resourcesNum == 0), programs[i].hiddenUserWords, stageCapture.supplied);
            if (drawStageDataHits() && stageCapture.capture != nullptr && !stageCapture.capture->readTrace.leaves.empty()) recordStageDataPositions(*variant, *stageCapture.capture, decodeReads[i]);
            if (traceDrawCache() && entry != nullptr && matched[i] == nullptr && i < entry->stages.size()) {
                const auto& variants = entry->stages[i];
                const auto front = std::find_if(variants.begin(), variants.end(), [&](const std::shared_ptr<DispatchVariant>& kept) { return kept->pushOffset == variant->pushOffset; });
                if (front != variants.end() && (*front)->runs == variant->runs && (*front)->words.size() == variant->words.size()) {
                    ++differingSameRuns;
                    for (std::size_t w = 0; w < variant->words.size(); ++w) differingWords += (*front)->words[w] != variant->words[w] ? 1u : 0u;
                } else if (front != variants.end()) {
                    ++differingRunsChanged;
                }
            }
            const auto sameContent = [](const DispatchVariant& a, const DispatchVariant& b) {
                const auto flatten = [](const DispatchVariant& source) {
                    std::vector<std::pair<std::uint64_t, std::uint32_t>> flat;
                    std::size_t word = 0;
                    for (const auto& [begin, end] : source.runs) {
                        for (auto address = begin; address < end && word < source.words.size(); address += sizeof(std::uint32_t)) flat.emplace_back(address, source.words[word++]);
                    }
                    std::sort(flat.begin(), flat.end());
                    return flat;
                };
                return (a.runs == b.runs && a.words == b.words) || flatten(a) == flatten(b);
            };
            const auto sameForwardedWords = [](const ShaderRecompiler::RecompileResult& a, const ShaderRecompiler::RecompileResult& b, std::uint32_t hidden) {
                if (a.pushConstants.size() != b.pushConstants.size() || a.shaderDataUserWords != b.shaderDataUserWords) return false;
                for (std::size_t slot = 0; slot < a.shaderDataUserWords.size() && (slot + 1) * sizeof(std::uint32_t) <= a.pushConstants.size(); ++slot) {
                    if (a.shaderDataUserWords[slot] >= hidden && std::memcmp(a.pushConstants.data() + slot * sizeof(std::uint32_t), b.pushConstants.data() + slot * sizeof(std::uint32_t), sizeof(std::uint32_t)) != 0) return false;
                }
                return true;
            };
            if (verifyHit && matched[i] != nullptr && (!sameContent(*matched[i], *variant) || !sameForwardedWords(*matched[i]->compiled, *variant->compiled, programs[i].hiddenUserWords))) {
                ++mismatches;
                static std::atomic<std::uint64_t> reports{0};
                if (reports.fetch_add(1) < 20) std::fprintf(stderr, "[draw-cache] verify: stage %zu (program 0x%llx) of a hit captured differently: %zu runs / %zu words matched, %zu / %zu fresh\n", i, static_cast<unsigned long long>(programs[i].binary.codeAddress), matched[i]->runs.size(), matched[i]->words.size(), variant->runs.size(), variant->words.size());
            }
            if (insertCompare()) {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
                if (!captureStable(stageCapture.regions)) {
                    ++unstable;
                    continue;
                }
            }
            fresh[i] = std::move(variant);
        }
        insertDrawEntry(drawKey, fresh, registerKey ? decode : nullptr, registerKey && drawDataHits() ? shapeKey : 0);
        std::uint64_t dataInserts = 0, dataPositions = 0;
        for (std::size_t i = 0; i < programs.size(); ++i) {
            if (fresh[i] == nullptr || reused[i] || fresh[i]->dataPositions.empty()) continue;
            ++dataInserts;
            dataPositions += fresh[i]->dataPositions.size();
        }
        if (unstable != 0 || mismatches != 0 || differingSameRuns != 0 || differingRunsChanged != 0 || dataInserts != 0) {
            std::lock_guard cacheLock(drawCacheMutex);
            drawEntryCounters.stageDataInserts += dataInserts;
            drawEntryCounters.stageDataPositions += dataPositions;
            drawEntryCounters.unstable += unstable;
            drawEntryCounters.verifyMismatches += mismatches;
            drawEntryCounters.differingSameRuns += differingSameRuns;
            drawEntryCounters.differingWords += differingWords;
            drawEntryCounters.differingRunsChanged += differingRunsChanged;
        }
        phaseTiming.Phase(DrawRowInsert);
    }
}

}
