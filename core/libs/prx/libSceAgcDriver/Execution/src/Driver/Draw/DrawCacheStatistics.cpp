#include "prx/libSceAgcDriver/Execution/include/ProfileOutput.hpp"
#include "prx/libSceAgcDriver/Execution/include/Driver/Driver.hpp"
#include <algorithm>
#include <cstdlib>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace AgcDriver::DriverDetail {

bool Driver::traceDrawCache() {
    static const bool trace = std::getenv("APS5_TRACE_DRAW_CACHE") != nullptr;
    return trace;
}

void Driver::traceInsert(std::unordered_set<std::uint64_t>& set, std::uint64_t key) {
    if (set.size() >= (1u << 16u)) set.clear();
    set.insert(key);
}

void Driver::maybeReportDrawCache(bool profile) {
    auto& counters = drawEntryCounters;
    if (!profile && !traceDrawCache()) return;
    const auto now = std::chrono::steady_clock::now();
    if (now - counters.lastReport < std::chrono::seconds(10)) return;
    reportDrawCache(counters, profile);
    counters.lastReport = now;
}

void Driver::reportDrawCache(DrawEntryCounters& counters, bool profile) {
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string text;
    if (traceDrawCache()) {
        char line[1536];
        std::uint64_t misses = 0;
        for (const auto value : counters.misses) misses += value;
        std::snprintf(line, sizeof(line), "[draw-cache] 10 s: %llu lookups, %llu hits (%.1f%%), %llu stage misses, %llu without an entry, %llu data-only hits (stages: %llu reused, %llu compiled; %llu verified); no entry: new registers %llu, user words/pointers only %llu, evicted %llu, seen but never inserted %llu; stage misses: front differing %llu, fragment differing %llu, other differing %llu, layout %llu, gate %llu, stage count %llu; differing captures: same runs %llu (%.1f words changed each), other runs %llu; %zu entries, %llu evicted\n", count(counters.lookups), count(counters.hits), counters.lookups != 0 ? 100.0 * static_cast<double>(counters.hits) / static_cast<double>(counters.lookups) : 0.0, count(misses), count(counters.absent), count(counters.dataHits), count(counters.dataStagesReused), count(counters.dataStagesCompiled), count(counters.dataVerified), count(counters.absentNewRegisters), count(counters.absentUserWords), count(counters.absentEvicted), count(counters.absentNeverInserted), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.differingSameRuns), counters.differingSameRuns != 0 ? static_cast<double>(counters.differingWords) / static_cast<double>(counters.differingSameRuns) : 0.0, count(counters.differingRunsChanged), drawCache.size(), count(counters.evictions));
        text += line;
        std::snprintf(line, sizeof(line), "[draw-cache] register-key miss component changes (10 s): stable registers %llu, user data %llu, auxiliary registers %llu, device %llu, first shader set %llu, unchanged components %llu\n", count(counters.registerKeyMissChanges[0]), count(counters.registerKeyMissChanges[1]), count(counters.registerKeyMissChanges[2]), count(counters.registerKeyMissChanges[3]), count(counters.registerKeyMissChanges[4]), count(counters.registerKeyMissChanges[5]));
        text += line;
        std::vector<std::pair<std::uint64_t, std::uint32_t>> ranked;
        ranked.reserve(counters.dataWordChanges.size());
        for (const auto& [word, changes] : counters.dataWordChanges) ranked.emplace_back(changes, word);
        std::sort(ranked.begin(), ranked.end(), std::greater<>());
        static constexpr const char* uses[] = {"value", "pointer", "register", "fetch", "beyond 64", "unwalked"};
        std::string words;
        for (std::size_t k = 0; k < ranked.size() && k < 16; ++k) {
            char item[64];
            const auto word = ranked[k].second;
            std::snprintf(item, sizeof(item), "%sstage %u word %u %s %llu", k == 0 ? "" : ", ", word >> 16u, word & 0xffu, uses[std::min<std::uint32_t>(word >> 8u & 0xffu, 5)], count(ranked[k].first));
            words += item;
        }
        std::snprintf(line, sizeof(line), "[draw-cache] data-only stage misses (10 s): draws off the vertex path %llu, user word counts differ %llu, a used word differs %llu, used words equal but memory or push offset differs %llu, no variant %llu; changed used words (%zu distinct): %s\n", count(counters.dataStageMisses[0]), count(counters.dataStageMisses[1]), count(counters.dataStageMisses[2]), count(counters.dataStageMisses[3]), count(counters.dataStageMisses[4]), ranked.size(), words.c_str());
        text += line;
    }
    if (drawShared()) {
        char line[512];
        std::snprintf(line, sizeof(line), "[draw-cache] shared stages (10 s): %llu built from a kept stage without a walk or a build; refused: push offset %llu, vertex formats %llu, descriptor shape %llu; %llu verified against a full build\n", count(counters.sharedStages), count(counters.sharedPushOffset), count(counters.sharedVertexShape), count(counters.sharedDescriptorShape), count(counters.sharedVerified));
        text += line;
    }
    if (profile) text += profileDrawCacheLines(counters) + validateSplitReport();
    AgcDriver::ProfilePrint_nid_no_patch("%s", text.c_str());
    counters = DrawEntryCounters{};
}

