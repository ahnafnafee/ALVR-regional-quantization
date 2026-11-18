#include "DynamicReferenceCapture.h"
#include <iostream>
using namespace gvl::streaming::alvr;
int main() try {
    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
        D3D11_SDK_VERSION, &device, nullptr, &context))) throw std::runtime_error("WARP device");
    D3D11_TEXTURE2D_DESC desc{}; desc.Width = 64; desc.Height = 32;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.Usage = D3D11_USAGE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &texture))) throw std::runtime_error("Test texture");
    wchar_t temporary[32768]{}; GetTempPathW(32768, temporary);
    const auto root = std::filesystem::path(temporary) / (L"gvl-dynamic-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("Test directory already exists");
    Geometry geometry; geometry.width = 64; geometry.height = 32;
    Packet packet{}; packet.magic = gvl::streaming::Magic; packet.version = 1; packet.bytes = sizeof(packet);
    packet.producerPid = 123; packet.frequency = 1000; packet.count = 1;
    packet.importantWeight = 70; packet.backgroundWeight = 3; packet.maxAgeMs = 75;
    packet.regions[0] = {0, 0, 1, 1, 0, 1};
    SetEnvironmentVariableW(L"GVL_ALVR_DYNAMIC_CAPTURE", nullptr);
    { DynamicReferenceCapture off; off.Capture(texture.Get(), context.Get(), 1, packet, geometry, true); }
    if (!std::filesystem::is_empty(root)) throw std::runtime_error("Disabled capture wrote files");
    SetEnvironmentVariableW(L"GVL_ALVR_DYNAMIC_CAPTURE", root.c_str());
    { std::ofstream arm(root / "capture.arm"); arm << "development"; }
    {
        DynamicReferenceCapture capture;
        std::vector<uint32_t> pixels(64 * 32);
        for (uint32_t i = 0; i < 368; ++i) {
            std::fill(pixels.begin(), pixels.end(), 0xff332200u | (i & 255));
            context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
            packet.sequence = i + 1; packet.qpc = i + 1;
            const uint64_t targetTimestamp = (i == 160 || i == 161 ? 160 : i + 1) * 13888889ull;
            capture.Capture(texture.Get(), context.Get(), targetTimestamp, packet, geometry, i != 123);
            context->Flush(); Sleep(2);
        }
    }
    if (std::filesystem::exists(root / "capture-error.txt")) throw std::runtime_error("Capture failed");
    if (!std::filesystem::exists(root / "development/complete.json")) throw std::runtime_error("Continuous capture incomplete");
    std::ifstream receipt(root / "development/complete.json");
    const std::string completed((std::istreambuf_iterator<char>(receipt)), {});
    receipt.close();
    if (completed.find("windows_qpc") == std::string::npos || completed.find("first_capture_qpc") == std::string::npos)
        throw std::runtime_error("Input capture clock receipt missing");
    if (std::filesystem::exists(root / "development/frame_360.rgba")) throw std::runtime_error("Frame cap exceeded");
    for (uint32_t i = 0; i < 360; ++i) {
        uint8_t rgba[4]{}; std::ifstream image(root / ("development/frame_" + std::to_string(i) + ".rgba"), std::ios::binary);
        image.read(reinterpret_cast<char*>(rgba), 4);
        if (rgba[0] != 0x33 || rgba[1] != 0x22 || rgba[2] != (i & 255) || rgba[3] != 0xff)
            throw std::runtime_error("Frame was repeated, reordered, or converted incorrectly");
        Packet saved{}; std::ifstream file(root / ("development/frame_" + std::to_string(i) + ".packet"), std::ios::binary);
        file.read(reinterpret_cast<char*>(&saved), sizeof(saved));
        if (saved.sequence != (i == 123 ? 0 : i + 1)) throw std::runtime_error("Fallback reused a stale packet");
    }
    { DynamicReferenceCapture duplicate; duplicate.Capture(texture.Get(), context.Get(), 1, packet, geometry, true); }
    if (!std::filesystem::exists(root / "capture-error.txt")) throw std::runtime_error("Existing group not rejected");
    SetEnvironmentVariableW(L"GVL_ALVR_DYNAMIC_CAPTURE", nullptr);
    // This process created and owns the exact temporary test directory above.
    std::filesystem::remove_all(root);
    std::cout << "726 dynamic reference checks passed, including repeated tracking targets\n";
} catch (const std::exception& e) {
    SetEnvironmentVariableW(L"GVL_ALVR_DYNAMIC_CAPTURE", nullptr);
    std::cerr << e.what() << '\n'; return 1;
}
