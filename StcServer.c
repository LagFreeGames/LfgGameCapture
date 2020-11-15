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

#include "StcServer.h"

#include "StcMisc.h"

#pragma comment(lib, "d3d11")
#pragma comment(lib, "dxguid")
#pragma warning(disable : 4710)
#pragma warning(disable : 4711)
#pragma warning(disable : 5045)

static StcServerStatus OpenServer(StcServerBase* const pBase, StcGlobalInfo* const pGlobalInfo) {
    StcServerStatus status = STC_SERVER_STATUS_SUCCESS;

    const StcMessageCallbacks* const pMessenger = &pBase->messenger;

    TCHAR pNameBuffer[256];
    const int result = stc_stprintf(pNameBuffer, _countof(pNameBuffer), TEXT("%") STC_TSTRINGWIDTH TEXT("s_%llu"),
                                    pBase->pNameBuffer, (unsigned long long)pBase->nextConnectToken);
    if (result < 0 || result >= _countof(pNameBuffer)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_CONNECTION_STRING_FORMAT, result);
        status = STC_SERVER_STATUS_FAIL_STRING_FORMAT;
        goto fail0;
    }

    const HANDLE hMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, STC_MAP_SIZE, pNameBuffer);
    if (hMapFile == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_CREATE_CONNECTION_FILE_MAPPING, GetLastError());
        status = STC_SERVER_STATUS_FAIL_CREATE_CONNECTION_FILE_MAPPING;
        goto fail0;
    }

    StcInfo* const pInfo = MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StcInfo));
    if (pInfo == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_MAP_CONNECTION_INFO, GetLastError());
        status = STC_SERVER_STATUS_FAIL_MAP_CONNECTION_INFO;
        goto fail1;
    }

    StcAtomicUint32StoreRelaxed(&pInfo->pendingWrites, STC_TEXTURE_COUNT - 1);
    StcAtomicUint32StoreRelaxed(&pInfo->pendingReads, 0);
    StcAtomicInt64StoreRelaxed(&pInfo->serverKeepAlive, StcGetCurrentTicks());

    StcAtomicInt64Store(&pGlobalInfo->connectToken, pBase->nextConnectToken);
    ++pBase->nextConnectToken;

    pBase->hMapFile = hMapFile;
    pBase->pInfo = pInfo;
    pBase->copyIndex = STC_TEXTURE_COUNT - 1;
    for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
        pInfo->writeFenceValues12[i] = 0;
        pInfo->readFenceValues12[i] = 0;
        pInfo->invalidated[i] = false;
        pBase->needResize[i] = true;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CONNECTION_READY);
    goto success;

fail1:
    CloseHandle(hMapFile);
fail0:
success:
    return status;
}

static void CloseServerD3D11(StcServerD3D11* const pServer, const StcServerStopReason reason) {
    StcServerBase* const pBase = &pServer->base;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo) {
        StcAtomicUint32Store(&pInfo->serverStopReason, reason);

        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            if (pServer->pTextures[i]) {
                pServer->allocator.pfnDestroy(pServer->allocator.pUserData, i);

                ID3D11Texture2D_Release(pServer->pTextures[i]);
                pServer->pTextures[i] = NULL;
                IDXGIKeyedMutex_Release(pServer->pKeyedMutexes[i]);
                pServer->pKeyedMutexes[i] = NULL;
                if (!pServer->usesLegacyHandles) {
                    CloseHandle((HANDLE)(uintptr_t)pInfo->hTextures[i]);
                }

                if (pServer->pTextures11On12[i] != NULL) {
                    ID3D11Texture2D_Release(pServer->pTextures11On12[i]);
                    pServer->pTextures11On12[i] = NULL;
                    ID3D11Fence_Release(pServer->pWriteFences11On12[i]);
                    pServer->pWriteFences11On12[i] = NULL;
                    ID3D11Fence_Release(pServer->pReadFences11On12[i]);
                    pServer->pReadFences11On12[i] = NULL;
                    CloseHandle((HANDLE)(uintptr_t)pInfo->hWriteFences12[i]);
                    CloseHandle((HANDLE)(uintptr_t)pInfo->hReadFences12[i]);
                }
            }
        }

        UnmapViewOfFile(pInfo);
        CloseHandle(pBase->hMapFile);

        pBase->pInfo = NULL;
    }
}

static void CloseServerD3D12(StcServerD3D12* const pServer, const StcServerStopReason reason) {
    StcServerBase* const pBase = &pServer->base;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo) {
        StcAtomicUint32Store(&pInfo->serverStopReason, reason);

        const size_t copyIndex = pBase->copyIndex;
        if (pServer->pTextures[copyIndex] != NULL) {
            ID3D12Fence* const fence = pServer->pWriteFences[copyIndex];
            const UINT64 fenceValue = pInfo->writeFenceValues12[copyIndex];
            if (ID3D12Fence_GetCompletedValue(fence) < fenceValue) {
                const HANDLE hFenceClearedAutoEvent = pServer->hFenceClearedAutoEvent;
                ID3D12Fence_SetEventOnCompletion(fence, fenceValue, hFenceClearedAutoEvent);
                WaitForSingleObject(hFenceClearedAutoEvent, INFINITE);
            }
        }

        for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
            if (pServer->pTextures[i]) {
                pServer->allocator.pfnDestroy(pServer->allocator.pUserData, i);

                ID3D12Resource_Release(pServer->pTextures[i]);
                pServer->pTextures[i] = NULL;
                CloseHandle((HANDLE)(uintptr_t)pInfo->hTextures[i]);

                ID3D12Fence_Release(pServer->pWriteFences[i]);
                pServer->pWriteFences[i] = NULL;

                if (pServer->pTextures11[i] != NULL) {
                    ID3D11Texture2D_Release(pServer->pTextures11[i]);
                    pServer->pTextures11[i] = NULL;
                    IDXGIKeyedMutex_Release(pServer->pKeyedMutexes11[i]);
                    pServer->pKeyedMutexes11[i] = NULL;
                } else {
                    ID3D12Fence_Release(pServer->pReadFences[i]);
                    pServer->pReadFences[i] = NULL;
                    CloseHandle((HANDLE)(uintptr_t)pInfo->hWriteFences12[i]);
                    CloseHandle((HANDLE)(uintptr_t)pInfo->hReadFences12[i]);
                }
            }
        }

        UnmapViewOfFile(pInfo);
        CloseHandle(pBase->hMapFile);

        pBase->pInfo = NULL;
    }
}

static StcServerStatus ReopenServerD3D11(StcServerD3D11* const pServer, const StcServerStopReason reason,
                                         StcGlobalInfo* const pGlobalInfo) {
    StcServerBase* const pBase = &pServer->base;
    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_SERVER_D3D11_CONNECTION_RESET);

    CloseServerD3D11(pServer, reason);
    return OpenServer(pBase, pGlobalInfo);
}

static StcServerStatus ReopenServerD3D12(StcServerD3D12* const pServer, const StcServerStopReason reason,
                                         StcGlobalInfo* const pGlobalInfo) {
    StcServerBase* const pBase = &pServer->base;
    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_SERVER_D3D12_CONNECTION_RESET);

    CloseServerD3D12(pServer, reason);
    return OpenServer(pBase, pGlobalInfo);
}

