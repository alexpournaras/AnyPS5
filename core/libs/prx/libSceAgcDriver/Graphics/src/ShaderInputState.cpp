#include "prx/libSceAgcDriver/Graphics/include/ShaderInputState.hpp"
#include "SceShaders.hpp"
#include "prx/libSceAgc/Shader/include/ShaderConstants.hpp"
#include "prx/libSceAgcDriver/Graphics/include/State.hpp"
#include "prx/libSceAgcDriver/Execution/include/GuestMemory.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>

namespace AgcDriver::Graphics {
namespace {

constexpr std::uint32_t computeNumThreadX = 0x207;
constexpr std::uint32_t computeNumThreadY = 0x208;
constexpr std::uint32_t computeNumThreadZ = 0x209;
constexpr std::uint32_t computePgmRsrc2 = 0x213;

constexpr std::uint32_t spiPsInputCntl0 = 0x191;
constexpr std::uint32_t spiPsInputEna = 0x1B3;
constexpr std::uint32_t spiPsInputAddr = 0x1B4;
constexpr std::uint32_t spiPsInControl = 0x1B6;
constexpr std::uint32_t spiBarycCntl = 0x1B8;
constexpr std::uint32_t dbShaderControl = 0x203;
constexpr std::uint32_t spiShaderColFormat = 0x1C5;
constexpr std::uint32_t defaultPixelInputs = 0x2u;

std::uint32_t read(const Registers& registers, std::uint32_t offset, RegisterBank bank) {
    NoteRegisterRead(bank, offset);
    const auto it = registers.find(offset);
    if (it == registers.end()) {
        char text[64];
        std::snprintf(text, sizeof(text), "AGC graphics: missing register at DWORD 0x%x", offset);
        throw std::runtime_error(text);
    }
    return it->second;
}

template <typename T> T _readHeaderPod(std::span<const std::byte> header, std::uint64_t headerAddress, const void* pointer, std::size_t bytes = sizeof(T)) {
    if (pointer == nullptr) throw std::runtime_error("AGC graphics: null AGC header pointer");
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    if (address < headerAddress) throw std::runtime_error("AGC graphics: AGC header pointer precedes the shader header");
    const auto offset = address - headerAddress;
    if (offset > header.size() || bytes > header.size() - offset) throw std::runtime_error("AGC graphics: AGC header pointer is outside the registered shader header");
    T value{};
    std::memcpy(&value, header.data() + offset, bytes);
    return value;
}

template <typename T> void _readHeaderArray(std::span<const std::byte> header, std::uint64_t headerAddress, const void* pointer, std::uint32_t count, T* destination) {
    if (count == 0) return;
    if (pointer == nullptr) throw std::runtime_error("AGC graphics: null AGC header array pointer");
    const auto address = reinterpret_cast<std::uint64_t>(pointer);
    if (address < headerAddress) throw std::runtime_error("AGC graphics: AGC header array pointer precedes the shader header");
    const auto offset = address - headerAddress;
    const auto bytes = static_cast<std::uint64_t>(count) * sizeof(T);
    if (offset + bytes > header.size()) throw std::runtime_error("AGC graphics: AGC header array is outside the registered shader header");
    std::memcpy(destination, header.data() + offset, bytes);
}

}

ShaderRecompiler::ShaderComputeStageInfo DecodeComputeStageInfo(const Registers& shader, std::span<const std::byte> header) {
    const auto numThreadX = read(shader, computeNumThreadX, RegisterBank::Shader) & 0xffffu;
    const auto numThreadY = read(shader, computeNumThreadY, RegisterBank::Shader) & 0xffffu;
    const auto numThreadZ = read(shader, computeNumThreadZ, RegisterBank::Shader) & 0xffffu;
    if (numThreadX == 0 || numThreadY == 0 || numThreadZ == 0) {
        throw std::runtime_error("AGC graphics: COMPUTE_NUM_THREAD_X/Y/Z must be nonzero");
    }
    const auto rsrc2 = read(shader, computePgmRsrc2, RegisterBank::Shader);
    std::uint32_t scratchDwords = 0;
    if ((rsrc2 & 0x1u) != 0) {
        if (header.size() < sizeof(Shader)) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN without an AGC shader header");
        Shader agcShader;
        std::memcpy(&agcShader, header.data(), sizeof(Shader));
        scratchDwords = agcShader.scratch_size_dw_per_thread;
        if (scratchDwords == 0) throw std::runtime_error("AGC graphics: COMPUTE_PGM_RSRC2.SCRATCH_EN with a zero scratch size");
    }
    // Debug aid: APS5_LDS_SLACK=<dwords> grows every dispatch's LDS allocation by that much, to tell
    // whether a program depends on addresses past its declared allocation.
    static const std::uint32_t ldsSlack = [] { const char* text = std::getenv("APS5_LDS_SLACK"); return text ? static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0)) : 0u; }();
    return ShaderRecompiler::ShaderComputeStageInfo{
        {numThreadX, numThreadY, numThreadZ},
        std::min(((rsrc2 >> 15u) & 0x1FFu) * 128u + ldsSlack, 16384u),
        {((rsrc2 >> 7u) & 0x1u) != 0, ((rsrc2 >> 8u) & 0x1u) != 0, ((rsrc2 >> 9u) & 0x1u) != 0},
        ((rsrc2 >> 10u) & 0x1u) != 0,
        ((rsrc2 >> 11u) & 0x3u) + 1u,
        {},
        scratchDwords
    };
}

