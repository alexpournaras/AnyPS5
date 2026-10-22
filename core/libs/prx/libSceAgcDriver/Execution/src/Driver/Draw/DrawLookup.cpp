#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Diagnostics.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>

namespace AgcDriver::DriverDetail {

namespace {

struct HitSplit {
    std::atomic<std::uint64_t> draws{0}, regions{0}, validate{0}, loop{0}, bookkeeping{0}, dataOnly{0}, dataRegions{0}, dataValidate{0};
    std::atomic<std::int64_t> lastReport{0};
};

HitSplit& hitSplit() {
    static HitSplit split;
    return split;
}

std::uint64_t SinceNs(std::chrono::steady_clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

void reportHitSplit() {
    auto& split = hitSplit();
    const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    auto last = split.lastReport.load(std::memory_order_relaxed);
    if (last == 0) {
        split.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed);
        return;
    }
    if (now - last < 10'000'000'000ll || !split.lastReport.compare_exchange_strong(last, now, std::memory_order_relaxed)) return;
    const auto draws = split.draws.exchange(0, std::memory_order_relaxed);
    const auto us = [&](std::atomic<std::uint64_t>& slot) { return draws != 0 ? static_cast<double>(slot.exchange(0, std::memory_order_relaxed)) / 1000.0 / static_cast<double>(draws) : 0.0; };
    const auto regions = us(split.regions), validate = us(split.validate), loop = us(split.loop), bookkeeping = us(split.bookkeeping), dataOnly = us(split.dataOnly), dataRegions = us(split.dataRegions), dataValidate = us(split.dataValidate);
    std::fprintf(stderr, "[draw-cache] hit split (10 s, %llu draws, us per draw): entry stages loop %.2f (regions %.2f, validateVariant %.2f), after the loop %.2f; data-only loop %.2f (regions %.2f, validateVariant %.2f)\n", static_cast<unsigned long long>(draws), loop, regions, validate, bookkeeping, dataOnly, dataRegions, dataValidate);
}

void ClassifyDataStageMiss(std::size_t stage, const std::vector<std::uint32_t>& now, const std::vector<std::uint32_t>& was, const std::vector<std::shared_ptr<DispatchVariant>>& variants, std::array<std::uint64_t, 5>& misses, std::vector<std::uint32_t>& words) {
    if (variants.empty() || variants.front() == nullptr) {
        ++misses[4];
        return;
    }
    const auto& front = *variants.front();
    bool used = false;
    for (std::size_t w = 0; w < now.size(); ++w) {
        if (now[w] == was[w]) continue;
        const auto bit = w < 64u ? std::uint64_t{1} << w : std::uint64_t{0};
        if (bit != 0 && (front.userDataMask & bit) == 0) continue;
        std::uint32_t use = 4;
        if (bit != 0 && front.userDataMask == ~std::uint64_t{0}) use = 5;
        else if (bit != 0 && (front.walkedValues & bit) != 0) use = 0;
        else if (bit != 0 && (front.walkedBases & bit) != 0) use = 1;
        else if (bit != 0 && front.compiled != nullptr && (front.compiled->userDataRegisterReads & bit) != 0) use = 2;
        else if (bit != 0) use = 3;
        words.push_back(static_cast<std::uint32_t>(std::min<std::size_t>(stage, 255)) << 16u | use << 8u | static_cast<std::uint32_t>(std::min<std::size_t>(w, 255)));
        used = true;
    }
    ++misses[used ? 2 : 3];
}

}

void Driver::lookupDraw(const Submission& submission, const std::shared_ptr<VulkanDevice>& localDevice, const Graphics::State& graphics, const ShaderRecompiler::ShaderPixelStageInfo& pixel, const std::vector<DrawProgram>& programs, const std::vector<ShaderRecompiler::ProgramRole>& roles, const std::vector<std::optional<ShaderRecompiler::ShaderVertexStageInfo>>& vertexInfos, bool useDrawEntries, bool registerKey, bool profile, std::uint64_t& drawKey, std::shared_ptr<DrawEntry>& entry, std::vector<std::shared_ptr<DispatchVariant>>& matched, std::vector<std::vector<ShaderRecompiler::MemoryRegion>>& matchedRegions, bool& drawHit, bool& verifyHit, DrawPhaseTiming& phaseTiming, std::array<double, DrawDriverPhaseCount>& phaseMs, const std::shared_ptr<DrawEntry>& dataEntry, DrawDataCandidates& dataCandidates, std::vector<StageDataHit>& stageData, std::vector<std::pair<std::shared_ptr<DispatchVariant>, std::vector<ShaderRecompiler::MemoryRegion>>>& sharedCandidates) {
    using Role = ShaderRecompiler::ProgramRole;
    static const bool splits = std::getenv("APS5_PROFILE_SPLITS") != nullptr;
    if (splits) {
        hitSplit().draws.fetch_add(1, std::memory_order_relaxed);
        reportHitSplit();
    }
    if (useDrawEntries) {
        if (!registerKey) {
            drawKey = 0xcbf29ce484222325ull;
            const auto mix = [&](std::uint64_t value) {
                drawKey ^= value;
                drawKey *= 0x100000001b3ull;
            };
            mix(localDevice->Serial());
            mix(static_cast<std::uint64_t>(graphics.stages.path));
            mix(graphics.stages.registerValue);
            mix(graphics.stages.vertexWaveSize);
            mix(graphics.stages.fragmentWaveSize);
            mix(graphics.stages.mesh.has_value());
            if (graphics.stages.mesh) {
                const auto& mesh = *graphics.stages.mesh;
                for (const auto value : {mesh.inputPrimitive, mesh.primitivesPerGroup, mesh.verticesPerGroup, mesh.maxVertices, mesh.maxPrimitives, mesh.threadsPerGroup, mesh.ldsSizeDwords, mesh.provokingVertex, mesh.esgsItemSize, mesh.outputPrimitiveSize}) mix(value);
            }
            mix(graphics.stages.tessellation.has_value());
            if (graphics.stages.tessellation) {
                const auto& tess = *graphics.stages.tessellation;
                for (const auto value : {tess.inputControlPoints, tess.outputControlPoints, tess.domain, tess.partitioning, tess.outputTopology, tess.patchesPerGroup, tess.primitiveGroupSize, static_cast<std::uint32_t>(tess.breakAtInstance)}) mix(value);
                mix(tess.mergedHull);
            }
            mix(graphics.rectList);
            mix(programs.size());
            for (std::size_t i = 0; i < programs.size(); ++i) {
                const auto& program = programs[i];
                mix(reinterpret_cast<std::uintptr_t>(program.snapshot.get()));
                mix(program.codeOffset);
                mix(static_cast<std::uint64_t>(roles[i]));
                mix(static_cast<std::uint64_t>(program.binary.stage));
                mix(program.userDataBase);
                mix(program.firstUserSgpr);
                mix(program.userData.size());
                for (const auto word : program.userData) mix(word);
                mix(vertexInfos[i].has_value());
                if (!vertexInfos[i]) continue;
                const auto& vertex = *vertexInfos[i];
                require(vertex.resourcesNum <= vertex.resources.size(), "vertex stage info resource count exceeds its table");
                mix(vertex.resourcesNum);
                mix(vertex.fetchAttribReg);
                mix(vertex.fetchBufferReg);
                mix(vertex.fetchEmbedded);
                mix(vertex.paClVsOutCntl);
                for (std::uint32_t r = 0; r < vertex.resourcesNum; ++r) {
                    for (const auto field : vertex.resources[r].fields) mix(field);
                    const auto& destination = vertex.resourcesDst[r];
                    mix(static_cast<std::uint32_t>(destination.registerStart));
                    mix(static_cast<std::uint32_t>(destination.registersNum));
                    mix(static_cast<std::uint32_t>(destination.attrId));
                    mix(destination.fetchIndex);
                }
            }
            require(pixel.interpolatorCount <= pixel.interpolatorSettings.size(), "pixel stage info interpolator count exceeds its table");
            mix(pixel.interpolatorCount);
            for (std::uint32_t i = 0; i < pixel.interpolatorCount; ++i) mix(pixel.interpolatorSettings[i]);
            mix(pixel.inputAddr);
            for (const bool flag : {pixel.wave32, pixel.hasPerspectiveCenterVgpr, pixel.perspectiveCentroid, pixel.posX, pixel.posY, pixel.posZ, pixel.posW, pixel.frontFace, pixel.frontFaceAllBits, pixel.ancillary, pixel.sampleShading, pixel.noPerspective, pixel.linearCentroid, pixel.pixelKillEnable, pixel.depthExportEnable, pixel.sampleMaskExportEnable, pixel.earlyZ, pixel.executeOnNoop}) mix(flag);
            mix(static_cast<std::uint64_t>(pixel.conservativeZExport));
            mix(pixel.orderedPixelShader);
            mix(pixel.stencilRefExportEnable);
            mix(pixel.stencilOperationValueExport);
            for (const auto value : pixel.targetOutputMode) mix(value);
            for (const auto value : pixel.targetExportMapping) mix(value);
            for (const auto value : pixel.targetExportPacking) mix(static_cast<std::uint64_t>(value));
            mix(pixel.dualSourceBlend);
            mix(pixel.reversedBlend);
            mix(pixel.alphaToCoverage);
            mix(static_cast<std::uint64_t>(pixel.rasterPrimitive));
            mix(pixel.ditheredAlphaToCoverage);
            mix(pixel.quadPixelMask);
            for (const auto mask : pixel.quadSampleMask) mix(mask);
            mix(pixel.iteratesSamples);
            mix(static_cast<std::uint64_t>(pixel.positionLocation));
            for (const auto location : pixel.sampleLocations) mix(location);
            for (const auto order : pixel.sampleOrder) mix(order);
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.lookups;
            const auto found = drawCache.find(drawKey);
            if (found != drawCache.end()) entry = found->second;
            else ++drawEntryCounters.absent;
            maybeReportDrawCache(profile);
        }
        if (entry != nullptr) {
            const auto waitedBeforeValidate = profile ? Graphics::Recorder::ThreadWaitedMs() : 0.0;
            std::optional<DrawMiss> miss;
            std::vector<std::size_t> ranks(programs.size(), 0);
            std::uint64_t stageValidations = 0, stageEqual = 0, compared = 0, imagesFlushed = 0, runsSynced = 0, dataStages = 0, dataWords = 0;
            std::vector<std::pair<std::uint32_t, std::uint32_t>> liveData;
            auto* live = drawStageDataHits() ? &liveData : nullptr;
            if (entry->stages.size() != programs.size()) miss = DrawMiss::Stages;

            std::uint32_t cursor = 0;
            const auto loopStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::uint64_t regionsNs = 0, validateNs = 0;
            {
                const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);

                std::optional<SampledReadScope> sampling;
                for (std::size_t i = 0; !miss && i < programs.size(); ++i) {
                    if (roles[i] == Role::GeometryBack) continue;
                    ++stageValidations;
                    cursor = Graphics::StagePushOffset(cursor, programs[i].binary.stage, localDevice->GraphicsPipelineLibraries());
                    const auto& variants = entry->stages[i];
                    auto outcome = EntryOutcome::Differing;
                    bool anyLayout = false;
                    for (std::size_t rank = 0; rank < variants.size(); ++rank) {
                        const auto& variant = variants[rank];
                        if (variant->pushOffset != cursor) continue;
                        if (programs[i].binary.stage == ShaderRecompiler::ShaderStage::Mesh && !std::equal(variant->meshIndexDescriptor.begin(), variant->meshIndexDescriptor.end(), programs[i].userData.begin() + ShaderRecompiler::MeshIndexBufferUserWord)) {
                            if (!anyLayout) outcome = EntryOutcome::Differing;
                            anyLayout = true;
                            continue;
                        }
                        auto& regions = matchedRegions[i];
                        const auto regionsStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        regions.clear();
                        appendEntryRegions(*variant, regions);
                        ++compared;
                        if (splits) regionsNs += SinceNs(regionsStarted);
                        const auto validateStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling, live);
                        if (splits) validateNs += SinceNs(validateStarted);
                        if (!anyLayout) outcome = result;
                        anyLayout = true;
                        if (result != EntryOutcome::Equal && result != EntryOutcome::EqualData) continue;
                        if (result == EntryOutcome::EqualData) {
                            auto& hit = stageData[i];
                            hit.words = variant->words;
                            for (const auto& [position, value] : liveData) hit.words[position] = value;
                            regions.clear();
                            appendEntryRegions(*variant, regions, &hit.words);
                            hit.compiled = patchDataWords(*variant, hit.words);
                            ++dataStages;
                            dataWords += liveData.size();
                        }
                        matched[i] = variant;
                        ranks[i] = rank;
                        ++stageEqual;
                        cursor += static_cast<std::uint32_t>(variant->compiled->pushConstants.size());
                        break;
                    }
                    if (matched[i] != nullptr) continue;
                    if (!anyLayout) miss = DrawMiss::Layout;
                    else if (outcome != EntryOutcome::Differing) miss = DrawMiss::Gate;
                    else miss = roles[i] == Role::Fragment ? DrawMiss::FragmentDiffering : i == 0 ? DrawMiss::FrontDiffering : DrawMiss::OtherDiffering;
                }
            }
            drawHit = !miss;
            const auto bookkeepingStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            if (splits) {
                auto& split = hitSplit();
                split.loop.fetch_add(SinceNs(loopStarted), std::memory_order_relaxed);
                split.regions.fetch_add(regionsNs, std::memory_order_relaxed);
                split.validate.fetch_add(validateNs, std::memory_order_relaxed);
            }
            struct BookkeepingTimer {
                bool on;
                std::chrono::steady_clock::time_point start;
                ~BookkeepingTimer() {
                    if (on) hitSplit().bookkeeping.fetch_add(SinceNs(start), std::memory_order_relaxed);
                }
            } bookkeepingTimer{splits, bookkeepingStarted};
            std::lock_guard cacheLock(drawCacheMutex);
            auto& counters = drawEntryCounters;
            counters.stageValidations += stageValidations;
            counters.stageEqual += stageEqual;
            counters.variantsCompared += compared;
            if (drawHit) {
                ++counters.hits;
                counters.stageDataHits += dataStages;
                counters.stageDataWords += dataWords;
                if (registerKey) ++counters.registerKeyHits;
                ++drawCacheHits;
                bool rotate = false;
                for (std::size_t i = 0; i < programs.size(); ++i) {
                    if (matched[i] == nullptr) continue;
                    ++counters.variantHitsByRank[ranks[i]];
                    if (ranks[i] != 0) rotate = true;
                }

                const auto again = drawCache.find(drawKey);
                if (again != drawCache.end() && again->second == entry) {
                    if (rotate) {
                        auto rotated = std::make_shared<DrawEntry>();
                        rotated->decode = entry->decode;
                        rotated->stages = entry->stages;
                        rotated->shape = entry->shape;
                        rotated->recipes.store(entry->recipes.load());
                        for (std::size_t i = 0; i < programs.size(); ++i) {
                            if (ranks[i] == 0) continue;
                            auto& variants = rotated->stages[i];
                            variants.erase(variants.begin() + static_cast<std::ptrdiff_t>(ranks[i]));
                            variants.insert(variants.begin(), matched[i]);
                        }
                        rotated->touched = entry->touched;
                        rotated->order = entry->order;
                        again->second = std::move(rotated);
                    }
                    if (drawCacheHits - again->second->touched > drawCacheEntries() / 8) {
                        drawOrder.splice(drawOrder.begin(), drawOrder, again->second->order);
                        again->second->touched = drawCacheHits;
                        ++counters.touches;
                    }
                }
                if (verifyDrawEntries()) {
                    ++counters.verifyHits;
                    verifyHit = true;
                    drawHit = false;
                }
                if (dataStages != 0 && verifyDrawDataHits()) drawHit = false;
            } else {
                ++counters.misses[static_cast<std::size_t>(*miss)];
                if (registerKey && entry->decode != nullptr) ++counters.decodePartial;
            }
            if (profile) {

                phaseTiming.Phase(DrawRowKeyLookupValidate);
                const auto waited = std::min(Graphics::Recorder::ThreadWaitedMs() - waitedBeforeValidate, phaseMs[DrawRowKeyLookupValidate]);
                phaseMs[DrawRowKeyLookupValidate] -= waited;
                phaseMs[DrawRowValidateWait] += waited;
                counters.validateUs += (phaseMs[DrawRowKeyLookup] + phaseMs[DrawRowKeyLookupValidate]) * 1000;
            }
        } else if (dataEntry != nullptr && graphics.stages.path == Graphics::ShaderPath::Vertex && dataEntry->stages.size() == programs.size() && dataEntry->decode->programs.size() == programs.size()) {
            std::uint64_t imagesFlushed = 0, runsSynced = 0;
            const GuestMemory::ReadSiteScope site(GuestMemory::ReadSite::DrawCache);
            std::optional<SampledReadScope> sampling;
            std::array<std::uint64_t, 5> stageMisses{};
            std::vector<std::uint32_t> changedWords;
            const auto dataStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            std::uint64_t dataRegionsNs = 0, dataValidateNs = 0;
            for (std::size_t i = 0; i < programs.size(); ++i) {
                if (roles[i] == Role::GeometryBack) continue;
                if (programs[i].userData.size() != dataEntry->decode->programs[i].userData.size()) {
                    ++stageMisses[1];
                    continue;
                }
                const auto& now = programs[i].userData;
                const auto& was = dataEntry->decode->programs[i].userData;
                auto& candidates = dataCandidates[i];
                for (const auto& variant : dataEntry->stages[i]) {
                    bool usedEqual = true;
                    for (std::size_t w = 0; w < now.size() && usedEqual; ++w) {
                        if (now[w] != was[w] && (w >= 64u || ((variant->userDataMask >> w) & 1u) != 0)) usedEqual = false;
                    }
                    if (!usedEqual) continue;
                    if ((i == 0 && variant->pushOffset != 0) || std::any_of(candidates.begin(), candidates.end(), [&](const auto& candidate) { return candidate.first->pushOffset == variant->pushOffset; })) continue;
                    const auto regionsStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    std::vector<ShaderRecompiler::MemoryRegion> regions;
                    appendEntryRegions(*variant, regions);
                    if (splits) dataRegionsNs += SinceNs(regionsStarted);
                    const auto validateStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                    auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling);
                    if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling);
                    if (splits) dataValidateNs += SinceNs(validateStarted);
                    if (result == EntryOutcome::Equal) candidates.emplace_back(variant, std::move(regions));
                }
                if (candidates.empty() && drawShared() && i < sharedCandidates.size()) {
                    for (const auto& variant : dataEntry->stages[i]) {
                        if (variant == nullptr || variant->compiled == nullptr) continue;
                        const auto walked = variant->walkedValues | variant->walkedBases;
                        std::uint64_t fetch = 0;
                        if (variant->vertexInfo != nullptr) {
                            for (const auto reg : {variant->vertexInfo->fetchAttribReg, variant->vertexInfo->fetchBufferReg}) fetch |= reg + 1u < 64u ? std::uint64_t{3} << reg : ~std::uint64_t{0};
                        }
                        std::uint64_t code = 0;
                        if (const auto tail = programs[i].snapshot->userDataTailRegister; tail && *tail >= programs[i].firstUserSgpr) {
                            const auto word = *tail - programs[i].firstUserSgpr;
                            code = word + 1u < 64u ? std::uint64_t{3} << word : ~std::uint64_t{0};
                        }
                        const auto allowed = (variant->shareWords | (variant->compiled->userDataRegisterReads & ~walked) | (fetch & ~walked)) & ~code;
                        bool shareable = true;
                        for (std::size_t w = 0; w < now.size() && shareable; ++w) {
                            if (now[w] == was[w]) continue;
                            if (w >= 64u) shareable = false;
                            else if ((variant->userDataMask >> w & 1u) != 0 && (allowed >> w & 1u) == 0) shareable = false;
                        }
                        if (!shareable) continue;
                        const auto regionsStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        std::vector<ShaderRecompiler::MemoryRegion> regions;
                        appendEntryRegions(*variant, regions);
                        if (splits) dataRegionsNs += SinceNs(regionsStarted);
                        const auto validateStarted = splits ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                        auto result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling);
                        if (gateRetry() && (result == EntryOutcome::PublishMoved || result == EntryOutcome::PendingMoved)) result = validateVariant(programs[i].binary.codeAddress, submission.queue, *variant, regions, imagesFlushed, runsSynced, sampling);
                        if (splits) dataValidateNs += SinceNs(validateStarted);
                        if (result != EntryOutcome::Equal) continue;
                        sharedCandidates[i] = {variant, std::move(regions)};
                        break;
                    }
                }
                if (traceDrawCache() && candidates.empty()) ClassifyDataStageMiss(i, now, was, dataEntry->stages[i], stageMisses, changedWords);
            }
            if (splits) {
                auto& split = hitSplit();
                split.dataOnly.fetch_add(SinceNs(dataStarted), std::memory_order_relaxed);
                split.dataRegions.fetch_add(dataRegionsNs, std::memory_order_relaxed);
                split.dataValidate.fetch_add(dataValidateNs, std::memory_order_relaxed);
            }
            if (traceDrawCache()) {
                std::lock_guard cacheLock(drawCacheMutex);
                auto& counters = drawEntryCounters;
                for (std::size_t reason = 0; reason < stageMisses.size(); ++reason) counters.dataStageMisses[reason] += stageMisses[reason];
                for (const auto word : changedWords) ++counters.dataWordChanges[word];
            }
            phaseTiming.Phase(DrawRowDataValidate);
        } else if (dataEntry != nullptr && traceDrawCache()) {
            std::lock_guard cacheLock(drawCacheMutex);
            ++drawEntryCounters.dataStageMisses[0];
        }
        phaseTiming.Phase(DrawRowKeyLookupValidate);
    }
}

}