static StcServerStatus StcServerCreate(StcServerBase* const pBase, const TCHAR* const pPrefix,
                                       const StcServerGraphicsInfo* const pGraphicsInfo, const StcMessageCallbacks* pMessenger,
                                       const StcApi serverApi) {
    StcServerStatus status = STC_SERVER_STATUS_SUCCESS;

    if (pMessenger) {
        pBase->messenger = *pMessenger;
    } else {
        pBase->messenger.pUserData = NULL;
        pBase->messenger.pfnMessage = NULL;
        pMessenger = &pBase->messenger;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_VERSION, STC_MAJOR_VERSION, STC_MINOR_VERSION, STC_PATCH_VERSION,
                  StcGetApiName(serverApi));

    const int result = stc_stprintf(pBase->pNameBuffer, _countof(pBase->pNameBuffer), TEXT("%") STC_TSTRINGWIDTH TEXT("s_%u"),
                                    pPrefix, (unsigned)GetCurrentProcessId());
    if (result < 0 || result >= _countof(pBase->pNameBuffer)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_GLOBAL_STRING_FORMAT, result);
        status = STC_SERVER_STATUS_FAIL_STRING_FORMAT;
        goto fail0;
    }

    HANDLE hGlobalMapFile = CreateFileMapping(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0, STC_MAP_SIZE, pBase->pNameBuffer);
    if (hGlobalMapFile == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_CREATE_GLOBAL_FILE_MAPPING, GetLastError());
        status = STC_SERVER_STATUS_FAIL_CREATE_GLOBAL_FILE_MAPPING;
        goto fail0;
    }

    StcGlobalInfo* const pGlobalInfo = MapViewOfFile(hGlobalMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(StcGlobalInfo));
    if (pGlobalInfo == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_MAP_GLOBAL_INFO, GetLastError());
        status = STC_SERVER_STATUS_FAIL_MAP_GLOBAL_INFO;
        goto fail1;
    }

    pGlobalInfo->version = STC_MAJOR_VERSION;
    pGlobalInfo->serverApi = serverApi;

    pBase->nextConnectToken = 1;

    status = OpenServer(pBase, pGlobalInfo);
    if (status != STC_SERVER_STATUS_SUCCESS) {
        goto fail2;
    }

    pBase->clientInProgress = false;

    pBase->graphicsInfo = *pGraphicsInfo;

    pBase->hGlobalMapFile = hGlobalMapFile;
    pBase->pGlobalInfo = pGlobalInfo;
    pBase->initialized = true;

    goto success;

fail2:
    UnmapViewOfFile(pGlobalInfo);
fail1:
    CloseHandle(hGlobalMapFile);
fail0:
    pBase->initialized = false;
success:
    return status;
}

static bool StcCreateFunctionD3D11Null(void* pUserData, size_t index, ID3D11Texture2D* pTexture) {
    (void)pUserData;
    (void)index;
    (void)pTexture;

    return true;
}

static void StcDestroyFunctionD3D11Null(void* pUserData, size_t index) {
    (void)pUserData;
    (void)index;
}

static bool StcCreateFunctionD3D12Null(void* pUserData, size_t index, ID3D12Resource* pTexture) {
    (void)pUserData;
    (void)index;
    (void)pTexture;

    return true;
}

static void StcDestroyFunctionD3D12Null(void* pUserData, size_t index) {
    (void)pUserData;
    (void)index;
}

static const IID Stc_IID_ID3D12CompatibilityDevice = {
    0x8F1C0E3C,
    0xFAE3,
    0x4A82,
    {
        0xB0,
        0x98,
        0xBF,
        0xE1,
        0x70,
        0x82,
        0x07,
        0xFF,
    },
};

typedef struct Interop12For11 {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule12;
#endif
    ID3D11Device5* pDevice11_5;
    ID3D11DeviceContext4* pContext11_4;
    ID3D12Device* pDevice12;
    ID3D11On12Device* pDevice11On12;
    ID3D12CompatibilityDevice* pCompatibilityDevice;
} Interop12For11;

static void CreateInterop12For11(ID3D11Device* const pDevice, const StcMessageCallbacks* const pMessenger,
                                 Interop12For11* const pInterop) {
    ID3D11Device5* pDevice11_5 = NULL;
    ID3D11DeviceContext4* pContext11_4 = NULL;
    ID3D12Device* pDevice12 = NULL;
    ID3D11On12Device* pDevice11On12 = NULL;
    ID3D12CompatibilityDevice* pCompatibilityDevice = NULL;

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule12 = LoadLibrary(TEXT("D3D12"));
    if (hModule12 == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_LOADLIBRARY_D3D12);
        goto fail0;
    }

    const HMODULE hModule11 = GetModuleHandle(TEXT("D3D11"));
    if (hModule11 == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_GETMODULEHANDLE_D3D11);
        goto fail1;
    }

    const PFN_D3D12_CREATE_DEVICE createDevice12Function = (PFN_D3D12_CREATE_DEVICE)GetProcAddress(hModule12, "D3D12CreateDevice");
    if (createDevice12Function == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_GETPROCADDRESS_D3D12CREATEDEVICE);
        goto fail1;
    }

    const PFN_D3D11ON12_CREATE_DEVICE createDevice11On12Function =
        (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(hModule11, "D3D11On12CreateDevice");
    if (createDevice11On12Function == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_GETPROCADDRESS_D3D11ON12CREATEDEVICE);
        goto fail1;
    }
#else
    const PFN_D3D12_CREATE_DEVICE createDevice12Function = &D3D12CreateDevice;
    const PFN_D3D11ON12_CREATE_DEVICE createDevice11On12Function = &D3D11On12CreateDevice;
#endif

    if (FAILED(ID3D11Device_QueryInterface(pDevice, &IID_ID3D11Device5, &pDevice11_5))) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11DEVICE5);
        goto fail1;
    }

    ID3D11DeviceContext* pContext;
    ID3D11Device_GetImmediateContext(pDevice, &pContext);
    HRESULT hr = ID3D11DeviceContext_QueryInterface(pContext, &IID_ID3D11DeviceContext4, &pContext11_4);
    ID3D11DeviceContext_Release(pContext);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11DEVICECONTEXT4);
        goto fail2;
    }

    IDXGIDevice* pDxgiDevice;
    if (FAILED(ID3D11Device_QueryInterface(pDevice, &IID_IDXGIDevice, &pDxgiDevice))) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_QUERYINTERFACE_IDXGIDEVICE);
        goto fail3;
    }

    IDXGIAdapter* pAdapter;
    hr = IDXGIDevice_GetAdapter(pDxgiDevice, &pAdapter);
    IDXGIDevice_Release(pDxgiDevice);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_GETADAPTER);
        goto fail3;
    }

    hr = createDevice12Function((IUnknown*)pAdapter, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, &pDevice12);
    IDXGIAdapter_Release(pAdapter);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_D3D12CREATEDEVICE);
        goto fail3;
    }

    ID3D11Device* pDevice11;
    if (FAILED(createDevice11On12Function((IUnknown*)pDevice12, 0, NULL, 0, NULL, 0, 0, &pDevice11, NULL, NULL))) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_D3D11ON12CREATEDEVICE);
        goto fail4;
    }

    hr = ID3D11Device_QueryInterface(pDevice11, &IID_ID3D11On12Device, &pDevice11On12);
    ID3D11On12Device_Release(pDevice11);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_12_FOR_11_QUERYINTERFACE_ID3D11ON12DEVICE);
        goto fail4;
    }

    if (FAILED(ID3D12Device_QueryInterface(pDevice12, &Stc_IID_ID3D12CompatibilityDevice, &pCompatibilityDevice))) {
        goto fail5;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D12_CLIENT_ALLOWED);
    goto success;

fail5:
    ID3D11On12Device_Release(pDevice11On12);
    pDevice11On12 = NULL;
fail4:
    ID3D12Device_Release(pDevice12);
    pDevice12 = NULL;
fail3:
    ID3D11DeviceContext4_Release(pContext11_4);
    pContext11_4 = NULL;
fail2:
    ID3D11Device5_Release(pDevice11_5);
    pDevice11_5 = NULL;
fail1:
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    FreeLibrary(hModule12);
    hModule12 = NULL;
fail0:
#endif
success:

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    pInterop->hModule12 = hModule12;
#endif
    pInterop->pDevice11_5 = pDevice11_5;
    pInterop->pContext11_4 = pContext11_4;
    pInterop->pDevice12 = pDevice12;
    pInterop->pDevice11On12 = pDevice11On12;
    pInterop->pCompatibilityDevice = pCompatibilityDevice;
}

