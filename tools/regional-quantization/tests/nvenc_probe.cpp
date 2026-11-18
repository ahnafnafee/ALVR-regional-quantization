// Offscreen hardware check for HEVC regional quantization. This does not touch SteamVR.
#include "AttentionMap.h"
#include "RowCodingCost.h"
#include "ReplayGeometry.h"
#include "nvEncodeAPI.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl.h>
#include <fstream>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <filesystem>
#include <cstring>
using Microsoft::WRL::ComPtr;
static void Nv(NVENCSTATUS status, const char* step) {
    if (status != NV_ENC_SUCCESS) throw std::runtime_error(std::string(step) + ": NVENC status " + std::to_string(status));
}
static void ReadExact(const std::filesystem::path& path, void* data, size_t bytes) {
    std::ifstream input(path, std::ios::binary);
    if (!input || !input.read(static_cast<char*>(data), std::streamsize(bytes)) || input.peek() != EOF)
        throw std::runtime_error("Invalid reference size: " + path.string());
}
int main(int argc, char** argv) {
    try {
        if (argc < 4) throw std::runtime_error("Usage: nvenc_probe output.hevc bitrateMbps roi(0|1) [--band] [--cqp N] [--stats rows.csv]");
        bool band = false, reconfigure = false, studyPreset = false, continuous = false, capabilities = false, rowBudget = false; int fixedQp = -1; float extraQp = 0;
        float importantWeight = 30, backgroundWeight = 10; uint32_t frames = 144; std::string statsPath, blockStatsPath, frameStatsPath, referencePath;
        gvl::streaming::alvr::MapTuning tuning;
        for (int i = 4; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--band") band = true;
            else if (option == "--capabilities") capabilities = true;
            else if (option == "--extra-qp" && i + 1 < argc) extraQp = std::stof(argv[++i]);
            else if (option == "--reconfigure") reconfigure = true;
            else if (option == "--cqp" && i + 1 < argc) fixedQp = std::stoi(argv[++i]);
            else if (option == "--stats" && i + 1 < argc) statsPath = argv[++i];
            else if (option == "--block-stats" && i + 1 < argc) blockStatsPath = argv[++i];
            else if (option == "--frame-stats" && i + 1 < argc) frameStatsPath = argv[++i];
            else if (option == "--frames" && i + 1 < argc) frames = uint32_t(std::stoul(argv[++i]));
            else if (option == "--study-preset") studyPreset = true;
            else if (option == "--reference" && i + 1 < argc) referencePath = argv[++i];
            else if (option == "--continuous") continuous = true;
            else if (option == "--row-budget") rowBudget = true;
            else if (option == "--boost" && i + 1 < argc) tuning.maximumBoost = std::stoi(argv[++i]);
            else if (option == "--headroom" && i + 1 < argc) tuning.headroom = std::stoi(argv[++i]);
            else if (option == "--contrast" && i + 1 < argc) tuning.contrastExponent = std::stof(argv[++i]);
            else if (option == "--road-floor" && i + 1 < argc) tuning.minimumRoadImportance = std::stof(argv[++i]);
            else if (option == "--hazard-floor" && i + 1 < argc) tuning.minimumHazardImportance = std::stof(argv[++i]);
            else if (option == "--rate-proxy" && i + 1 < argc) tuning.rateProxyTarget = std::stof(argv[++i]);
            else if (option == "--weights" && i + 2 < argc) {
                importantWeight = std::stof(argv[++i]); backgroundWeight = std::stof(argv[++i]);
            }
            else throw std::runtime_error("Unknown or incomplete option " + option);
        }
        if (fixedQp < -1 || fixedQp > 51) throw std::runtime_error("QP out of range");
        if (continuous && referencePath.empty()) throw std::runtime_error("Continuous replay requires a captured sequence");
        if (!frames || frames > 2160 || !std::isfinite(importantWeight) || !std::isfinite(backgroundWeight)
            || backgroundWeight < 1 || importantWeight < backgroundWeight || importantWeight > 100)
            throw std::runtime_error("Invalid frame count or relative weights");
        uint32_t width = 1920, height = band ? 1024 : 1088;
        const uint32_t fps = 72;
        gvl::streaming::alvr::Geometry geometry; geometry.width = width; geometry.height = height;
        if (!referencePath.empty()) {
            gvl::streaming::alvr::ReplayGeometry saved;
            ReadExact(std::filesystem::path(referencePath) / "geometry.bin", &saved, sizeof(saved));
            if (!saved.Valid() || !studyPreset || band || reconfigure || fixedQp >= 0 || frames != 360)
                throw std::runtime_error("Reference replay requires validated geometry, study preset and 360 frames");
            geometry = saved.GeometryValue(); width = geometry.width; height = geometry.height;
        }
        if (tuning.maximumBoost < 1 || tuning.maximumBoost > 12 || tuning.headroom < 0 || tuning.headroom > 3
            || !std::isfinite(tuning.contrastExponent) || tuning.contrastExponent < .5f || tuning.contrastExponent > 1)
            throw std::runtime_error("Invalid map tuning");
        if (!std::isfinite(tuning.minimumRoadImportance) || tuning.minimumRoadImportance < 0 || tuning.minimumRoadImportance > 1
            || !std::isfinite(tuning.minimumHazardImportance) || tuning.minimumHazardImportance < 0 || tuning.minimumHazardImportance > 1)
            throw std::runtime_error("Invalid semantic priority floor");
        if (!std::isfinite(tuning.rateProxyTarget) || tuning.rateProxyTarget < .75f || tuning.rateProxyTarget > 1.3f)
            throw std::runtime_error("Invalid rate proxy target");
        const uint32_t bitrate = uint32_t(std::stoul(argv[2])) * 1000000;
        const bool roi = std::stoi(argv[3]) != 0;
        ComPtr<IDXGIFactory1> factory; CreateDXGIFactory1(IID_PPV_ARGS(&factory));
        ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 desc; adapter->GetDesc1(&desc);
            if (desc.VendorId == 0x10DE) break;
            adapter.Reset();
        }
        if (!adapter) throw std::runtime_error("No NVIDIA adapter");
        ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
        if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context))) throw std::runtime_error("D3D11 device");
        HMODULE module = LoadLibraryW(L"nvEncodeAPI64.dll");
        if (!module) throw std::runtime_error("NVENC library unavailable");
        auto create = reinterpret_cast<NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*)>(GetProcAddress(module, "NvEncodeAPICreateInstance"));
        NV_ENCODE_API_FUNCTION_LIST api{NV_ENCODE_API_FUNCTION_LIST_VER}; Nv(create(&api), "API");
        NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open{NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER};
        open.device = device.Get(); open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX; open.apiVersion = NVENCAPI_VERSION;
        void* encoder = nullptr; Nv(api.nvEncOpenEncodeSessionEx(&open, &encoder), "session");
        if (!statsPath.empty() && !blockStatsPath.empty()) throw std::runtime_error("Choose row or block output statistics");
        if (rowBudget && statsPath.empty()) throw std::runtime_error("Measured-row budgeting requires actual row statistics");