ShaderRecompiler::ShaderPixelStageInfo DefaultUnexportedInputs(ShaderRecompiler::ShaderPixelStageInfo pixel, std::span<const std::uint32_t> parameterExports) {
    for (std::uint32_t input = 0; input < pixel.interpolatorCount && input < pixel.interpolatorSettings.size(); ++input) {
        auto& setting = pixel.interpolatorSettings[input];
        if ((setting & 0x20u) != 0u || std::ranges::find(parameterExports, setting & 0x1fu) != parameterExports.end()) continue;
        setting = (setting & ~0x700u) | 0x20u;
        if ((setting & 0x80000u) != 0u) setting = (setting & ~0x600000u) | 0x100000u;
    }
    return pixel;
}

ShaderRecompiler::ShaderPixelStageInfo DecodePixelStageInfo(const Registers& context, const std::array<std::uint8_t, 8>& exportMappings, bool nullProgram) {
    const auto inControl = nullProgram ? 0u : read(context, spiPsInControl, RegisterBank::Context);
    const auto inputNum = inControl & 0x3Fu;
    if (inputNum > 32u) {
        throw std::runtime_error("AGC graphics: SPI_PS_IN_CONTROL input count exceeds 32");
    }
    const auto ena = nullProgram ? defaultPixelInputs : read(context, spiPsInputEna, RegisterBank::Context);
    const auto addr = nullProgram ? defaultPixelInputs : read(context, spiPsInputAddr, RegisterBank::Context);
    const auto activeInputs = ena & addr;
    using ShaderRecompiler::PixelInput;
    using ShaderRecompiler::PixelInputBit;
    constexpr std::uint32_t knownMask = PixelInputBit(PixelInput::PerspectiveSample) | PixelInputBit(PixelInput::PerspectiveCenter) | PixelInputBit(PixelInput::PerspectiveCentroid) |
        PixelInputBit(PixelInput::LinearSample) | PixelInputBit(PixelInput::LinearCenter) | PixelInputBit(PixelInput::LinearCentroid) |
        PixelInputBit(PixelInput::PositionX) | PixelInputBit(PixelInput::PositionY) | PixelInputBit(PixelInput::PositionZ) | PixelInputBit(PixelInput::PositionW) |
        PixelInputBit(PixelInput::FrontFace) | PixelInputBit(PixelInput::Ancillary) | PixelInputBit(PixelInput::SampleCoverage) | PixelInputBit(PixelInput::LineStipple) | PixelInputBit(PixelInput::PositionFixedPoint);
    if ((activeInputs & ~knownMask) != 0) {
        char message[128];
        std::snprintf(message, sizeof(message), "AGC graphics: unsupported SPI_PS_INPUT_ENA/ADDR bit combination (ena 0x%x addr 0x%x)", ena, addr);
        throw std::runtime_error(message);
    }
    std::array<std::uint32_t, 32> interpolatorSettings{};
    for (std::uint32_t i = 0; i < inputNum; ++i) {
        interpolatorSettings[i] = read(context, spiPsInputCntl0 + i, RegisterBank::Context);
    }
    const auto shaderControl = nullProgram ? 0u : read(context, dbShaderControl, RegisterBank::Context);
    // Bits 9 and 11 are EXEC_ON_HIER_FAIL and ALPHA_TO_MASK_DISABLE on GFX10; neither changes what the
    // recompiled pixel shader computes.
    const auto conservativeZExport = (shaderControl >> 13u) & 0x3u;
    if (conservativeZExport == 3u) {
        throw std::runtime_error("AGC graphics: DB_SHADER_CONTROL.CONSERVATIVE_Z_EXPORT uses the reserved value 3");
    }
    const auto colFormat = nullProgram ? 0u : read(context, spiShaderColFormat, RegisterBank::Context);
    std::array<std::uint8_t, 8> targetOutputMode{};
    for (std::uint32_t i = 0; i < 8u; ++i) {
        targetOutputMode[i] = static_cast<std::uint8_t>((colFormat >> (4u * i)) & 0xFu);
    }
    const bool pixelKillEnable = ((shaderControl >> 6u) & 0x1u) != 0;
    const bool depthExportEnable = (shaderControl & 0x1u) != 0;
    const bool sampleMaskExportEnable = ((shaderControl >> 8u) & 0x1u) != 0;
    const bool stencilOperationValue = !nullProgram && StencilOperationValueReplaces(context);
    const bool stencilRefExportEnable = ((shaderControl >> 1u) & 0x1u) != 0 || stencilOperationValue;
    if (stencilOperationValue && PixelEarlyZ(shaderControl)) throw std::runtime_error("AGC graphics: early tests with a pixel shader that exports the stencil operation value are unsupported");
    const auto loaded = [&](PixelInput input) { return (activeInputs & PixelInputBit(input)) != 0; };
    std::uint32_t positionControl = 0;
    if (loaded(PixelInput::PositionX) || loaded(PixelInput::PositionY)) {
        NoteRegisterRead(RegisterBank::Context, spiBarycCntl);
        if (const auto it = context.find(spiBarycCntl); it != context.end()) positionControl = it->second;
    }
    const auto floatLocation = (positionControl >> 16u) & 0x3u;
    if (floatLocation == 3u) {
        throw std::runtime_error("AGC graphics: SPI_BARYC_CNTL.POS_FLOAT_LOCATION uses the reserved value 3");
    }
    return ShaderRecompiler::ShaderPixelStageInfo{
        .interpolatorCount = inputNum,
        .interpolatorSettings = interpolatorSettings,
        .wave32 = (inControl & 0x8000u) != 0,
        .inputAddr = addr,
        .hasPerspectiveCenterVgpr = loaded(PixelInput::PerspectiveCenter),
        .perspectiveCentroid = loaded(PixelInput::PerspectiveCentroid),
        .posX = loaded(PixelInput::PositionX),
        .posY = loaded(PixelInput::PositionY),
        .posZ = loaded(PixelInput::PositionZ),
        .posW = loaded(PixelInput::PositionW),
        .frontFace = loaded(PixelInput::FrontFace),
        .frontFaceAllBits = loaded(PixelInput::FrontFace) && ((read(context, spiBarycCntl, RegisterBank::Context) >> 24u) & 0x1u) != 0,
        .ancillary = loaded(PixelInput::Ancillary),
        .sampleShading = loaded(PixelInput::PerspectiveSample) && loaded(PixelInput::LinearSample),
        .noPerspective = loaded(PixelInput::LinearCenter),
        .linearCentroid = loaded(PixelInput::LinearCentroid),
        .pixelKillEnable = pixelKillEnable,
        .depthExportEnable = depthExportEnable,
        .sampleMaskExportEnable = sampleMaskExportEnable,
        .earlyZ = PixelEarlyZ(shaderControl),
        .executeOnNoop = ((shaderControl >> 10u) & 0x1u) != 0,
        .conservativeZExport = static_cast<ShaderRecompiler::ConservativeZExport>(conservativeZExport),
        .orderedPixelShader = ((shaderControl >> 16u) & 0x1u) != 0,
        .targetOutputMode = targetOutputMode,
        .targetExportMapping = nullProgram ? std::array<std::uint8_t, 8>{} : exportMappings,
        .stencilRefExportEnable = stencilRefExportEnable,
        .stencilOperationValueExport = stencilOperationValue,
        .positionLocation = (positionControl & 0x100000u) != 0u ? ShaderRecompiler::PixelPositionLocation::UpperLeft : static_cast<ShaderRecompiler::PixelPositionLocation>(floatLocation)
    };
}