StcServerStatus StcServerD3D11Create(StcServerD3D11* const pServer, const TCHAR* const pPrefix,
                                     const StcServerGraphicsInfo* const pGraphicsInfo, ID3D11Device* const pDevice,
                                     const StcD3D11AllocationCallbacks* const pAllocator,
                                     const StcMessageCallbacks* pMessenger) {
    StcServerBase* const pBase = &pServer->base;
    StcServerStatus status = StcServerCreate(pBase, pPrefix, pGraphicsInfo, pMessenger, STC_API_D3D11);
    if (status != STC_SERVER_STATUS_SUCCESS) {
        goto fail0;
    }

    pMessenger = &pBase->messenger;

    pServer->pDevice = pDevice;

    for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
        pServer->pTextures[i] = NULL;
        pServer->pKeyedMutexes[i] = NULL;
        pServer->pTextures11On12[i] = NULL;
        pServer->pWriteFences11On12[i] = NULL;
        pServer->pReadFences11On12[i] = NULL;
    }

    if (pAllocator) {
        pServer->allocator = *pAllocator;
    } else {
        pServer->allocator.pfnCreate = StcCreateFunctionD3D11Null;
        pServer->allocator.pfnDestroy = StcDestroyFunctionD3D11Null;
    }

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    pServer->usesLegacyHandles = !IsWindows8OrGreater();
#else
    pServer->usesLegacyHandles = false;
#endif

    Interop12For11 interop;
    CreateInterop12For11(pDevice, pMessenger, &interop);
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    pServer->hModule12 = interop.hModule12;
#endif
    pServer->pDevice11_5 = interop.pDevice11_5;
    pServer->pContext11_4 = interop.pContext11_4;
    pServer->pDevice12 = interop.pDevice12;
    pServer->pDevice11On12 = interop.pDevice11On12;
    pServer->pCompatibilityDevice = interop.pCompatibilityDevice;

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CREATE_D3D11_SUCCESS);

fail0:
    return status;
}

typedef struct Interop11For12 {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule11;
#endif
    ID3D11On12Device* pDevice11On12;
    ID3D12CompatibilityDevice* pCompatibilityDevice;
} Interop11For12;

static void CreateInterop11For12(ID3D12Device* const pDevice, const StcMessageCallbacks* const pMessenger,
                                 Interop11For12* const pInterop) {
    ID3D11On12Device* pDevice11On12 = NULL;
    ID3D12CompatibilityDevice* pCompatibilityDevice = NULL;

    PFN_D3D11ON12_CREATE_DEVICE createDeviceFunction = NULL;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    HMODULE hModule = LoadLibrary(TEXT("D3D11"));
    if (hModule == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_11_FOR_12_LOADLIBRARY_D3D11);
        goto fail0;
    }

    createDeviceFunction = (PFN_D3D11ON12_CREATE_DEVICE)GetProcAddress(hModule, "D3D11On12CreateDevice");
    if (createDeviceFunction == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_11_FOR_12_GETPROCADDRESS_D3D11ON12CREATEDEVICE);
        goto fail1;
    }
#else
    createDeviceFunction = &D3D11On12CreateDevice;
#endif

    ID3D11Device* pDevice11;
    if (FAILED(createDeviceFunction((IUnknown*)pDevice, 0, NULL, 0, NULL, 0, 0, &pDevice11, NULL, NULL))) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_11_FOR_12_D3D11ON12CREATEDEVICE);
        goto fail1;
    }

    const HRESULT hr = ID3D11Device_QueryInterface(pDevice11, &IID_ID3D11On12Device, &pDevice11On12);
    ID3D11Device_Release(pDevice11);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_11_FOR_12_QUERYINTERFACE_ID3D11ON12DEVICE);
        goto fail1;
    }

    if (FAILED(ID3D12Device_QueryInterface(pDevice, &Stc_IID_ID3D12CompatibilityDevice, &pCompatibilityDevice))) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_11_FOR_12_QUERYINTERFACE_ID3D12COMPATIBILITYDEVICE);
        goto fail2;
    }

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D11_CLIENT_ALLOWED);
    goto success;

fail2:
    ID3D11On12Device_Release(pDevice11On12);
    pDevice11On12 = NULL;
fail1:
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    FreeLibrary(hModule);
    hModule = NULL;
fail0:
#endif
success:

#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    pInterop->hModule11 = hModule;
#endif
    pInterop->pDevice11On12 = pDevice11On12;
    pInterop->pCompatibilityDevice = pCompatibilityDevice;
}

StcServerStatus StcServerD3D12Create(StcServerD3D12* const pServer, const TCHAR* const pPrefix,
                                     const StcServerGraphicsInfo* const pGraphicsInfo, ID3D12Device* const pDevice,
                                     const StcD3D12AllocationCallbacks* const pAllocator, const StcMessageCallbacks* pMessenger) {
    StcServerStatus status;

    const HANDLE hFenceClearedAutoEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
    if (hFenceClearedAutoEvent == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_CREATE_FENCE_EVENT);
        status = STC_SERVER_STATUS_FAIL_CREATE_EVENT;
        goto fail0;
    }

    StcServerBase* const pBase = &pServer->base;
    status = StcServerCreate(pBase, pPrefix, pGraphicsInfo, pMessenger, STC_API_D3D12);
    if (status != STC_SERVER_STATUS_SUCCESS) {
        goto fail1;
    }

    pMessenger = &pBase->messenger;

    pServer->hFenceClearedAutoEvent = hFenceClearedAutoEvent;
    pServer->pDevice = pDevice;

    for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
        pServer->pTextures[i] = NULL;
        pServer->pWriteFences[i] = NULL;
        pServer->pReadFences[i] = NULL;
        pServer->pTextures11[i] = NULL;
        pServer->pKeyedMutexes11[i] = NULL;
    }

    if (pAllocator) {
        pServer->allocator = *pAllocator;
    } else {
        pServer->allocator.pfnCreate = StcCreateFunctionD3D12Null;
        pServer->allocator.pfnDestroy = StcDestroyFunctionD3D12Null;
    }

    Interop11For12 interop;
    CreateInterop11For12(pDevice, pMessenger, &interop);
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
    pServer->hModule11 = interop.hModule11;
#endif
    pServer->pDevice11On12 = interop.pDevice11On12;
    pServer->pCompatibilityDevice = interop.pCompatibilityDevice;

    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CREATE_D3D12_SUCCESS);
    goto success;

fail1:
    CloseHandle(hFenceClearedAutoEvent);
fail0:
success:
    return status;
}

void StcServerD3D11Destroy(StcServerD3D11* const pServer) {
    StcServerBase* const pBase = &pServer->base;
    if (pBase->initialized) {
        CloseServerD3D11(pServer, STC_SERVER_STOP_REASON_DESTROY);

        ID3D11Device5* const pDevice11_5 = pServer->pDevice11_5;
        if (pDevice11_5) {
            ID3D12CompatibilityDevice_Release(pServer->pCompatibilityDevice);
            ID3D11On12Device_Release(pServer->pDevice11On12);
            ID3D12Device_Release(pServer->pDevice12);
            ID3D11DeviceContext4_Release(pServer->pContext11_4);
            ID3D11Device5_Release(pDevice11_5);
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
            FreeLibrary(pServer->hModule12);
#endif
        }

        UnmapViewOfFile(pBase->pGlobalInfo);
        CloseHandle(pBase->hGlobalMapFile);

        pBase->initialized = false;
    }

    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_SERVER_DESTROY_D3D11_SUCCESS);
}

void StcServerD3D12Destroy(StcServerD3D12* const pServer) {
    StcServerBase* const pBase = &pServer->base;
    if (pBase->initialized) {
        CloseServerD3D12(pServer, STC_SERVER_STOP_REASON_DESTROY);

        ID3D11On12Device* const pDevice11On12 = pServer->pDevice11On12;
        if (pDevice11On12) {
            ID3D12CompatibilityDevice_Release(pServer->pCompatibilityDevice);
            ID3D11On12Device_Release(pDevice11On12);
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP)
            FreeLibrary(pServer->hModule11);
#endif
        }

        UnmapViewOfFile(pBase->pGlobalInfo);
        CloseHandle(pBase->hGlobalMapFile);

        CloseHandle(pServer->hFenceClearedAutoEvent);

        pBase->initialized = false;
    }

    StcLogMessage(&pBase->messenger, STC_MESSAGE_ID_SERVER_DESTROY_D3D12_SUCCESS);
}

static void StcServerResizeBuffers(StcServerBase* const pBase, const UINT width, const UINT height, const StcFormat format) {
    StcServerGraphicsInfo* const pGraphicsInfo = &pBase->graphicsInfo;
    pGraphicsInfo->width = width;
    pGraphicsInfo->height = height;
    pGraphicsInfo->format = format;

    for (size_t i = 0; i < STC_TEXTURE_COUNT; ++i) {
        pBase->needResize[i] = true;
    }
}

