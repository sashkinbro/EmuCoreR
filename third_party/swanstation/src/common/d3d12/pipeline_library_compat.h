#pragma once

// Forward declarations for ID3D12PipelineLibrary and the two error
// HRESULTs the library load can return. These are present in the
// official Windows SDK / MSVC d3d12.h but the mingw-w64 d3d12.h ships
// only the CreatePipelineLibrary method signature on ID3D12Device1;
// the interface itself and the error codes are missing.
//
// We forward-declare them here under the same guards the SDK header
// uses, so on a toolchain that does have them we no-op and pick up
// the SDK declaration, and on one that doesn't we provide a complete
// definition matching the published Microsoft spec. The interface and
// IID have been stable since D3D12.1 (April 2017, Windows 10 v1703).
//
// Reference:
//   https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nn-d3d12-id3d12pipelinelibrary
//   https://learn.microsoft.com/en-us/windows/win32/direct3d12/d3d12-error-codes

#include "../windows_headers.h"
#include <d3d12.h>

// Driver returned a serialized library blob from an incompatible
// driver version. Not an error in the bug sense - the right response
// is to delete the file and create a fresh library.
#ifndef D3D12_ERROR_DRIVER_VERSION_MISMATCH
#define D3D12_ERROR_DRIVER_VERSION_MISMATCH _HRESULT_TYPEDEF_(0x887E0002L)
#endif

// Adapter the cached library was serialized against is not present.
// Same recovery action: delete + recreate.
#ifndef D3D12_ERROR_ADAPTER_NOT_FOUND
#define D3D12_ERROR_ADAPTER_NOT_FOUND _HRESULT_TYPEDEF_(0x887E0001L)
#endif

#ifndef __ID3D12PipelineLibrary_INTERFACE_DEFINED__
#define __ID3D12PipelineLibrary_INTERFACE_DEFINED__

DEFINE_GUID(IID_ID3D12PipelineLibrary, 0xc64226a8, 0x9201, 0x46af, 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7,
            0x41, 0x4f);

// MIDL_INTERFACE expands to __declspec(uuid(...)) struct on MSVC and
// an empty marker on mingw-w64. Both produce a usable polymorphic
// COM interface; the destructor of a Microsoft::WRL::ComPtr<T> will
// call T::Release() through the inherited IUnknown vtable. The COM
// vtable layout is fixed by the standard (IUnknown's 3 slots, then
// ID3D12Object's 4, then ID3D12DeviceChild's 1, then this interface's
// 5), so the IID is what binds runtime-loaded ID3D12PipelineLibrary
// objects to this declaration.
MIDL_INTERFACE("c64226a8-9201-46af-b4cc-53fb9ff7414f")
ID3D12PipelineLibrary : public ID3D12DeviceChild
{
public:
  virtual HRESULT STDMETHODCALLTYPE StorePipeline(LPCWSTR pName, ID3D12PipelineState* pPipeline) = 0;

  virtual HRESULT STDMETHODCALLTYPE LoadGraphicsPipeline(LPCWSTR pName,
                                                         const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc, REFIID riid,
                                                         void** ppPipelineState) = 0;

  virtual HRESULT STDMETHODCALLTYPE LoadComputePipeline(LPCWSTR pName,
                                                        const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc, REFIID riid,
                                                        void** ppPipelineState) = 0;

  virtual SIZE_T STDMETHODCALLTYPE GetSerializedSize(void) = 0;

  virtual HRESULT STDMETHODCALLTYPE Serialize(void* pData, SIZE_T DataSizeInBytes) = 0;
};

// mingw-w64's __CRT_UUID_DECL macro registers the IID so __uuidof()
// works on the interface. On MSVC this is provided by MIDL_INTERFACE
// already (via __declspec(uuid)), so this is mingw-only.
#ifdef __CRT_UUID_DECL
__CRT_UUID_DECL(ID3D12PipelineLibrary, 0xc64226a8, 0x9201, 0x46af, 0xb4, 0xcc, 0x53, 0xfb, 0x9f, 0xf7, 0x41, 0x4f)
#endif

#endif // __ID3D12PipelineLibrary_INTERFACE_DEFINED__