bool PixelEarlyZ(std::uint32_t shaderControl) {
    const bool pixelKillEnable = ((shaderControl >> 6u) & 0x1u) != 0;
    const bool depthExportEnable = (shaderControl & 0x1u) != 0;
    const bool sampleMaskExportEnable = ((shaderControl >> 8u) & 0x1u) != 0;
    const bool stencilRefExportEnable = ((shaderControl >> 1u) & 0x1u) != 0;
    const bool depthBeforeShader = ((shaderControl >> 12u) & 0x1u) != 0;
    const auto zOrder = (shaderControl >> 4u) & 0x3u;
    return depthBeforeShader || ((zOrder & 1u) != 0u && !pixelKillEnable && !depthExportEnable && !sampleMaskExportEnable && !stencilRefExportEnable);
}

static ShaderRecompiler::ShaderVertexStageInfo decodeVertexStageInfoWith(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::vector<DecodeRead>* reads, bool staticAbi, std::uint32_t hiddenUserWords, bool batched) {
    if (header.empty()) return {};
    if (header.size() < sizeof(Shader)) throw std::runtime_error("AGC graphics: shader header is smaller than the fixed AGC header");
    Shader shader;
    std::memcpy(&shader, header.data(), sizeof(Shader));
    ShaderRecompiler::ShaderVertexStageInfo info{};
    if (shader.user_data == nullptr) throw std::runtime_error("AGC graphics: missing AGC user-data header");
    constexpr auto userDataBytes = offsetof(ShaderUserData, sharp_resource_count) + sizeof(ShaderUserData{}.sharp_resource_count);
    const auto userDataHeader = _readHeaderPod<ShaderUserData>(header, headerAddress, shader.user_data, userDataBytes);
    if (userDataHeader.direct_resource_count > ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT) throw std::runtime_error("AGC graphics: AGC direct-resource count exceeds the known resource domain");
    std::array<std::uint16_t, ShaderRegs::AGC_DIRECT_RESOURCE_TYPE_COUNT> directOffsets{};
    directOffsets.fill(ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET);
    if (userDataHeader.direct_resource_count != 0) _readHeaderArray(header, headerAddress, userDataHeader.direct_resource_offset, userDataHeader.direct_resource_count, directOffsets.data());
    std::int32_t vertexBufferReg = -1;
    std::int32_t vertexAttribReg = -1;
    for (std::uint32_t type = 0; type < userDataHeader.direct_resource_count; ++type) {
        const auto reg = directOffsets[type];
        if (reg == ShaderRegs::AGC_ILLEGAL_DIRECT_OFFSET) continue;
        if (type == static_cast<std::uint32_t>(ShaderRegs::AgcDirectResourceType::PtrVertexBufferTable)) vertexBufferReg = reg;
        if (type == static_cast<std::uint32_t>(ShaderRegs::AgcDirectResourceType::PtrVertexAttribDescTable)) vertexAttribReg = reg;
    }
    if (vertexAttribReg < 0) return info;
    if (vertexBufferReg < 0) throw std::runtime_error("AGC graphics: vertex attribute table requires a vertex buffer table");
    const auto bufferWord = static_cast<std::uint32_t>(vertexBufferReg) + hiddenUserWords;
    const auto attribWord = static_cast<std::uint32_t>(vertexAttribReg) + hiddenUserWords;
    if (bufferWord + 1u >= userData.size()) throw std::runtime_error("AGC graphics: vertex buffer table pointer exceeds the user-SGPR domain");
    if (attribWord + 1u >= userData.size()) throw std::runtime_error("AGC graphics: vertex attribute table pointer exceeds the user-SGPR domain");
    if (shader.num_input_semantics == 0 || shader.num_input_semantics > ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex semantic count is outside the supported domain");
    if (shader.input_semantics == nullptr) throw std::runtime_error("AGC graphics: missing vertex input semantics");
    std::array<ShaderSemantic, ShaderRecompiler::ShaderVertexStageInfo::MaxResources> semantics{};
    _readHeaderArray(header, headerAddress, shader.input_semantics, shader.num_input_semantics, semantics.data());
    const auto attribTableAddr = static_cast<std::uint64_t>(userData[attribWord]) | (static_cast<std::uint64_t>(userData[attribWord + 1u]) << 32u);
    const auto bufferTableAddr = static_cast<std::uint64_t>(userData[bufferWord]) | (static_cast<std::uint64_t>(userData[bufferWord + 1u]) << 32u);
    if (!staticAbi && attribTableAddr == 0) throw std::runtime_error("AGC graphics: null vertex attribute table address");
    if (!staticAbi && bufferTableAddr == 0) throw std::runtime_error("AGC graphics: null vertex buffer table address");
    info.fetchEmbedded = true;
    info.fetchAttribReg = static_cast<std::uint32_t>(vertexAttribReg);
    info.fetchBufferReg = static_cast<std::uint32_t>(vertexBufferReg);
    const auto record = [&](std::uint64_t address, std::span<const std::byte> bytes) {
        if (reads == nullptr) return;
        auto& read = reads->emplace_back();
        read.address = address;
        std::copy(bytes.begin(), bytes.end(), read.data.begin());
        read.size = static_cast<std::uint32_t>(bytes.size());
    };
    if (reads != nullptr) reads->reserve(reads->size() + 2u * shader.num_input_semantics);
    constexpr std::uint64_t BatchAttribWords = 64;
    std::array<std::uint32_t, BatchAttribWords> attribBlock{};
    std::array<std::array<std::uint32_t, 4>, ShaderRecompiler::ShaderVertexStageInfo::MaxResources> sharpBlock{};
    std::uint64_t attribFirst = 0, attribCount = 0, sharpFirst = 0, sharpCount = 0;
    if (batched && !staticAbi) {
        std::uint64_t low = ~std::uint64_t{0}, high = 0;
        for (std::uint32_t i = 0; i < shader.num_input_semantics; ++i) {
            low = std::min<std::uint64_t>(low, semantics[i].semantic);
            high = std::max<std::uint64_t>(high, semantics[i].semantic);
        }
        if (high - low < BatchAttribWords) {
            attribFirst = low;
            attribCount = high - low + 1u;
            AgcDriver::GuestMemory::Read(attribTableAddr + attribFirst * 4u, std::as_writable_bytes(std::span(attribBlock.data(), static_cast<std::size_t>(attribCount))), 4);
            std::uint64_t lowIndex = ~std::uint64_t{0}, highIndex = 0;
            for (std::uint32_t i = 0; i < shader.num_input_semantics; ++i) {
                const auto index = static_cast<std::uint64_t>(attribBlock[semantics[i].semantic - attribFirst] & 0x1fu);
                lowIndex = std::min(lowIndex, index);
                highIndex = std::max(highIndex, index);
            }
            sharpFirst = lowIndex;
            sharpCount = highIndex - lowIndex + 1u;
            AgcDriver::GuestMemory::Read(bufferTableAddr + sharpFirst * 16u, std::as_writable_bytes(std::span(sharpBlock.data(), static_cast<std::size_t>(sharpCount))), 4);
        }
    }
    for (std::uint32_t i = 0; i < shader.num_input_semantics; ++i) {
        const auto& semantic = semantics[i];
        if (semantic.static_vb_index == 1 || semantic.static_attribute == 1) throw std::runtime_error("AGC graphics: statically bound vertex attributes are not implemented");
        if (staticAbi) {
            auto& destination = info.resourcesDst.at(info.resourcesNum++);
            destination.registerStart = static_cast<std::int32_t>(semantic.hardware_mapping);
            destination.registersNum = static_cast<std::int32_t>(semantic.size_in_elements);
            destination.attrId = static_cast<std::int32_t>(semantic.semantic);
            continue;
        }
        std::array<std::byte, 4> attribWordBytes{};
        const auto attribWordAddress = attribTableAddr + static_cast<std::uint64_t>(semantic.semantic) * 4u;
        if (attribCount != 0) std::memcpy(attribWordBytes.data(), &attribBlock[semantic.semantic - attribFirst], 4);
        else AgcDriver::GuestMemory::Read(attribWordAddress, attribWordBytes, 4);
        record(attribWordAddress, attribWordBytes);
        std::uint32_t attribWord;
        std::memcpy(&attribWord, attribWordBytes.data(), 4);
        const auto index = attribWord & 0x1fu;
        const auto format = (attribWord >> 5u) & 0x1ffu;
        const auto offset = (attribWord >> 14u) & 0xfffu;
        const auto fetchIndex = (attribWord >> 26u) & 0x1u;
        if (index >= ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex buffer index exceeds the supported domain");
        std::array<std::byte, 16> sharpBytes{};
        const auto sharpAddress = bufferTableAddr + static_cast<std::uint64_t>(index) * 16u;
        if (sharpCount != 0) std::memcpy(sharpBytes.data(), sharpBlock[index - sharpFirst].data(), 16);
        else AgcDriver::GuestMemory::Read(sharpAddress, sharpBytes, 4);
        record(sharpAddress, sharpBytes);
        std::array<std::uint32_t, 4> sharp{};
        std::memcpy(sharp.data(), sharpBytes.data(), 16);
        if (info.resourcesNum >= ShaderRecompiler::ShaderVertexStageInfo::MaxResources) throw std::runtime_error("AGC graphics: vertex resource count exceeds the supported domain");
        auto& resource = info.resources[info.resourcesNum];
        auto& destination = info.resourcesDst[info.resourcesNum];
        resource.fields = sharp;
        destination.registerStart = static_cast<std::int32_t>(semantic.hardware_mapping);
        destination.registersNum = static_cast<std::int32_t>(semantic.size_in_elements);
        destination.attrId = static_cast<std::int32_t>(semantic.semantic);
        destination.fetchIndex = fetchIndex;
        if (format != 0u) {
            const auto bufferFormat = format >> 2u;
            const auto channels = (format & 0x3u) + 1u;
            const auto dstSelY = channels > 1u ? 5u : 0u;
            const auto dstSelZ = channels > 2u ? 6u : 0u;
            const auto dstSelW = channels > 3u ? 7u : 1u;
            const auto dstSel = 4u | (dstSelY << 3u) | (dstSelZ << 6u) | (dstSelW << 9u);
            resource.fields[3] = (resource.fields[3] & ~((0x7fu << 12u) | 0xfffu)) | (bufferFormat << 12u) | dstSel;
        }
        if (offset != 0u) {
            const auto base = ((static_cast<std::uint64_t>(resource.fields[0]) | (static_cast<std::uint64_t>(resource.fields[1]) << 32u)) & 0xffffffffffffull) + offset;
            resource.fields[0] = static_cast<std::uint32_t>(base & 0xffffffffu);
            resource.fields[1] = (resource.fields[1] & 0xffff0000u) | static_cast<std::uint32_t>((base >> 32u) & 0xffffu);
        }
        ++info.resourcesNum;
    }
    return info;
}

ShaderRecompiler::ShaderVertexStageInfo DecodeVertexStageInfo(std::span<const std::byte> header, std::uint64_t headerAddress, std::span<const std::uint32_t> userData, std::vector<DecodeRead>* reads, bool staticAbi, std::uint32_t hiddenUserWords) {
    static const bool batched = [] {
        const char* value = std::getenv("APS5_BATCH_VERTEX_DECODE");
        return value == nullptr || std::string_view(value) != "0";
    }();
    static const bool verify = std::getenv("APS5_VERIFY_VERTEX_DECODE") != nullptr;
    const auto first = reads != nullptr ? reads->size() : 0;
    auto info = decodeVertexStageInfoWith(header, headerAddress, userData, reads, staticAbi, hiddenUserWords, batched);
    if (!verify || !batched) return info;
    std::vector<DecodeRead> checkReads;
    const auto check = decodeVertexStageInfoWith(header, headerAddress, userData, reads != nullptr ? &checkReads : nullptr, staticAbi, hiddenUserWords, false);
    bool same = info.resourcesNum == check.resourcesNum && info.fetchAttribReg == check.fetchAttribReg && info.fetchBufferReg == check.fetchBufferReg && info.fetchEmbedded == check.fetchEmbedded && info.paClVsOutCntl == check.paClVsOutCntl && info.depthRemapMode == check.depthRemapMode;
    for (std::uint32_t r = 0; same && r < info.resourcesNum; ++r) {
        same = info.resources[r].fields == check.resources[r].fields && info.resourcesDst[r].registerStart == check.resourcesDst[r].registerStart && info.resourcesDst[r].registersNum == check.resourcesDst[r].registersNum && info.resourcesDst[r].attrId == check.resourcesDst[r].attrId && info.resourcesDst[r].fetchIndex == check.resourcesDst[r].fetchIndex;
    }
    if (same && reads != nullptr) {
        same = reads->size() - first == checkReads.size();
        for (std::size_t k = 0; same && k < checkReads.size(); ++k) {
            const auto& a = (*reads)[first + k];
            const auto& b = checkReads[k];
            same = a.address == b.address && a.size == b.size && std::equal(a.Bytes().begin(), a.Bytes().end(), b.Bytes().begin());
        }
    }
    if (!same) throw std::runtime_error("APS5_VERIFY_VERTEX_DECODE: the batched vertex stage decode differs from the per-entry decode");
    return info;
}

}