void StcServerD3D11ResizeBuffers(StcServerD3D11* const pServer, const UINT width, const UINT height, const StcFormat format) {
    StcServerResizeBuffers(&pServer->base, width, height, format);
}

void StcServerD3D12ResizeBuffers(StcServerD3D12* const pServer, const UINT width, const UINT height, const StcFormat format) {
    StcServerResizeBuffers(&pServer->base, width, height, format);
}

typedef struct ResourceFrame11 {
    ID3D11Texture2D* pTexture;
    IDXGIKeyedMutex* pKeyedMutex;
    HANDLE hTexture;

    ID3D11Texture2D* pTexture11On12;
    ID3D11Fence* pWriteFence11On12;
    ID3D11Fence* pReadFence11On12;
    HANDLE hWriteFence11On12;
    HANDLE hReadFence11On12;
} ResourceFrame11;

typedef struct ResourceFrame12 {
    ID3D12Resource* pTexture;
    ID3D12Fence* pWriteFence;
    ID3D12Fence* pReadFence;
    HANDLE hTexture;
    HANDLE hWriteFence;
    HANDLE hReadFence;

    ID3D11Texture2D* pTexture11;
    IDXGIKeyedMutex* pKeyedMutex11;
} ResourceFrame12;

D3D11_BIND_FLAG ComputeD3D11BindFlags(StcBindFlags flags) {
    D3D11_BIND_FLAG flagsD3D11 = 0;

    if (flags & STC_BIND_FLAG_SHADER_RESOURCE) {
        flagsD3D11 |= D3D11_BIND_SHADER_RESOURCE;
    }
    if (flags & STC_BIND_FLAG_RENDER_TARGET) {
        flagsD3D11 |= D3D11_BIND_RENDER_TARGET;
    }
    if (flags & STC_BIND_FLAG_UNORDERED_ACCESS) {
        flagsD3D11 |= D3D11_BIND_UNORDERED_ACCESS;
    }

    return flagsD3D11;
}

D3D12_RESOURCE_FLAGS ComputeD3D12ResourceFlags(StcBindFlags flags) {
    D3D12_RESOURCE_FLAGS flagsD3D12 = D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    // Debug layer doesn't like D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE alone.
    // Ignore STC_BIND_FLAG_SHADER_RESOURCE for simplicity.

    // if ((flags & STC_BIND_FLAG_SHADER_RESOURCE) == 0) {
    //    flagsD3D12 |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
    // }

    if (flags & STC_BIND_FLAG_RENDER_TARGET) {
        flagsD3D12 |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    if (flags & STC_BIND_FLAG_UNORDERED_ACCESS) {
        flagsD3D12 |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    }

    return flagsD3D12;
}

static DXGI_FORMAT ConvertFormat(const StcFormat format, const StcSrgbChannelType srgbChannelType) {
    DXGI_FORMAT dxgiFormat;
    switch (format) {
        case STC_FORMAT_R16G16B16A16_FLOAT:
            dxgiFormat = DXGI_FORMAT_R16G16B16A16_FLOAT;
            break;
        case STC_FORMAT_R10G10B10A2_UNORM:
            dxgiFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
            break;
        case STC_FORMAT_R8G8B8A8_SRGB:
            switch (srgbChannelType) {
                case STC_SRGB_CHANNEL_TYPE_UNORM:
                    dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
                    break;
                case STC_SRGB_CHANNEL_TYPE_UNORM_SRGB:
                    dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                    break;
                case STC_SRGB_CHANNEL_TYPE_TYPELESS:
                    dxgiFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
                    break;
                case STC_SRGB_CHANNEL_TYPE_MAX_ENUM:
                default:
                    dxgiFormat = DXGI_FORMAT_UNKNOWN;
            }
            break;
        case STC_FORMAT_B8G8R8A8_SRGB:
            switch (srgbChannelType) {
                case STC_SRGB_CHANNEL_TYPE_UNORM:
                    dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
                    break;
                case STC_SRGB_CHANNEL_TYPE_UNORM_SRGB:
                    dxgiFormat = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
                    break;
                case STC_SRGB_CHANNEL_TYPE_TYPELESS:
                    dxgiFormat = DXGI_FORMAT_B8G8R8A8_TYPELESS;
                    break;
                case STC_SRGB_CHANNEL_TYPE_MAX_ENUM:
                default:
                    dxgiFormat = DXGI_FORMAT_UNKNOWN;
            }
            break;
        case STC_FORMAT_R10G10B10_XR_BIAS_A2_UNORM:
            dxgiFormat = STC_FORMAT_R10G10B10_XR_BIAS_A2_UNORM;
            break;
        default:
            dxgiFormat = DXGI_FORMAT_UNKNOWN;
    }

    return dxgiFormat;
}

static StcServerStopReason CreateD3D11ResourceFrame(const StcServerD3D11* const pServer, bool need12,
                                                    ResourceFrame11* const pFrame) {
    StcServerStopReason reason = STC_CLIENT_STOP_REASON_NONE;

    const StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    const StcServerGraphicsInfo* const pGraphicsInfo = &pBase->graphicsInfo;
    StcInfo* const pInfo = pBase->pInfo;
    ID3D11Device* const pDevice = pServer->pDevice;

    ID3D11Texture2D* pTexture;
    IDXGIKeyedMutex* pKeyedMutex = NULL;
    HANDLE hTexture;
    ID3D11Texture2D* pTexture11On12 = NULL;
    ID3D11Fence* pWriteFence11On12 = NULL;
    ID3D11Fence* pReadFence11On12 = NULL;
    HANDLE hWriteFence11On12 = NULL;
    HANDLE hReadFence11On12 = NULL;

    const DXGI_FORMAT format = ConvertFormat(pGraphicsInfo->format, pInfo->srgbChannelType);

    if (need12) {
        ID3D11Device5* const pDevice11_5 = pServer->pDevice11_5;
        if (pDevice11_5 == NULL) {
            reason = STC_SERVER_STOP_REASON_MISSING_11_TO_12_SUPPORT;
            goto fail0;
        }

        D3D12_HEAP_PROPERTIES heapProperties;
        heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
        heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
        heapProperties.CreationNodeMask = 0;
        heapProperties.VisibleNodeMask = 0;

        D3D12_RESOURCE_DESC resourceDesc;
        resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        resourceDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
        resourceDesc.Width = pGraphicsInfo->width;
        resourceDesc.Height = pGraphicsInfo->height;
        resourceDesc.DepthOrArraySize = 1;
        resourceDesc.MipLevels = 1;
        resourceDesc.Format = format;
        resourceDesc.SampleDesc.Count = 1;
        resourceDesc.SampleDesc.Quality = 0;
        resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        resourceDesc.Flags = ComputeD3D12ResourceFlags(pInfo->clientBindFlags);

        D3D11_RESOURCE_FLAGS flags11;
        flags11.BindFlags = ComputeD3D11BindFlags(pInfo->clientBindFlags);
        flags11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        flags11.CPUAccessFlags = 0;
        flags11.StructureByteStride = 0;

        ID3D12Resource* pTexture12;
        HRESULT hr = ID3D12CompatibilityDevice_CreateSharedResource(
            pServer->pCompatibilityDevice, &heapProperties, D3D12_HEAP_FLAG_SHARED, &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
            NULL, &flags11, D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX, NULL, NULL, &IID_ID3D12Resource, &pTexture12);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_COMPATIBILITY_TEXTURE;
            goto fail0;
        }

        hr = ID3D11On12Device_CreateWrappedResource(pServer->pDevice11On12, (IUnknown*)pTexture12, &flags11,
                                                    D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COMMON, &IID_ID3D11Texture2D,
                                                    &pTexture11On12);
        ID3D12Resource_Release(pTexture12);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_WRAPPED_RESOURCE;
            goto fail0;
        }

        IDXGIResource1* pDxgiResource1;
        hr = ID3D11Texture2D_QueryInterface(pTexture11On12, &IID_IDXGIResource1, &pDxgiResource1);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_QUERY_DXGI_RESOURCE1;
            goto fail12_0;
        }

        HANDLE hTexture11;
        hr = IDXGIResource1_CreateSharedHandle(pDxgiResource1, NULL, GENERIC_ALL, NULL, &hTexture11);
        IDXGIResource1_Release(pDxgiResource1);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
            goto fail12_0;
        }

        hr = ID3D11Device1_OpenSharedResource1(pDevice11_5, hTexture11, &IID_ID3D11Texture2D, &pTexture);
        CloseHandle(hTexture11);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_OPEN_SHARED_HANDLE;
            goto fail12_0;
        }
    } else {
        D3D11_TEXTURE2D_DESC desc;
        desc.Width = pGraphicsInfo->width;
        desc.Height = pGraphicsInfo->height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = ComputeD3D11BindFlags(pInfo->clientBindFlags);
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = pServer->usesLegacyHandles ? D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
                                                    : (D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE);

        HRESULT hr = ID3D11Device_CreateTexture2D(pDevice, &desc, NULL, &pTexture);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_TEXTURE_11;
            goto fail0;
        }
    }

    HRESULT hr = ID3D11Texture2D_QueryInterface(pTexture, &IID_IDXGIKeyedMutex, &pKeyedMutex);
    if (FAILED(hr)) {
        reason = STC_SERVER_STOP_REASON_FAIL_QUERY_KEYED_MUTEX;
        goto fail1;
    }

    hr = IDXGIKeyedMutex_AcquireSync(pKeyedMutex, STC_KEY_INITIAL, 0);
    if (FAILED(hr) || (hr == WAIT_ABANDONED) || (hr == WAIT_TIMEOUT)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE, hr);
        reason = STC_SERVER_STOP_REASON_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE;
        goto fail2;
    }

    hr = IDXGIKeyedMutex_ReleaseSync(pKeyedMutex, STC_KEY_SERVER);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_INITIALIZE, hr);
        reason = STC_SERVER_STOP_REASON_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_INITIALIZE;
        goto fail2;
    }

    if (pServer->usesLegacyHandles) {
        IDXGIResource* pDxgiResource;
        hr = ID3D11Texture2D_QueryInterface(pTexture, &IID_IDXGIResource, &pDxgiResource);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_QUERY_DXGI_RESOURCE;
            goto fail2;
        }

        hr = IDXGIResource_GetSharedHandle(pDxgiResource, &hTexture);
        IDXGIResource1_Release(pDxgiResource);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_GET_SHARED_HANDLE;
            goto fail2;
        }
    } else {
        IDXGIResource1* pDxgiResource1;
        hr = ID3D11Texture2D_QueryInterface(pTexture, &IID_IDXGIResource1, &pDxgiResource1);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_QUERY_DXGI_RESOURCE1;
            goto fail2;
        }

        hr = IDXGIResource1_CreateSharedHandle(pDxgiResource1, NULL, GENERIC_ALL, NULL, &hTexture);
        IDXGIResource1_Release(pDxgiResource1);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
            goto fail2;
        }

        if (need12) {
            ID3D11Device5* const pDevice11_5 = pServer->pDevice11_5;

            hr = ID3D11Device5_CreateFence(pDevice11_5, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, &pWriteFence11On12);
            if (FAILED(hr)) {
                reason = STC_SERVER_STOP_REASON_FAIL_CREATE_FENCE;
                goto fail3;
            }

            hr = ID3D11Device5_CreateFence(pDevice11_5, 0, D3D11_FENCE_FLAG_SHARED, &IID_ID3D11Fence, &pReadFence11On12);
            if (FAILED(hr)) {
                reason = STC_SERVER_STOP_REASON_FAIL_CREATE_FENCE;
                goto fail4;
            }

            hr = ID3D11Fence_CreateSharedHandle(pWriteFence11On12, NULL, GENERIC_ALL, NULL, &hWriteFence11On12);
            if (FAILED(hr)) {
                reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
                goto fail5;
            }

            hr = ID3D11Fence_CreateSharedHandle(pReadFence11On12, NULL, GENERIC_ALL, NULL, &hReadFence11On12);
            if (FAILED(hr)) {
                reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
                goto fail6;
            }
        }
    }

    pFrame->pTexture = pTexture;
    pFrame->pKeyedMutex = pKeyedMutex;
    pFrame->hTexture = hTexture;
    pFrame->pTexture11On12 = pTexture11On12;
    pFrame->pWriteFence11On12 = pWriteFence11On12;
    pFrame->pReadFence11On12 = pReadFence11On12;
    pFrame->hWriteFence11On12 = hWriteFence11On12;
    pFrame->hReadFence11On12 = hReadFence11On12;
    goto success;