DrawEntryCounters Driver::DrawCacheCounters() {
    std::lock_guard cacheLock(drawCacheMutex);
    return drawEntryCounters;
}

std::string Driver::profileDrawCacheLines(const DrawEntryCounters& counters) {
    std::string text;
    const auto append = [&](const char* format, auto... values) {
        char line[2048];
        std::snprintf(line, sizeof(line), format, values...);
        text += line;
    };
    const auto count = [](std::uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto validated = counters.lookups > counters.absent ? counters.lookups - counters.absent : 0;
    const auto miss = [&](DrawMiss reason) { return count(counters.misses[static_cast<std::size_t>(reason)]); };
    std::string ranks;
    for (std::size_t rank = 0; rank < dispatchVariants(); ++rank) {
        char item[32];
        std::snprintf(item, sizeof(item), "%s%llu", rank == 0 ? "" : " / ", count(counters.variantHitsByRank[rank]));
        ranks += item;
    }
    append("[draw-cache] %llu lookups (10 s): %llu no entry, %llu validated in %.1f us each (key, lookup and every stage): %llu hits (every stage equal; stage hits by variant rank 1..k %s), misses by reason: front stage differing %llu, fragment differing %llu, other stage differing %llu, layout (push offset) %llu, gate %llu, stage count %llu; %llu stage validations (%llu equal, %.2f variants compared each); %llu inserts (%llu variants inserted, %llu evicted beyond k, %llu already present, %llu unstable), %zu entries (%llu variants, ~%.1f MiB), %llu evictions in total, %llu LRU moves; verify: %llu hits captured again, %llu stages differed\n", count(counters.lookups), count(counters.absent), count(validated), validated != 0 ? counters.validateUs / static_cast<double>(validated) : 0.0, count(counters.hits), ranks.c_str(), miss(DrawMiss::FrontDiffering), miss(DrawMiss::FragmentDiffering), miss(DrawMiss::OtherDiffering), miss(DrawMiss::Layout), miss(DrawMiss::Gate), miss(DrawMiss::Stages), count(counters.stageValidations), count(counters.stageEqual), counters.stageValidations != 0 ? static_cast<double>(counters.variantsCompared) / static_cast<double>(counters.stageValidations) : 0.0, count(counters.inserts), count(counters.variantsInserted), count(counters.variantsEvicted), count(counters.present), count(counters.unstable), drawCache.size(), count(drawCacheVariants), static_cast<double>(drawCacheVariantBytes) / (1024.0 * 1024.0), count(drawCacheEvictions), count(counters.touches), count(counters.verifyHits), count(counters.verifyMismatches));
    append("[draw-cache] register key (10 s): %llu lookups, %llu hits, key build %.1f us each; decode skipped %llu, partial (state/pixel/programs from the entry) %llu; facade log != table %llu; verify: %llu decodes compared, %llu differed\n", count(counters.registerKeyLookups), count(counters.registerKeyHits), counters.registerKeyLookups != 0 ? counters.keyUs / static_cast<double>(counters.registerKeyLookups) : 0.0, count(counters.decodeSkipped), count(counters.decodePartial), count(counters.facadeMismatches), count(counters.verifyDecodes), count(counters.verifyDecodeMismatches));
    append("[draw-cache] stage data hits (10 s): %llu stages (%llu words refreshed), verified %llu; stage inserts with data positions %llu (%.1f positions each)\n", count(counters.stageDataHits), count(counters.stageDataWords), count(counters.stageDataVerified), count(counters.stageDataInserts), counters.stageDataInserts != 0 ? static_cast<double>(counters.stageDataPositions) / static_cast<double>(counters.stageDataInserts) : 0.0);
    return text;
}

}
