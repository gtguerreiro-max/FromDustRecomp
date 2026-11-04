#pragma once

// Each executable linking the native renderer opts into its bundled SDK.
extern "C" {
__declspec(dllexport) extern const unsigned int D3D12SDKVersion = 614;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}