fail6:
    CloseHandle(hWriteFence11On12);
fail5:
    ID3D11Fence_Release(pReadFence11On12);
fail4:
    ID3D11Fence_Release(pWriteFence11On12);
fail3:
    CloseHandle(hTexture);
fail2:
    IDXGIKeyedMutex_Release(pKeyedMutex);
fail1:
    ID3D11Texture2D_Release(pTexture);
    if (!need12) {
        goto fail0;
    }
fail12_0:
    ID3D11Texture2D_Release(pTexture11On12);
fail0:
success:
    return reason;
}

static StcServerStatus CreateD3D12ResourceFrame(const StcServerD3D12* const pServer, bool need11, ResourceFrame12* const pFrame) {
    StcServerStopReason reason = STC_CLIENT_STOP_REASON_NONE;

    ID3D11On12Device* const pDevice11On12 = pServer->pDevice11On12;
    if (need11 && (pDevice11On12 == NULL)) {
        reason = STC_SERVER_STOP_REASON_MISSING_12_TO_11_SUPPORT;
        goto fail0;
    }

    const StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    const StcServerGraphicsInfo* const pGraphicsInfo = &pBase->graphicsInfo;
    StcInfo* const pInfo = pBase->pInfo;

    D3D12_HEAP_PROPERTIES heapProperties;
    heapProperties.Type = D3D12_HEAP_TYPE_DEFAULT;
    heapProperties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heapProperties.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    heapProperties.CreationNodeMask = 0;
    heapProperties.VisibleNodeMask = 0;

    D3D12_RESOURCE_DESC resourceDesc;
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resourceDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
    resourceDesc.Width = pGraphicsInfo->width;
    resourceDesc.Height = pGraphicsInfo->height;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = ConvertFormat(pGraphicsInfo->format, pInfo->srgbChannelType);
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.SampleDesc.Quality = 0;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    resourceDesc.Flags = ComputeD3D12ResourceFlags(pInfo->clientBindFlags);

    D3D11_RESOURCE_FLAGS flags11;
    flags11.BindFlags = ComputeD3D11BindFlags(pInfo->clientBindFlags);
    flags11.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    flags11.CPUAccessFlags = 0;
    flags11.StructureByteStride = 0;

    ID3D12Device* const pDevice = pServer->pDevice;
    ID3D12Resource* pTexture;
    if (need11) {
        const HRESULT hr = ID3D12CompatibilityDevice_CreateSharedResource(
            pServer->pCompatibilityDevice, &heapProperties, D3D12_HEAP_FLAG_SHARED, &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
            NULL, &flags11, D3D12_COMPATIBILITY_SHARED_FLAG_KEYED_MUTEX, NULL, NULL, &IID_ID3D12Resource, &pTexture);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_COMPATIBILITY_TEXTURE;
            goto fail0;
        }
    } else {
        const HRESULT hr = ID3D12Device_CreateCommittedResource(pDevice, &heapProperties, D3D12_HEAP_FLAG_SHARED, &resourceDesc,
                                                                D3D12_RESOURCE_STATE_COMMON, NULL, &IID_ID3D12Resource, &pTexture);
        if (FAILED(hr)) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_TEXTURE_12;
            goto fail0;
        }
    }

    HANDLE hTexture;
    if (FAILED(ID3D12Device_CreateSharedHandle(pDevice, (ID3D12DeviceChild*)pTexture, NULL, GENERIC_ALL, NULL, &hTexture))) {
        reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
        goto fail1;
    }

    ID3D12Fence* pWriteFence;
    if (FAILED(ID3D12Device_CreateFence(pDevice, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, &pWriteFence))) {
        reason = STC_SERVER_STOP_REASON_FAIL_CREATE_FENCE;
        goto fail2;
    }

    ID3D11Texture2D* pTexture11 = NULL;
    IDXGIKeyedMutex* pKeyedMutex11 = NULL;
    ID3D12Fence* pReadFence = NULL;
    HANDLE hWriteFence = NULL;
    HANDLE hReadFence = NULL;
    if (need11) {
        if (FAILED(ID3D11On12Device_CreateWrappedResource(pDevice11On12, (IUnknown*)pTexture, &flags11, D3D12_RESOURCE_STATE_COMMON,
                                                          D3D12_RESOURCE_STATE_COMMON, &IID_ID3D11Texture2D, &pTexture11))) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_WRAPPED_RESOURCE;
            goto fail3;
        }

        ID3D11On12Device_AcquireWrappedResources(pDevice11On12, &(ID3D11Resource*)pTexture11, 1);

        if (FAILED(ID3D11Texture2D_QueryInterface(pTexture11, &IID_IDXGIKeyedMutex, &pKeyedMutex11))) {
            reason = STC_SERVER_STOP_REASON_FAIL_QUERY_KEYED_MUTEX;
            goto fail11_0;
        }

        HRESULT hr = IDXGIKeyedMutex_AcquireSync(pKeyedMutex11, STC_KEY_INITIAL, 0);
        if (FAILED(hr) || (hr == WAIT_ABANDONED) || (hr == WAIT_TIMEOUT)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_INITIALIZE;
            goto fail11_1;
        }

        hr = IDXGIKeyedMutex_ReleaseSync(pKeyedMutex11, STC_KEY_SERVER);
        if (FAILED(hr)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_INITIALIZE, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_INITIALIZE;
            goto fail11_1;
        }
    } else {
        if (FAILED(ID3D12Device_CreateFence(pDevice, 0, D3D12_FENCE_FLAG_SHARED, &IID_ID3D12Fence, &pReadFence))) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_FENCE;
            goto fail3;
        }

        if (FAILED(
                ID3D12Device_CreateSharedHandle(pDevice, (ID3D12DeviceChild*)pWriteFence, NULL, GENERIC_ALL, NULL, &hWriteFence))) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
            goto fail12_0;
        }

        if (FAILED(
                ID3D12Device_CreateSharedHandle(pDevice, (ID3D12DeviceChild*)pReadFence, NULL, GENERIC_ALL, NULL, &hReadFence))) {
            reason = STC_SERVER_STOP_REASON_FAIL_CREATE_SHARED_HANDLE;
            goto fail12_1;
        }
    }

    pFrame->pTexture = pTexture;
    pFrame->pWriteFence = pWriteFence;
    pFrame->pReadFence = pReadFence;
    pFrame->hTexture = hTexture;
    pFrame->hWriteFence = hWriteFence;
    pFrame->hReadFence = hReadFence;
    pFrame->pTexture11 = pTexture11;
    pFrame->pKeyedMutex11 = pKeyedMutex11;
    goto success;