#if defined(NV_ENC_OUTPUT_STATS_ROW_VER)
        NV_ENC_CAPS_PARAM caps{NV_ENC_CAPS_PARAM_VER};
        int blockSupported = 0, rowSupported = 0;
        caps.capsToQuery = NV_ENC_CAPS_OUTPUT_BLOCK_STATS;
        Nv(api.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &caps, &blockSupported), "block stats capability");
        caps.capsToQuery = NV_ENC_CAPS_OUTPUT_ROW_STATS;
        Nv(api.nvEncGetEncodeCaps(encoder, NV_ENC_CODEC_HEVC_GUID, &caps, &rowSupported), "row stats capability");
        if (capabilities) {
            std::cout << "{\"header_output_stats\":true,\"block_stats_supported\":" << blockSupported
                << ",\"row_stats_supported\":" << rowSupported << ",\"api_major\":" << NVENCAPI_MAJOR_VERSION
                << ",\"api_minor\":" << NVENCAPI_MINOR_VERSION << "}\n";
            Nv(api.nvEncDestroyEncoder(encoder), "destroy encoder"); return 0;
        }
        if (!statsPath.empty() && !rowSupported) throw std::runtime_error("GPU does not expose encoded row bitcounts");
        if (!blockStatsPath.empty() && !blockSupported) throw std::runtime_error("GPU does not expose encoded block bitcounts");
