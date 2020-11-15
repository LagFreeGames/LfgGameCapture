/*
 * Copyright 2020 Lag Free Games, LLC
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#pragma once

#include "StcCommon.h"

#include "Stc_d3d12compatibility.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma warning(push)
#pragma warning(disable : 4820)

typedef enum StcFormat {
    STC_FORMAT_R16G16B16A16_FLOAT,
    STC_FORMAT_R10G10B10A2_UNORM,
    STC_FORMAT_R8G8B8A8_SRGB,
    STC_FORMAT_B8G8R8A8_SRGB,
    STC_FORMAT_R10G10B10_XR_BIAS_A2_UNORM,
} StcFormat;

typedef struct StcServerGraphicsInfo {
    UINT width;
    UINT height;
    StcFormat format;
} StcServerGraphicsInfo;

typedef struct StcServerD3D11NextInfo {
    ID3D11Texture2D* pTexture;
    size_t index;
} StcServerD3D11NextInfo;

typedef struct StcServerD3D12NextInfo {
    ID3D12Resource* pTexture;
    size_t index;
} StcServerD3D12NextInfo;

typedef struct StcServerBase {
    // Create initialized
    StcMessageCallbacks messenger;
    TCHAR pNameBuffer[256];
    uint64_t nextConnectToken;
    bool clientInProgress;
    StcServerGraphicsInfo graphicsInfo;
    HANDLE hGlobalMapFile;
    StcGlobalInfo* pGlobalInfo;
    bool initialized;

    // Tick initialized
    int64_t clientFirstSeen;

    // MakeConnection initialized
    HANDLE hMapFile;
    StcInfo* pInfo;
    size_t copyIndex;
    bool needResize[STC_TEXTURE_COUNT];
} StcServerBase;

typedef struct StcServerD3D11 {
    StcServerBase base;

    // Create initialized
    ID3D11Device* pDevice;
    StcD3D11AllocationCallbacks allocator;
    bool usesLegacyHandles;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule12;
#endif
    ID3D11Device5* pDevice11_5;
    ID3D11DeviceContext4* pContext11_4;
    ID3D12Device* pDevice12;
    ID3D11On12Device* pDevice11On12;
    ID3D12CompatibilityDevice* pCompatibilityDevice;

    // Tick initialized
    ID3D11Texture2D* pTextures[STC_TEXTURE_COUNT];
    IDXGIKeyedMutex* pKeyedMutexes[STC_TEXTURE_COUNT];
    ID3D11Texture2D* pTextures11On12[STC_TEXTURE_COUNT];
    ID3D11Fence* pWriteFences11On12[STC_TEXTURE_COUNT];
    ID3D11Fence* pReadFences11On12[STC_TEXTURE_COUNT];
} StcServerD3D11;

typedef struct StcServerD3D12 {
    StcServerBase base;

    // Create initialized
    HANDLE hFenceClearedAutoEvent;
    ID3D12Device* pDevice;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule11;
#endif
    ID3D11On12Device* pDevice11On12;
    ID3D12CompatibilityDevice* pCompatibilityDevice;
    StcD3D12AllocationCallbacks allocator;

    // Tick initialized
    ID3D12Resource* pTextures[STC_TEXTURE_COUNT];
    ID3D12Fence* pWriteFences[STC_TEXTURE_COUNT];
    ID3D12Fence* pReadFences[STC_TEXTURE_COUNT];
    ID3D11Texture2D* pTextures11[STC_TEXTURE_COUNT];
    IDXGIKeyedMutex* pKeyedMutexes11[STC_TEXTURE_COUNT];
} StcServerD3D12;

#pragma warning(pop)

StcServerStatus StcServerD3D11Create(StcServerD3D11* pServer, const TCHAR* pPrefix, const StcServerGraphicsInfo* pGraphicsInfo,
                                     ID3D11Device* pDevice, const StcD3D11AllocationCallbacks* pAllocator,
                                     const StcMessageCallbacks* pMessenger);
StcServerStatus StcServerD3D12Create(StcServerD3D12* pServer, const TCHAR* pPrefix, const StcServerGraphicsInfo* pGraphicsInfo,
                                     ID3D12Device* pDevice, const StcD3D12AllocationCallbacks* pAllocator,
                                     const StcMessageCallbacks* pMessenger);
void StcServerD3D11Destroy(StcServerD3D11* pServer);
void StcServerD3D12Destroy(StcServerD3D12* pServer);
void StcServerD3D11ResizeBuffers(StcServerD3D11* pServer, UINT width, UINT height, StcFormat format);
void StcServerD3D12ResizeBuffers(StcServerD3D12* pServer, UINT width, UINT height, StcFormat format);
StcServerStatus StcServerD3D11Tick(StcServerD3D11* pServer, StcServerD3D11NextInfo* pNextInfo);
StcServerStatus StcServerD3D12Tick(StcServerD3D12* pServer, StcServerD3D12NextInfo* pNextInfo);
StcServerStatus StcServerD3D11WaitForClientRead(StcServerD3D11* pServer);
StcServerStatus StcServerD3D12WaitForClientRead(StcServerD3D12* pServer, ID3D12CommandQueue* pQueue);
StcServerStatus StcServerD3D11SignalWrite(StcServerD3D11* pServer);
StcServerStatus StcServerD3D12SignalWrite(StcServerD3D12* pServer, ID3D12CommandQueue* pQueue);

#ifdef __cplusplus
}
#endif