fail12_1:
    CloseHandle(hWriteFence);
fail12_0:
    ID3D12Fence_Release(pReadFence);
    goto fail3;
fail11_1:
    IDXGIKeyedMutex_Release(pKeyedMutex11);
fail11_0:
    ID3D11Texture2D_Release(pTexture11);
fail3:
    ID3D12Fence_Release(pWriteFence);
fail2:
    CloseHandle(hTexture);
fail1:
    ID3D12Resource_Release(pTexture);
fail0:
success:
    return reason;
}

static StcServerStatus TickServer(StcServerBase* const pBase, StcServerStopReason* const pReason) {
    StcServerStatus status = STC_SERVER_STATUS_SUCCESS;

    const int64_t count = StcGetCurrentTicks();

    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    StcGlobalInfo* const pGlobalInfo = pBase->pGlobalInfo;
    StcInfo* pInfo = pBase->pInfo;
    if (pInfo == NULL) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_RECOVER_FROM_OPEN_FAILURE);

        status = OpenServer(pBase, pGlobalInfo);
        pInfo = pBase->pInfo;
    }

    if (pInfo != NULL) {
        StcAtomicInt64Store(&pInfo->serverKeepAlive, count);

        const bool serverInitialized = StcAtomicBoolLoad(&pInfo->serverInitialized);
        if (serverInitialized) {
            const StcClientStopReason clientStopReason = StcAtomicUint32Load(&pInfo->clientStopReason);
            if (clientStopReason != STC_CLIENT_STOP_REASON_NONE) {
                StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CLIENT_REQUEST_STOP,
                              StcGetClientReasonDescription(clientStopReason));
                *pReason = STC_SERVER_STOP_REASON_CLIENT_REQUESTED;
            } else if ((count - StcAtomicInt64Load(&pInfo->clientKeepAlive)) >= StcGetTimeoutTicks()) {
                StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CLIENT_TIMEOUT);
                *pReason = STC_SERVER_STOP_REASON_CLIENT_TIMED_OUT;
            } else {
                status = STC_SERVER_STATUS_SUCCESS;
            }
        } else {
            if (pBase->clientInProgress) {
                if ((count - pBase->clientFirstSeen) >= StcGetTimeoutTicks()) {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CLIENT_TIMEOUT_HANDSHAKE);
                    *pReason = STC_SERVER_STOP_REASON_CLIENT_TIMED_OUT;
                } else if (StcAtomicBoolLoad(&pInfo->clientParametersSpecified)) {
                    StcAtomicBoolStore(&pInfo->serverInitialized, true);

                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CONNECT_HANDSHAKE_COMPLETE);
                    status = STC_SERVER_STATUS_SUCCESS;
                } else {
                    status = STC_SERVER_STATUS_FAIL_CONNECT_IN_PROGRESS;
                }
            } else if (StcAtomicInt64Load(&pGlobalInfo->connectToken) == 0) {
                pBase->clientInProgress = true;
                pBase->clientFirstSeen = count;

                StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_CONNECT_TOKEN_TAKEN);
                status = STC_SERVER_STATUS_FAIL_CONNECT_IN_PROGRESS;
            } else {
                status = STC_SERVER_STATUS_FAIL_DISCONNECTED;
            }
        }

        if (*pReason != STC_SERVER_STOP_REASON_NONE) {
            pBase->clientInProgress = false;

            StcAtomicUint32Store(&pInfo->serverStopReason, *pReason);
        }
    }

    return status;
}

static StcServerStatus StcServerD3D11ConnectionTick(StcServerD3D11* const pServer) {
    StcServerBase* const pBase = &pServer->base;
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;
    StcServerStatus status = TickServer(pBase, &reason);
    if (reason != STC_SERVER_STOP_REASON_NONE) {
        status = ReopenServerD3D11(pServer, reason, pBase->pGlobalInfo);
        if (status == STC_SERVER_STATUS_SUCCESS) {
            status = STC_SERVER_STATUS_FAIL_DISCONNECTED;
        }
    }

    return status;
}

static StcServerStatus StcServerD3D12ConnectionTick(StcServerD3D12* const pServer) {
    StcServerBase* const pBase = &pServer->base;
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;
    StcServerStatus status = TickServer(pBase, &reason);
    if (reason != STC_SERVER_STOP_REASON_NONE) {
        status = ReopenServerD3D12(pServer, reason, pBase->pGlobalInfo);
        if (status == STC_SERVER_STATUS_SUCCESS) {
            status = STC_SERVER_STATUS_FAIL_DISCONNECTED;
        }
    }

    return status;
}