#else
        if (capabilities) {
            std::cout << "{\"header_output_stats\":false,\"block_stats_supported\":null,\"row_stats_supported\":null}\n";
            Nv(api.nvEncDestroyEncoder(encoder), "destroy encoder"); return 0;
        }
        if (!statsPath.empty() || !blockStatsPath.empty()) throw std::runtime_error("Output statistics require a Video Codec SDK 12.1 or newer header");
#endif
        NV_ENC_PRESET_CONFIG preset{NV_ENC_PRESET_CONFIG_VER}; preset.presetCfg.version = NV_ENC_CONFIG_VER;
        const GUID presetGuid = studyPreset ? NV_ENC_PRESET_P3_GUID : NV_ENC_PRESET_P1_GUID;
        const auto presetTuning = studyPreset ? NV_ENC_TUNING_INFO_LOW_LATENCY : NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
        Nv(api.nvEncGetEncodePresetConfigEx(encoder, NV_ENC_CODEC_HEVC_GUID, presetGuid, presetTuning, &preset), "preset");
        auto& config = preset.presetCfg;
        config.gopLength = NVENC_INFINITE_GOPLENGTH; config.frameIntervalP = 1;
        config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
        config.rcParams.averageBitRate = config.rcParams.maxBitRate = bitrate;
        config.rcParams.vbvBufferSize = config.rcParams.vbvInitialDelay = bitrate / fps;
        if (fixedQp >= 0) {
            config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CONSTQP;
            config.rcParams.constQP = {uint32_t(fixedQp), uint32_t(fixedQp), uint32_t(fixedQp)};
        }
        config.rcParams.enableAQ = config.rcParams.enableTemporalAQ = config.rcParams.enableLookahead = 0;
        config.rcParams.multiPass = studyPreset ? NV_ENC_TWO_PASS_QUARTER_RESOLUTION : NV_ENC_MULTI_PASS_DISABLED;
        config.rcParams.qpMapMode = (roi || !referencePath.empty()) ? NV_ENC_QP_MAP_DELTA : NV_ENC_QP_MAP_DISABLED;
        config.encodeCodecConfig.hevcConfig.maxCUSize = NV_ENC_HEVC_CUSIZE_32x32;
        config.encodeCodecConfig.hevcConfig.repeatSPSPPS = 1;
        if (!referencePath.empty()) {
            // Match ALVR's authored full-range BT.709 RGB input declaration in both arms.
            auto& vui = config.encodeCodecConfig.hevcConfig.hevcVUIParameters;
            vui.videoSignalTypePresentFlag = 1; vui.videoFormat = NV_ENC_VUI_VIDEO_FORMAT_UNSPECIFIED; vui.videoFullRangeFlag = 1;
            vui.colourDescriptionPresentFlag = 1;
            vui.colourPrimaries = NV_ENC_VUI_COLOR_PRIMARIES_BT709;
            vui.transferCharacteristics = NV_ENC_VUI_TRANSFER_CHARACTERISTIC_SRGB;
            vui.colourMatrix = NV_ENC_VUI_MATRIX_COEFFS_BT709;
        }
        NV_ENC_INITIALIZE_PARAMS init{NV_ENC_INITIALIZE_PARAMS_VER};
        init.encodeGUID = NV_ENC_CODEC_HEVC_GUID; init.presetGUID = presetGuid;
        init.tuningInfo = presetTuning;
        init.encodeWidth = init.darWidth = init.maxEncodeWidth = width;
        init.encodeHeight = init.darHeight = init.maxEncodeHeight = height;
        init.frameRateNum = fps; init.frameRateDen = 1; init.enablePTD = 1; init.encodeConfig = &config;
#if defined(NV_ENC_OUTPUT_STATS_ROW_VER)
        if (!statsPath.empty()) { init.enableOutputStats = 1; init.outputStatsLevel = NV_ENC_OUTPUT_STATS_ROW_LEVEL; }
        if (!blockStatsPath.empty()) { init.enableOutputStats = 1; init.outputStatsLevel = NV_ENC_OUTPUT_STATS_BLOCK_LEVEL; }
