#include "EncoderFrameTelemetry.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace gvl::streaming::alvr;
int main() try {
    SetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", nullptr);
    EncoderFrameTelemetry disabled;
    if (disabled.Begin(100, 64, 32, false, 0, 0, 0, 0, 0) != 0 || disabled.Ready())
        throw std::runtime_error("Ordinary encoding enabled diagnostics");
    wchar_t temporary[32768]{}; GetTempPathW(32768, temporary);
    const auto root = std::filesystem::path(temporary) / (L"gvl-encoder-telemetry-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("Test directory already exists");
    SetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", root.c_str());
    {
        EncoderFrameTelemetry enabled;
        for (int i = 0; !enabled.Ready() && i < 1000; ++i) Sleep(1);
        if (!enabled.Ready()) throw std::runtime_error("Telemetry writer did not start");
        const auto first = enabled.Begin(123, 64, 32, true, 99, 10, 20, -3, 12);
        const auto second = enabled.Begin(123, 64, 32, false, 99, 10, 20, -3, 12);
        if (first != 1 || second != 2) throw std::runtime_error("Repeated poses did not receive distinct encoder IDs");
        enabled.SubmissionResult(first, true, true, 0);
        enabled.SubmissionResult(second, true, false, 0);
        enabled.Complete(second, 567, 35);
        enabled.Complete(first, 1234, 31);
        enabled.Complete(first, 1234, 31);
        if (enabled.InvalidOutputs() != 1) throw std::runtime_error("Duplicate output was silently reused");
        const auto third = enabled.Begin(124, 64, 32, true, 99, 11, 21, -3, 12);
        enabled.SubmissionResult(third, true, true, 0);
        enabled.Complete(third, 100, 99);
        if (enabled.InvalidOutputs() != 2) throw std::runtime_error("Invalid quantizer was accepted");
        enabled.Begin(125, 64, 32, true, 99, 12, 22, -3, 12); // deliberately incomplete
    }
    std::ifstream file(root / "encoder-frames.jsonl");
    std::string header, second, first, footer, extra, line;
    unsigned clocks = 0;
    std::getline(file, header); std::getline(file, second); std::getline(file, first);
    while (std::getline(file, line)) {
        if (line.find("\"kind\":\"clock\"") != std::string::npos) { ++clocks; continue; }
        footer = line; break;
    }
    std::getline(file, extra); file.close();
    if (header.find("\"physical_motion_to_photon_ms\":null") == std::string::npos
        || header.find("\"periodic_clock_anchors\":true") == std::string::npos)
        throw std::runtime_error("Software telemetry claimed physical motion-to-photon latency");
    if (first.find("\"frame_id\":1,") == std::string::npos || second.find("\"frame_id\":2,") == std::string::npos
        || first.find("\"bitstream_bytes\":1234,") == std::string::npos || first.find("\"frame_average_qp\":31,") == std::string::npos)
        throw std::runtime_error("Reordered outputs were joined to the wrong submissions");
    if (second.find("\"map_applied\":false,\"producer_pid\":0,\"packet_sequence\":0,\"unity_frame\":0,") == std::string::npos
        || first.find("\"submission_confirmed\":true,\"encoder_status\":0,\"map_applied\":true") == std::string::npos)
        throw std::runtime_error("Fallback output retained old map provenance");
    if (footer.find("\"frames\":2,\"invalid_or_missing_outputs\":3,\"write_failed\":false") == std::string::npos || !extra.empty())
        throw std::runtime_error("Telemetry footer did not reconcile missing/invalid output");
    {
        EncoderFrameTelemetry existing;
        Sleep(25);
        if (existing.Ready()) throw std::runtime_error("An existing measurement was overwritten");
    }
    if (std::filesystem::file_size(root / "encoder-frames.jsonl") < 100)
        throw std::runtime_error("Existing measurement was truncated");
    const auto live = root / L"sentinel";
    std::filesystem::create_directory(live);
    SetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", live.c_str());
    {
        EncoderFrameTelemetry enabled;
        for (int i = 0; !enabled.Ready() && i < 1000; ++i) Sleep(1);
        Sleep(1150); // The background clock must progress even without encoder output.
        const auto id = enabled.Begin(10, 64, 32, false, 0, 0, 0, 0, 0);
        enabled.SubmissionResult(id, true, false, 0);
        { std::ofstream signal(live / "telemetry.stop"); signal << "complete"; }
        for (int i = 0; enabled.Ready() && i < 1000; ++i) Sleep(1);
        // The output already in flight must still be accepted after new submissions stop.
        enabled.Complete(id, 200, 30);
        for (int i = 0; !std::filesystem::exists(live / "encoder-complete.json") && i < 1000; ++i) Sleep(1);
        if (!std::filesystem::exists(live / "encoder-complete.json") || enabled.Ready()
            || enabled.Begin(11, 64, 32, false, 0, 0, 0, 0, 0) != 0)
            throw std::runtime_error("Explicit diagnostic stop did not seal a live process");
        std::ifstream sealed(live / "encoder-complete.json"); std::string result;
        std::getline(sealed, result); sealed.close();
        if (result.find("\"frames\":1,\"invalid_or_missing_outputs\":0,\"write_failed\":false") == std::string::npos)
            throw std::runtime_error("Explicit stop dropped the in-flight output");
        std::ifstream observations(live / "encoder-frames.jsonl");
        bool clockObserved = false;
        while (std::getline(observations, line))
            if (line.find("\"kind\":\"clock\"") != std::string::npos
                && line.find("\"utc_qpc_before\":") != std::string::npos
                && line.find("\"utc_filetime_100ns\":") != std::string::npos) clockObserved = true;
        observations.close();
        if (!clockObserved) throw std::runtime_error("An idle writer omitted actual host clock anchors");
    }
    SetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", nullptr);
    std::filesystem::remove_all(root);
    std::cout << "15 encoder telemetry checks passed\n";
} catch (const std::exception& e) {
    SetEnvironmentVariableW(L"GVL_ALVR_ENCODER_TELEMETRY", nullptr);
    std::cerr << e.what() << '\n'; return 1;
}