StcServerStatus StcServerD3D11Tick(StcServerD3D11* const pServer, StcServerD3D11NextInfo* const pNextInfo) {
    StcServerStatus status = StcServerD3D11ConnectionTick(pServer);
    if (status == STC_SERVER_STATUS_SUCCESS) {
        status = STC_SERVER_STATUS_FAIL_NO_FRAMES_AVAIALBLE;

        StcServerBase* const pBase = &pServer->base;
        const StcMessageCallbacks* const pMessenger = &pBase->messenger;
        StcInfo* const pInfo = pBase->pInfo;
        if (StcAtomicUint32Load(&pInfo->pendingWrites) > 0) {
            StcAtomicUint32Decrement(&pInfo->pendingWrites);

            StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;
            const size_t copyIndex = (pBase->copyIndex + 1) % STC_TEXTURE_COUNT;

            if (pServer->pTextures[copyIndex] != NULL) {
                HRESULT hr = IDXGIKeyedMutex_AcquireSync(pServer->pKeyedMutexes[copyIndex], STC_KEY_CLIENT, 0);
                if (SUCCEEDED(hr) && (hr != WAIT_ABANDONED) && (hr != WAIT_TIMEOUT)) {
                    hr = IDXGIKeyedMutex_ReleaseSync(pServer->pKeyedMutexes[copyIndex], STC_KEY_SERVER);
                    if (FAILED(hr)) {
                        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_OWN, hr);
                        reason = STC_SERVER_STOP_REASON_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_OWN;
                    }
                } else {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_OWN, hr);
                    reason = STC_SERVER_STOP_REASON_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_OWN;
                }
            }

            if (reason == STC_SERVER_STOP_REASON_NONE) {
                pBase->copyIndex = copyIndex;

                if (pBase->needResize[copyIndex]) {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D11_CREATE_FRAME_ATTEMPT, (int)copyIndex);

                    const bool need12 = pInfo->clientApi == STC_API_D3D12;
                    ResourceFrame11 frame;
                    reason = CreateD3D11ResourceFrame(pServer, need12, &frame);

                    if (reason == STC_CLIENT_STOP_REASON_NONE) {
                        if (pServer->pTextures[copyIndex] != NULL) {
                            pServer->allocator.pfnDestroy(pServer->allocator.pUserData, copyIndex);

                            ID3D11Texture2D_Release(pServer->pTextures[copyIndex]);
                            IDXGIKeyedMutex_Release(pServer->pKeyedMutexes[copyIndex]);
                            if (!pServer->usesLegacyHandles) {
                                CloseHandle((HANDLE)(uintptr_t)pInfo->hTextures[copyIndex]);
                            }

                            if (pServer->pTextures11On12[copyIndex] != NULL) {
                                ID3D11Texture2D_Release(pServer->pTextures11On12[copyIndex]);
                                ID3D11Fence_Release(pServer->pWriteFences11On12[copyIndex]);
                                ID3D11Fence_Release(pServer->pReadFences11On12[copyIndex]);
                                CloseHandle((HANDLE)(uintptr_t)pInfo->hWriteFences12[copyIndex]);
                                CloseHandle((HANDLE)(uintptr_t)pInfo->hReadFences12[copyIndex]);
                            }
                        }

                        pServer->pTextures[copyIndex] = frame.pTexture;
                        pServer->pKeyedMutexes[copyIndex] = frame.pKeyedMutex;
                        pServer->pTextures11On12[copyIndex] = frame.pTexture11On12;
                        pServer->pWriteFences11On12[copyIndex] = frame.pWriteFence11On12;
                        pServer->pReadFences11On12[copyIndex] = frame.pReadFence11On12;
                        pBase->needResize[copyIndex] = false;

                        pInfo->hTextures[copyIndex] = (uint32_t)(uintptr_t)frame.hTexture;
                        pInfo->hWriteFences12[copyIndex] = (uint32_t)(uintptr_t)frame.hWriteFence11On12;
                        pInfo->hReadFences12[copyIndex] = (uint32_t)(uintptr_t)frame.hReadFence11On12;
                        pInfo->writeFenceValues12[copyIndex] = 0;
                        pInfo->readFenceValues12[copyIndex] = 0;
                        pInfo->invalidated[copyIndex] = true;

                        if (pServer->allocator.pfnCreate(pServer->allocator.pUserData, copyIndex, frame.pTexture)) {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D11_CREATE_FRAME_SUCCESS, (int)copyIndex);
                        } else {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_USER_CREATE_FRAME_CALLBACK, (int)copyIndex);
                            reason = STC_SERVER_STOP_REASON_FAIL_D3D11_USER_CREATE_FRAME_CALLBACK;
                        }
                    }
                }
            }

            if (reason == STC_SERVER_STOP_REASON_NONE) {
                pNextInfo->pTexture = pServer->pTextures[copyIndex];
                pNextInfo->index = copyIndex;
                status = STC_SERVER_STATUS_SUCCESS;
            } else {
                ReopenServerD3D11(pServer, reason, pBase->pGlobalInfo);
                status = STC_SERVER_STATUS_FAIL_TICK;
            }
        }
    }

    return status;
}

StcServerStatus StcServerD3D12Tick(StcServerD3D12* const pServer, StcServerD3D12NextInfo* const pNextInfo) {
    StcServerStatus status = StcServerD3D12ConnectionTick(pServer);
    if (status == STC_SERVER_STATUS_SUCCESS) {
        status = STC_SERVER_STATUS_FAIL_NO_FRAMES_AVAIALBLE;

        StcServerBase* const pBase = &pServer->base;
        const StcMessageCallbacks* const pMessenger = &pBase->messenger;
        StcInfo* const pInfo = pBase->pInfo;
        if (StcAtomicUint32Load(&pInfo->pendingWrites) > 0) {
            StcAtomicUint32Decrement(&pInfo->pendingWrites);

            StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;
            const size_t copyIndex = (pBase->copyIndex + 1) % STC_TEXTURE_COUNT;
            const bool need11 = pInfo->clientApi == STC_API_D3D11;

            if (need11 && (pServer->pTextures[copyIndex] != NULL)) {
                HRESULT hr = IDXGIKeyedMutex_AcquireSync(pServer->pKeyedMutexes11[copyIndex], STC_KEY_CLIENT, 0);
                if (SUCCEEDED(hr) && (hr != WAIT_ABANDONED) && (hr != WAIT_TIMEOUT)) {
                    hr = IDXGIKeyedMutex_ReleaseSync(pServer->pKeyedMutexes11[copyIndex], STC_KEY_SERVER);
                    if (SUCCEEDED(hr)) {
                        ID3D11On12Device_ReleaseWrappedResources(pServer->pDevice11On12,
                                                                 &(ID3D11Resource*)pServer->pTextures11[copyIndex], 1);
                    } else {
                        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_OWN, hr);
                        reason = STC_SERVER_STOP_REASON_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_OWN;
                    }
                } else {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_OWN, hr);
                    reason = STC_SERVER_STOP_REASON_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_OWN;
                }
            }

            if (reason == STC_SERVER_STOP_REASON_NONE) {
                pBase->copyIndex = copyIndex;

                if (pBase->needResize[copyIndex]) {
                    StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D12_CREATE_FRAME_ATTEMPT, (int)copyIndex);

                    ResourceFrame12 frame;
                    reason = CreateD3D12ResourceFrame(pServer, need11, &frame);

                    if (reason == STC_CLIENT_STOP_REASON_NONE) {
                        if (pServer->pTextures[copyIndex] != NULL) {
                            ID3D12Fence* const fence = pServer->pWriteFences[copyIndex];
                            const UINT64 fenceValue = pInfo->writeFenceValues12[copyIndex];
                            if (ID3D12Fence_GetCompletedValue(fence) < fenceValue) {
                                const HANDLE hFenceClearedAutoEvent = pServer->hFenceClearedAutoEvent;
                                ID3D12Fence_SetEventOnCompletion(fence, fenceValue, hFenceClearedAutoEvent);
                                WaitForSingleObject(hFenceClearedAutoEvent, INFINITE);
                            }

                            pServer->allocator.pfnDestroy(pServer->allocator.pUserData, copyIndex);

                            ID3D12Resource_Release(pServer->pTextures[copyIndex]);
                            CloseHandle((HANDLE)(uintptr_t)pInfo->hTextures[copyIndex]);

                            ID3D12Fence_Release(pServer->pWriteFences[copyIndex]);

                            if (need11) {
                                ID3D11Texture2D_Release(pServer->pTextures11[copyIndex]);
                                IDXGIKeyedMutex_Release(pServer->pKeyedMutexes11[copyIndex]);
                            } else {
                                ID3D12Fence_Release(pServer->pReadFences[copyIndex]);
                                CloseHandle((HANDLE)(uintptr_t)pInfo->hWriteFences12[copyIndex]);
                                CloseHandle((HANDLE)(uintptr_t)pInfo->hReadFences12[copyIndex]);
                            }
                        }

                        pServer->pTextures[copyIndex] = frame.pTexture;
                        pServer->pWriteFences[copyIndex] = frame.pWriteFence;
                        pServer->pReadFences[copyIndex] = frame.pReadFence;
                        if (need11) {
                            pServer->pTextures11[copyIndex] = frame.pTexture11;
                            pServer->pKeyedMutexes11[copyIndex] = frame.pKeyedMutex11;
                        }

                        pInfo->hTextures[copyIndex] = (uint32_t)(uintptr_t)frame.hTexture;
                        pInfo->hWriteFences12[copyIndex] = (uint32_t)(uintptr_t)frame.hWriteFence;
                        pInfo->hReadFences12[copyIndex] = (uint32_t)(uintptr_t)frame.hReadFence;
                        pInfo->writeFenceValues12[copyIndex] = 0;
                        pInfo->readFenceValues12[copyIndex] = 0;
                        pInfo->invalidated[copyIndex] = true;

                        pBase->needResize[copyIndex] = false;

                        if (pServer->allocator.pfnCreate(pServer->allocator.pUserData, copyIndex, frame.pTexture)) {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_D3D12_CREATE_FRAME_SUCCESS, (int)copyIndex);
                        } else {
                            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_USER_CREATE_FRAME_CALLBACK, (int)copyIndex);
                            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_USER_CREATE_FRAME_CALLBACK;
                        }
                    }
                }
            }

            if (reason == STC_SERVER_STOP_REASON_NONE) {
                pNextInfo->pTexture = pServer->pTextures[copyIndex];
                pNextInfo->index = copyIndex;
                status = STC_SERVER_STATUS_SUCCESS;
            } else {
                ReopenServerD3D12(pServer, reason, pBase->pGlobalInfo);
                status = STC_SERVER_STATUS_FAIL_TICK;
            }
        }
    }

    return status;
}

