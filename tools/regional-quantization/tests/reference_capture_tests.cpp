#include "EncoderReferenceCapture.h"
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
    std::vector<uint32_t> pixels(64 * 32, 0xff332211);
    context->UpdateSubresource(texture.Get(), 0, nullptr, pixels.data(), 64 * 4, 0);
    wchar_t temporary[32768]{}; GetTempPathW(32768, temporary);
    const auto root = std::filesystem::path(temporary) / (L"gvl-reference-test-" + std::to_wstring(GetCurrentProcessId()));
    if (!std::filesystem::create_directory(root)) throw std::runtime_error("Test directory already exists");
    Geometry geometry; geometry.width = 64; geometry.height = 32;
    Packet packet{}; packet.count = 6;
    for (uint32_t eye = 0; eye < 2; ++eye) for (uint32_t j = 0; j < 3; ++j)
        packet.regions[eye * 3 + j] = {0, 0, 1, 1, eye, j == 0 ? 1u : j == 1 ? 2u : 4u};
    SetEnvironmentVariableW(L"GVL_ALVR_REFERENCE_CAPTURE", nullptr);
    EncoderReferenceCapture disabled;
    disabled.Capture(texture.Get(), context.Get(), 1, packet, geometry);
    if (!std::filesystem::is_empty(root)) throw std::runtime_error("Disabled readback wrote files");
    SetEnvironmentVariableW(L"GVL_ALVR_REFERENCE_CAPTURE", root.c_str());
    EncoderReferenceCapture enabled;
    { std::ofstream arm(root / "capture.arm"); arm << "development"; }
    for (int i = 0; i < 18; ++i) enabled.Capture(texture.Get(), context.Get(), i, packet, geometry);
    if (!std::filesystem::exists(root / "development/frame_0.rgba")) throw std::runtime_error("Armed capture missing");
    std::filesystem::remove(root / "capture.arm"); Sleep(510);
    for (int i = 0; i < 18; ++i) enabled.Capture(texture.Get(), context.Get(), i, packet, geometry);
    if (std::filesystem::exists(root / "development/frame_1.rgba")) throw std::runtime_error("Disarmed capture continued");
    { std::ofstream arm(root / "capture.arm"); arm << "development"; } Sleep(510);
    for (int i = 0; i < 18 * 31; ++i) enabled.Capture(texture.Get(), context.Get(), i, packet, geometry);
    if (!std::filesystem::exists(root / "development/complete.json")) throw std::runtime_error("Bounded capture incomplete");
    for (int i = 0; i < 100; ++i) enabled.Capture(texture.Get(), context.Get(), i, packet, geometry);
    if (std::filesystem::exists(root / "development/frame_32.rgba")) throw std::runtime_error("Frame cap exceeded");
    std::ifstream first(root / "development/frame_0.rgba", std::ios::binary);
    uint8_t rgba[4]{}; first.read(reinterpret_cast<char*>(rgba), 4); first.close();
    if (rgba[0] != 0x33 || rgba[1] != 0x22 || rgba[2] != 0x11 || rgba[3] != 0xff)
        throw std::runtime_error("BGRA reference conversion wrong");
    EncoderReferenceCapture duplicate;
    duplicate.Capture(texture.Get(), context.Get(), 1, packet, geometry);
    if (!std::filesystem::exists(root / "capture-error.txt")) throw std::runtime_error("Existing capture not rejected");
    SetEnvironmentVariableW(L"GVL_ALVR_REFERENCE_CAPTURE", nullptr);
    // Only this process's freshly-created temporary directory is owned by this test.
    std::filesystem::remove_all(root);
    std::cout << "7 reference capture checks passed\n";
} catch (const std::exception& e) {
    SetEnvironmentVariableW(L"GVL_ALVR_REFERENCE_CAPTURE", nullptr);
    std::cerr << e.what() << '\n'; return 1;
}