#endif
        Nv(api.nvEncInitializeEncoder(encoder, &init), "initialize");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width; desc.Height = height; desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT; desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture; if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) throw std::runtime_error("texture");
        NV_ENC_REGISTER_RESOURCE reg{NV_ENC_REGISTER_RESOURCE_VER};
        reg.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX; reg.width = width; reg.height = height;
        reg.resourceToRegister = texture.Get(); reg.bufferFormat = NV_ENC_BUFFER_FORMAT_ABGR; reg.bufferUsage = NV_ENC_INPUT_IMAGE;
        Nv(api.nvEncRegisterResource(encoder, &reg), "register");
        NV_ENC_CREATE_BITSTREAM_BUFFER stream{NV_ENC_CREATE_BITSTREAM_BUFFER_VER}; Nv(api.nvEncCreateBitstreamBuffer(encoder, &stream), "bitstream");
        gvl::streaming::alvr::MapBuilder map; map.Configure(geometry);
        gvl::streaming::alvr::RowCodingCost codingCost; codingCost.Configure(width, height);
        gvl::streaming::alvr::Packet packet{}; packet.importantWeight = 30; packet.backgroundWeight = 10; packet.count = 2;
        packet.regions[0] = {.25f, .25f, .75f, .75f, 0, 1}; packet.regions[1] = {.25f, .25f, .75f, .75f, 1, 1};
        if (band) {
            // Complete CTU rows allow the Ampere hardware row bitcounts to separate
            // important and background payload without guessing a per-block split.
            packet.regions[0] = {0, .250001f, 1, .749999f, 0, 1};
            packet.regions[1] = {0, .250001f, 1, .749999f, 1, 1};
        }
        if (extraQp > 0) { packet.importantWeight = 70; packet.backgroundWeight = 3; }
        packet.importantWeight = importantWeight; packet.backgroundWeight = backgroundWeight;
        map.Build(packet, float(bitrate) / 1e6f, extraQp, tuning);
        std::vector<uint32_t> pixels(size_t(width) * height);
        std::ofstream output(argv[1], std::ios::binary);
        if (!output) throw std::runtime_error("Cannot open bitstream output");
        std::ofstream stats;
        std::ofstream blockOutput;
        std::ofstream frameStats;
        if (!frameStatsPath.empty()) {
            frameStats.open(frameStatsPath); if (!frameStats) throw std::runtime_error("Frame statistics output");
            frameStats << "frame,bytes,average_qp,encode_and_lock_ms,source_index,qp_min,qp_max,rate_proxy,map_valid,important_weight,background_weight\n";
        }
        if (!statsPath.empty()) {
            stats.open(statsPath);
            if (!stats) throw std::runtime_error("Cannot open statistics output");
            stats << "frame,row,important,bits,row_qp,frame_bytes\n";
        }
        if (!blockStatsPath.empty()) {
            blockOutput.open(blockStatsPath); if (!blockOutput) throw std::runtime_error("Block statistics output");
            blockOutput << "frame,source_index,block_x,block_y,bits,actual_qp,frame_bytes\n";
        }
#if defined(NV_ENC_OUTPUT_STATS_ROW_VER)
        std::vector<NV_ENC_OUTPUT_STATS_ROW> rowStats((height + 31) / 32);
        for (auto& row : rowStats) row.version = NV_ENC_OUTPUT_STATS_ROW_VER;
        const uint32_t blockColumns = (width + 31) / 32;
        std::vector<NV_ENC_OUTPUT_STATS_BLOCK> blockStats(size_t(blockColumns) * ((height + 31) / 32));
        for (auto& block : blockStats) block.version = NV_ENC_OUTPUT_STATS_BLOCK_VER;