StcServerStatus StcServerD3D11WaitForClientRead(StcServerD3D11* const pServer) {
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;

    StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo->clientApi == STC_API_D3D12) {
        const size_t copyIndex = pBase->copyIndex;
        const HRESULT hr = ID3D11DeviceContext4_Wait(pServer->pContext11_4, pServer->pReadFences11On12[copyIndex],
                                                     pInfo->readFenceValues12[copyIndex]);
        if (SUCCEEDED(hr)) {
            ID3D11On12Device_AcquireWrappedResources(pServer->pDevice11On12, &(ID3D11Resource*)pServer->pTextures11On12[copyIndex],
                                                     1);
        } else {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_QUEUE_WAIT, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D11_QUEUE_WAIT;
        }
    }

    if (reason == STC_SERVER_STOP_REASON_NONE) {
        const HRESULT hr = IDXGIKeyedMutex_AcquireSync(pServer->pKeyedMutexes[pBase->copyIndex], STC_KEY_SERVER, 0);
        if (FAILED(hr) || (hr == WAIT_ABANDONED) || (hr == WAIT_TIMEOUT)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_WRITE, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D11_ACQUIRE_KEYED_MUTEX_TO_WRITE;
        }
    }

    if (reason != STC_SERVER_STOP_REASON_NONE) {
        ReopenServerD3D11(pServer, reason, pBase->pGlobalInfo);
    }

    return (reason == STC_SERVER_STOP_REASON_NONE) ? STC_SERVER_STATUS_SUCCESS : STC_SERVER_STATUS_FAIL_WAIT_CLIENT_READ;
}

StcServerStatus StcServerD3D12WaitForClientRead(StcServerD3D12* const pServer, ID3D12CommandQueue* const pQueue) {
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;

    StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    StcInfo* const pInfo = pBase->pInfo;

    if (pInfo->clientApi == STC_API_D3D11) {
        const HRESULT hr = IDXGIKeyedMutex_AcquireSync(pServer->pKeyedMutexes11[pBase->copyIndex], STC_KEY_SERVER, 0);
        if (FAILED(hr) || (hr == WAIT_ABANDONED) || (hr == WAIT_TIMEOUT)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_WRITE, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_ACQUIRE_KEYED_MUTEX_TO_WRITE;
        }
    } else {
        const size_t copyIndex = pBase->copyIndex;
        const HRESULT hr = ID3D12CommandQueue_Wait(pQueue, pServer->pReadFences[copyIndex], pInfo->readFenceValues12[copyIndex]);
        if (FAILED(hr)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_QUEUE_WAIT, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_QUEUE_WAIT;
        }
    }

    if (reason != STC_SERVER_STOP_REASON_NONE) {
        ReopenServerD3D12(pServer, reason, pBase->pGlobalInfo);
    }

    return (reason == STC_SERVER_STOP_REASON_NONE) ? STC_SERVER_STATUS_SUCCESS : STC_SERVER_STATUS_FAIL_WAIT_CLIENT_READ;
}

StcServerStatus StcServerD3D11SignalWrite(StcServerD3D11* const pServer) {
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;

    StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    StcInfo* const pInfo = pBase->pInfo;
    const size_t copyIndex = pBase->copyIndex;

    HRESULT hr = IDXGIKeyedMutex_ReleaseSync(pServer->pKeyedMutexes[copyIndex], STC_KEY_CLIENT);
    if (FAILED(hr)) {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_WRITE, hr);
        reason = STC_SERVER_STOP_REASON_FAIL_D3D11_RELEASE_KEYED_MUTEX_TO_WRITE;
    }

    if ((pInfo->clientApi == STC_API_D3D12) && (reason == STC_SERVER_STOP_REASON_NONE)) {
        ID3D11On12Device_ReleaseWrappedResources(pServer->pDevice11On12, &(ID3D11Resource*)pServer->pTextures11On12[copyIndex], 1);

        const UINT64 nextFenceValue = pInfo->writeFenceValues12[copyIndex] + 1;
        hr = ID3D11DeviceContext4_Signal(pServer->pContext11_4, pServer->pWriteFences11On12[copyIndex], nextFenceValue);
        if (SUCCEEDED(hr)) {
            pInfo->writeFenceValues12[copyIndex] = nextFenceValue;
        } else {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D11_QUEUE_SIGNAL, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D11_QUEUE_SIGNAL;
        }
    }

    if (reason == STC_SERVER_STOP_REASON_NONE) {
        StcAtomicUint32Increment(&pInfo->pendingReads);
    } else {
        ReopenServerD3D11(pServer, reason, pBase->pGlobalInfo);
    }

    return (reason == STC_SERVER_STOP_REASON_NONE) ? STC_SERVER_STATUS_SUCCESS : STC_SERVER_STATUS_FAIL_SIGNAL_WRITE;
}

StcServerStatus StcServerD3D12SignalWrite(StcServerD3D12* const pServer, ID3D12CommandQueue* const pQueue) {
    StcServerStopReason reason = STC_SERVER_STOP_REASON_NONE;

    StcServerBase* const pBase = &pServer->base;
    const StcMessageCallbacks* const pMessenger = &pBase->messenger;
    StcInfo* const pInfo = pBase->pInfo;
    const size_t copyIndex = pBase->copyIndex;

    if (pInfo->clientApi == STC_API_D3D11) {
        ID3D11On12Device_AcquireWrappedResources(pServer->pDevice11On12, &(ID3D11Resource*)pServer->pTextures11[copyIndex], 1);

        HRESULT hr = IDXGIKeyedMutex_ReleaseSync(pServer->pKeyedMutexes11[copyIndex], STC_KEY_CLIENT);
        if (FAILED(hr)) {
            StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_WRITE, hr);
            reason = STC_SERVER_STOP_REASON_FAIL_D3D12_RELEASE_KEYED_MUTEX_TO_WRITE;
        }
    }

    const UINT64 nextFenceValue = pInfo->writeFenceValues12[copyIndex] + 1;
    HRESULT hr = ID3D12CommandQueue_Signal(pQueue, pServer->pWriteFences[copyIndex], nextFenceValue);
    if (SUCCEEDED(hr)) {
        pInfo->writeFenceValues12[copyIndex] = nextFenceValue;
    } else {
        StcLogMessage(pMessenger, STC_MESSAGE_ID_SERVER_FAIL_D3D12_QUEUE_SIGNAL, hr);
        reason = STC_SERVER_STOP_REASON_FAIL_D3D12_QUEUE_SIGNAL;
    }

    if (reason == STC_SERVER_STOP_REASON_NONE) {
        StcAtomicUint32Increment(&pInfo->pendingReads);
    } else {
        ReopenServerD3D12(pServer, reason, pBase->pGlobalInfo);
    }

    return (reason == STC_SERVER_STOP_REASON_NONE) ? STC_SERVER_STATUS_SUCCESS : STC_SERVER_STATUS_FAIL_SIGNAL_WRITE;
}
