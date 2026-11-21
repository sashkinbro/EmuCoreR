#pragma once
#include "../windows_headers.h"
#include <d3d11.h>
#include <type_traits>
#include <wrl/client.h>

namespace D3D11::ShaderCompiler {
template<typename T>
using ComPtr = Microsoft::WRL::ComPtr<T>;

ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length);
ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const ID3DBlob* blob);
ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length);
ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const ID3DBlob* blob);
ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length);
ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const ID3DBlob* blob);
ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length);
ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const ID3DBlob* blob);

}; // namespace D3D11::ShaderCompiler