#endif
        uint64_t bytes = 0; uint32_t largest = 0; int previousSource = -1;
        for (uint32_t f = 0; f < frames; ++f) {
            if (reconfigure && (f == frames / 3 || f == 2 * frames / 3)) {
                const uint32_t budget = f == frames / 3 ? uint32_t(bitrate * .65) : bitrate;
                config.rcParams.averageBitRate = config.rcParams.maxBitRate = budget;
                config.rcParams.vbvBufferSize = config.rcParams.vbvInitialDelay = budget / fps;
                NV_ENC_RECONFIGURE_PARAMS change{NV_ENC_RECONFIGURE_PARAMS_VER};
                change.reInitEncodeParams = init;
                Nv(api.nvEncReconfigureEncoder(encoder, &change), "regional bitrate reconfigure");
            }
            const int sourceIndex = continuous ? int(f) : f < 72 ? 0 : int((f - 72) / 9);
            bool validMap = true;
            if (!referencePath.empty()) {
                if (sourceIndex != previousSource) {
                    const auto base = std::filesystem::path(referencePath) / ("frame_" + std::to_string(sourceIndex));
                    ReadExact(base.string() + ".rgba", pixels.data(), pixels.size() * sizeof(pixels[0]));
                    ReadExact(base.string() + ".packet", &packet, sizeof(packet));
                    validMap = gvl::streaming::Valid(packet, packet.qpc, packet.frequency, packet.producerPid);
                    const gvl::streaming::Packet empty{};
                    if (!validMap && (!continuous || std::memcmp(&packet, &empty, sizeof(packet)) != 0))
                        throw std::runtime_error("Invalid captured region packet");
                    if (validMap) map.Build(packet, float(bitrate) / 1e6f, 0, tuning, rowBudget ? &codingCost.blocks : nullptr);
                    previousSource = sourceIndex;
                }
            } else for (uint32_t y = 0; y < height; ++y) for (uint32_t x = 0; x < width; ++x) {
                // Identical moving fine detail in both eyes/regions; reproducible across encodes.
                const uint32_t sx = (x % (width / 2) + f * 3) % (width / 2), sy = (y + f) % height;
                uint32_t h = (sx / 2) * 374761393u + (sy / 2) * 668265263u;
                h = (h ^ (h >> 13)) * 1274126177u;
                pixels[size_t(y) * width + x] = 0xff000000u | (h & 0x00ffffffu);
            }
            context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), width * 4, 0);
            NV_ENC_MAP_INPUT_RESOURCE input{NV_ENC_MAP_INPUT_RESOURCE_VER}; input.registeredResource = reg.registeredResource;
            Nv(api.nvEncMapInputResource(encoder, &input), "map");
            NV_ENC_PIC_PARAMS pic{NV_ENC_PIC_PARAMS_VER};
            pic.inputBuffer = input.mappedResource; pic.bufferFmt = NV_ENC_BUFFER_FORMAT_ABGR;
            pic.inputWidth = width; pic.inputHeight = height; pic.outputBitstream = stream.bitstreamBuffer;
            pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME; pic.inputTimeStamp = f;
            if (f == 0) pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR;
            if (roi && validMap) { pic.qpDeltaMap = map.deltas.data(); pic.qpDeltaMapSize = uint32_t(map.deltas.size()); }
            const auto encodeStart = std::chrono::steady_clock::now();
            Nv(api.nvEncEncodePicture(encoder, &pic), "encode");
            NV_ENC_LOCK_BITSTREAM lock{NV_ENC_LOCK_BITSTREAM_VER}; lock.outputBitstream = stream.bitstreamBuffer;
#if defined(NV_ENC_OUTPUT_STATS_ROW_VER)
            if (!statsPath.empty()) {
                lock.outputStatsPtr = rowStats.data();
                lock.outputStatsPtrSize = uint32_t(rowStats.size() * sizeof(rowStats[0]));
            }
            if (!blockStatsPath.empty()) {
                lock.outputStatsPtr = blockStats.data();
                lock.outputStatsPtrSize = uint32_t(blockStats.size() * sizeof(blockStats[0]));
            }
#endif
            Nv(api.nvEncLockBitstream(encoder, &lock), "lock");
            const double encodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - encodeStart).count();
            if (frameStats) frameStats << f << ',' << lock.bitstreamSizeInBytes << ',' << lock.frameAvgQP << ',' << encodeMs
                << ',' << (!referencePath.empty() ? sourceIndex : -1) << ',' << (roi && validMap ? map.minDelta : 0)
                << ',' << (roi && validMap ? map.maxDelta : 0) << ',' << (roi && validMap ? map.rateProxy : 1)
                << ',' << (validMap ? 1 : 0) << ',' << (validMap ? packet.importantWeight : 0)
                << ',' << (validMap ? packet.backgroundWeight : 0) << '\n';
