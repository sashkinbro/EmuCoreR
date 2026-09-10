#include "shader_compiler.h"
#include "../log.h"
Log_SetChannel(D3D11);

namespace D3D11::ShaderCompiler {

ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11VertexShader> shader;
  const HRESULT hr = device->CreateVertexShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create vertex shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11VertexShader> CreateVertexShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateVertexShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                            const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11GeometryShader> shader;
  const HRESULT hr = device->CreateGeometryShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create geometry shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11GeometryShader> CreateGeometryShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateGeometryShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                              const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11PixelShader> shader;
  const HRESULT hr = device->CreatePixelShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create pixel shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11PixelShader> CreatePixelShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreatePixelShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                           const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const void* bytecode, size_t bytecode_length)
{
  ComPtr<ID3D11ComputeShader> shader;
  const HRESULT hr = device->CreateComputeShader(bytecode, bytecode_length, nullptr, shader.GetAddressOf());
  if (FAILED(hr))
  {
    Log_ErrorPrintf("Failed to create compute shader: 0x%08X", hr);
    return {};
  }

  return shader;
}

ComPtr<ID3D11ComputeShader> CreateComputeShader(ID3D11Device* device, const ID3DBlob* blob)
{
  return CreateComputeShader(device, const_cast<ID3DBlob*>(blob)->GetBufferPointer(),
                             const_cast<ID3DBlob*>(blob)->GetBufferSize());
}

} // namespace D3D11::ShaderCompiler