#if defined(NV_ENC_OUTPUT_STATS_ROW_VER)
            if (!statsPath.empty()) for (uint32_t row = 0; row < rowStats.size(); ++row) {
                const bool important = band && row >= height / 128 && row < 3 * height / 128;
                stats << f << ',' << row << ',' << important << ',' << rowStats[row].bitcount
                    << ',' << int(rowStats[row].QP) << ',' << lock.bitstreamSizeInBytes << '\n';
            }
            if (rowBudget && f > 0) {
                std::vector<uint32_t> rowBits(rowStats.size());
                std::vector<uint8_t> rowQp(rowStats.size());
                for (uint32_t row = 0; row < rowStats.size(); ++row) {
                    rowBits[row] = rowStats[row].bitcount; rowQp[row] = rowStats[row].QP;
                }
                if (!codingCost.Observe(rowBits.data(), rowQp.data(), uint32_t(rowBits.size()), lock.frameAvgQP))
                    throw std::runtime_error("Measured row cost update failed");
            }
            if (!blockStatsPath.empty()) for (uint32_t block = 0; block < blockStats.size(); ++block)
                blockOutput << f << ',' << (!referencePath.empty() ? sourceIndex : -1) << ',' << block % blockColumns
                    << ',' << block / blockColumns << ',' << blockStats[block].bitcount << ',' << int(blockStats[block].QP)
                    << ',' << lock.bitstreamSizeInBytes << '\n';
#endif
            output.write(static_cast<const char*>(lock.bitstreamBufferPtr), lock.bitstreamSizeInBytes);
            bytes += lock.bitstreamSizeInBytes; largest = std::max(largest, lock.bitstreamSizeInBytes);
            Nv(api.nvEncUnlockBitstream(encoder, stream.bitstreamBuffer), "unlock");
            Nv(api.nvEncUnmapInputResource(encoder, input.mappedResource), "unmap");
        }
        Nv(api.nvEncDestroyBitstreamBuffer(encoder, stream.bitstreamBuffer), "destroy output");
        Nv(api.nvEncUnregisterResource(encoder, reg.registeredResource), "unregister");
        Nv(api.nvEncDestroyEncoder(encoder), "destroy encoder");
        double relativeRateProxy = 0;
        for (auto delta : map.deltas) relativeRateProxy += std::exp2(-delta / 6.0);
        relativeRateProxy /= map.deltas.size();
        std::cout << "{\"frames\":" << frames << ",\"width\":" << width << ",\"height\":" << height
            << ",\"extra_peripheral_qp\":" << extraQp << ",\"reconfigured\":" << (reconfigure ? "true" : "false")
            << ",\"fixed_qp\":" << fixedQp << ",\"band\":" << (band ? "true" : "false")
            << ",\"study_preset\":" << (studyPreset ? "true" : "false")
            << ",\"reference_replay\":" << (!referencePath.empty() ? "true" : "false")
            << ",\"continuous_reference\":" << (continuous ? "true" : "false")
            << ",\"row_statistics\":" << (!statsPath.empty() ? "true" : "false")
            << ",\"block_statistics\":" << (!blockStatsPath.empty() ? "true" : "false")
            << ",\"measured_row_budget\":" << (rowBudget ? "true" : "false")
            << ",\"maximum_boost\":" << tuning.maximumBoost << ",\"headroom\":" << tuning.headroom
            << ",\"contrast_exponent\":" << tuning.contrastExponent
            << ",\"minimum_road_importance\":" << tuning.minimumRoadImportance
            << ",\"minimum_hazard_importance\":" << tuning.minimumHazardImportance
            << ",\"rate_proxy_target\":" << tuning.rateProxyTarget
            << ",\"weight_source\":\"" << (referencePath.empty() ? "command_line" : "per_frame_reference_packet") << '"'
            << ",\"important_weight\":" << (referencePath.empty() ? std::to_string(importantWeight) : "null")
            << ",\"background_weight\":" << (referencePath.empty() ? std::to_string(backgroundWeight) : "null")
            << ",\"actual_mbps\":" << (bytes * 8.0 * fps / frames / 1e6)
            << ",\"max_frame_bytes\":" << largest << ",\"qp_min\":" << (roi ? map.minDelta : 0)
            << ",\"qp_max\":" << (roi ? map.maxDelta : 0)
            << ",\"relative_rate_proxy\":" << (roi ? relativeRateProxy : 1) << "}\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
