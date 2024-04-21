// Audio Stream - Windows Audio Session API
// Required libraries:
//	- ole32.lib (COM stuff)

//--------------------------------------------------------------------------------------
// Include
//--------------------------------------------------------------------------------------
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <mmdeviceapi.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <cwchar>

#include <Audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys.h>
#include <MMSystem.h>	// for WAVEFORMATEX

#ifdef _MSC_VER
#define strdup _strdup
#define wcsdup _wcsdup
#endif

#include "../stdtype.h"

#include "AudioStream.h"
#include "../utils/OSThread.h"
#include "../utils/OSSignal.h"
#include "../utils/OSMutex.h"

//--------------------------------------------------------------------------------------
// Define
//--------------------------------------------------------------------------------------
// Uncomment to enable the volume limiter on the master voice.
//#define MASTERING_LIMITER

#define CORE_AUDIO_DEVICE_NAME_COUNT (256)

struct sCoreAudioDeviceInfo {
	int id;
	wchar_t name[CORE_AUDIO_DEVICE_NAME_COUNT];

	sCoreAudioDeviceInfo(void) {
		id = -1;
		name[0] = 0;
	}

	sCoreAudioDeviceInfo(int id, const wchar_t* name) {
		this->id = id;
		wcsncpy_s(this->name, _countof(this->name), name, _TRUNCATE);
	}
};

//--------------------------------------------------------------------------------------
// Forward declaration
//--------------------------------------------------------------------------------------
static HRESULT DeviceNameGet(winrt::com_ptr<::IMMDeviceCollection> dc, UINT id, wchar_t* name, size_t nameBytes);

#if defined(DEBUG) && !defined(NDEBUG)
static void WASAPI_log_ThreadId(void);

#endif

//--------------------------------------------------------------------------------------
// DeviceNameGet help function
//--------------------------------------------------------------------------------------
static HRESULT DeviceNameGet(winrt::com_ptr<::IMMDeviceCollection> dc, UINT id, wchar_t* name, size_t nameBytes)
{
	HRESULT hr = 0;

	winrt::com_ptr<::IMMDevice> pDevice;
	LPWSTR deviceId = nullptr;
	winrt::com_ptr<::IPropertyStore> pPropertyStore;
	PROPVARIANT pv;

	assert(dc);
	assert(name);

	name[0] = 0;

	assert(0 < nameBytes);

	// Initialize container for property value.
	PropVariantInit(&pv);

	hr = dc->Item(id, pDevice.put());
	if (FAILED(hr))
	{
		std::wprintf(L"Failed to create Device: %#lX\n", static_cast<unsigned long>(hr));
		return hr;
	}

	hr = pDevice->GetId(&deviceId);
	if (FAILED(hr))
	{
		std::wprintf(L"Failed to get device ID: %#lX\n", static_cast<unsigned long>(hr));
		return hr;
	}

	hr = pDevice->OpenPropertyStore(STGM_READ, pPropertyStore.put());
	if (FAILED(hr))
	{
		std::wprintf(L"Failed to create PropertyStore: %#lX\n", static_cast<unsigned long>(hr));
		return hr;
	}

	// Get the endpoint's friendly-name property.
	hr = pPropertyStore->GetValue(PKEY_Device_FriendlyName, &pv);
	if (FAILED(hr))
	{
		std::wprintf(L"Failed to GetValue PropertyStore: %#lX\n", static_cast<unsigned long>(hr));
		return hr;
	}

	wcsncpy_s(name, nameBytes / sizeof name[0], pv.pwszVal, _TRUNCATE);

	// GetValue succeeds and returns S_OK if PKEY_Device_FriendlyName is not found.
	// In this case vartName.vt is set to VT_EMPTY.
	//if (pv.vt != VT_EMPTY)
	//{
		// Print audio endpoint device endpoint ID string.
		//std::wprintf(L"device ID %d: audio endpoint device endpoint ID string %s\n", id, deviceId);
	//}

	PropVariantClear(&pv);
	CoTaskMemFree(deviceId);
	return hr;
}

#define EXT_C	extern "C"


typedef struct _wasapi_driver
{
	void* audDrvPtr;
	volatile UINT8 devState;	// 0 - not running, 1 - running, 2 - terminating
	UINT16 dummy;	// [for alignment purposes]

	WAVEFORMATEX waveFmt;
	UINT32 bufSmpls;
	UINT32 bufSize;
	UINT32 bufCount;

	winrt::com_ptr <::IMMDeviceEnumerator> devEnum;
	winrt::com_ptr <::IMMDevice> audDev;
	winrt::com_ptr <::IAudioClient> audClnt;
	winrt::com_ptr <::IAudioClient2> audClnt2; // not used
	winrt::com_ptr <::IAudioClient3> audClnt3; // not used
	winrt::com_ptr <::IAudioRenderClient> rendClnt;

	OS_THREAD* hThread;
	OS_SIGNAL* hSignal;
	OS_MUTEX* hMutex;
	void* userParam;
	AUDFUNC_FILLBUF FillBuffer;

	UINT32 bufFrmCount;
} DRV_WASAPI;


EXT_C UINT8 WASAPI_IsAvailable(void);
EXT_C UINT8 WASAPI_Init(void);
EXT_C UINT8 WASAPI_Deinit(void);
EXT_C const AUDIO_DEV_LIST* WASAPI_GetDeviceList(void);
EXT_C AUDIO_OPTS* WASAPI_GetDefaultOpts(void);

EXT_C UINT8 WASAPI_Create(void** retDrvObj);
EXT_C UINT8 WASAPI_Destroy(void* drvObj);
EXT_C UINT8 WASAPI_Start(void* drvObj, UINT32 deviceID, AUDIO_OPTS* options, void* audDrvParam);
EXT_C UINT8 WASAPI_Stop(void* drvObj);
EXT_C UINT8 WASAPI_Pause(void* drvObj);
EXT_C UINT8 WASAPI_Resume(void* drvObj);

EXT_C UINT8 WASAPI_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam);
EXT_C UINT32 WASAPI_GetBufferSize(void* drvObj);
static UINT32 GetFreeSamples(DRV_WASAPI* drv);
EXT_C UINT8 WASAPI_IsBusy(void* drvObj);
EXT_C UINT8 WASAPI_WriteData(void* drvObj, UINT32 dataSize, void* data);

EXT_C UINT32 WASAPI_GetLatency(void* drvObj);
static void WasapiThread(void* Arg);

extern "C"
{
	AUDIO_DRV audDrv_WASAPI =
	{
		{ADRVTYPE_OUT, ADRVSIG_WASAPI, "WASAPI"},

		WASAPI_IsAvailable,
		WASAPI_Init, WASAPI_Deinit,
		WASAPI_GetDeviceList, WASAPI_GetDefaultOpts,

		WASAPI_Create, WASAPI_Destroy,
		WASAPI_Start, WASAPI_Stop,
		WASAPI_Pause, WASAPI_Resume,

		WASAPI_SetCallback, WASAPI_GetBufferSize,
		WASAPI_IsBusy, WASAPI_WriteData,

		WASAPI_GetLatency,
	};
}	// extern "C"

static AUDIO_OPTS defOptions;
static AUDIO_DEV_LIST deviceList;

static UINT8 isInit = 0;
static UINT32 activeDrivers;

UINT8 WASAPI_IsAvailable(void)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	HRESULT retVal;
	UINT8 resVal = 1u;// UINT8 resVal;

	resVal = 0;
	retVal = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(retVal)) {
		std::wprintf(L"Failed to init COM: %#lX\n", static_cast<unsigned long>(retVal));
		return 0;
	}

	//
	// Create a deviceEnumerator
	//
	winrt::com_ptr<::IMMDeviceEnumerator> devEnum;

	retVal = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devEnum.put()));
	if (SUCCEEDED(retVal)) {
		resVal = 1;
		devEnum.~com_ptr();	devEnum = nullptr;
	}

	CoUninitialize();
	return resVal;
}

UINT8 WASAPI_Init(void)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	HRESULT retVal;

	//
	// Create an audio device
	//
	winrt::com_ptr<::IMMDevice> audDev;

	//
	// Create a property storre
	//
	winrt::com_ptr<::IPropertyStore> propSt;

	//
	// Create an audio client
	//
	winrt::com_ptr<::IAudioClient> audClnt;

	if (isInit) {
		return AERR_WASDONE;
	}

	deviceList.devCount = 0;
	deviceList.devNames = nullptr;

	retVal = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (FAILED(retVal)) {
		std::wprintf(L"Failed to init COM: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	//
	// Create a deviceEnumerator
	//
	winrt::com_ptr<::IMMDeviceEnumerator> devEnum;

	retVal = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(devEnum.put()));
	if (FAILED(retVal)) {
		std::wprintf(L"Failed to init deviceEnumerator: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	//
	// Create a deviceCollection
	//
	winrt::com_ptr<::IMMDeviceCollection> devList;

	retVal = devEnum->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, devList.put());
	if (FAILED(retVal)) {
		std::wprintf(L"Failed to init deviceCollection: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	UINT nDevices = 0u;
	retVal = devList->GetCount(&nDevices);
	if (FAILED(retVal))
	{
		std::wprintf(L"Failed to deviceCollection::Getcount method: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	std::vector<sCoreAudioDeviceInfo> deviceInfo;

	printf("Device list: %u %s\n", nDevices, (0u != nDevices) ? "entries" : "entry");
	deviceList.devNames = (char**)malloc(sizeof(char**) * nDevices);
	UINT i = 0u;
	for (i = 0u; i < nDevices; ++i) {
		wchar_t widechar_device_name[CORE_AUDIO_DEVICE_NAME_COUNT];
		std::memset(widechar_device_name, 0, sizeof widechar_device_name);
		retVal = DeviceNameGet(devList, i, widechar_device_name, sizeof widechar_device_name);
		deviceInfo.push_back(sCoreAudioDeviceInfo(i, widechar_device_name));

		char char_device_name[CORE_AUDIO_DEVICE_NAME_COUNT];
		std::memset(char_device_name, 0, sizeof char_device_name);
		WideCharToMultiByte(CP_ACP, 0, widechar_device_name, -1, char_device_name, sizeof char_device_name - 1, nullptr, nullptr);
		printf("    device ID %d: %s\n", i, char_device_name);

		size_t name_length = strlen(char_device_name) + (size_t)1u;
		deviceList.devNames[i] = (char*)malloc(name_length);
		std::memcpy(deviceList.devNames[i], char_device_name, name_length);
	}
	printf("\n");

	deviceList.devCount = nDevices;

	audDev.~com_ptr();	audDev = nullptr;

	memset(&defOptions, 0x00, sizeof(AUDIO_OPTS));
	defOptions.sampleRate = 44100;
	defOptions.numChannels = 2;
	defOptions.numBitsPerSmpl = 16;
	defOptions.usecPerBuf = 10000;	// 10 ms per buffer
	defOptions.numBuffers = 10;	// 100 ms latency

	WAVEFORMATEX* mixFmt = nullptr;
	WAVEFORMATEXTENSIBLE* mixFmtX = nullptr;

	retVal = devEnum->GetDefaultAudioEndpoint(eRender, eConsole, audDev.put());
	if (retVal == S_OK)
	{
		retVal = audDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, audClnt.put_void());
		if (retVal == S_OK)
		{
			retVal = audClnt->GetMixFormat(&mixFmt);
			if (retVal == S_OK)
			{
				defOptions.sampleRate = static_cast<UINT32>(mixFmt->nSamplesPerSec);
				defOptions.numChannels = static_cast<UINT8>(mixFmt->nChannels);
				if (mixFmt->wFormatTag == WAVE_FORMAT_PCM)
				{
					defOptions.numBitsPerSmpl = static_cast<UINT8>(mixFmt->wBitsPerSample);
				}
				else if (mixFmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
				{
					mixFmtX = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFmt);
#if defined(DEBUG) && !defined(NDEBUG)
					std::wprintf(L"WASAPI shared mix format:\n");
					std::wprintf(
						L"  wFormatTag=0x%x\n  nChannels=%d\n"
						L"  nSamplesPerSec=%d\n"
						L"  nAvgBytesPerSec=%d\n"
						L"  nBlockAlign=%d\n"
						L"  wBitsPerSample=%d\n"
						L"  cbSize=%d\n"
						L"  Samples.wValidBitsPerSample=%d\n",
						mixFmtX->Format.wFormatTag,
						mixFmtX->Format.nChannels,
						mixFmtX->Format.nSamplesPerSec,
						mixFmtX->Format.nAvgBytesPerSec,
						mixFmtX->Format.nBlockAlign,
						mixFmtX->Format.wBitsPerSample,
						mixFmtX->Format.cbSize,
						mixFmtX->Samples.wValidBitsPerSample);

#endif

					if (mixFmtX->SubFormat == KSDATAFORMAT_SUBTYPE_PCM)
						defOptions.numBitsPerSmpl = static_cast<UINT8>(mixFmt->wBitsPerSample);
				}
				CoTaskMemFree(mixFmt);	mixFmt = nullptr;	mixFmtX = nullptr;
			}
			audClnt.~com_ptr();	audClnt = nullptr;
		}
		audDev.~com_ptr();	audDev = nullptr;
	}
	devEnum.~com_ptr();	devEnum = nullptr;

	activeDrivers = 0;
	isInit = 1;

	CoUninitialize();
	return AERR_OK;
}

UINT8 WASAPI_Deinit(void)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	UINT32 curDev;

	if (!isInit)
		return AERR_WASDONE;

	for (curDev = 0; curDev < deviceList.devCount; curDev ++)
	{
		free(deviceList.devNames[curDev]);
	}
	deviceList.devCount = 0;
	free(deviceList.devNames);	deviceList.devNames = nullptr;

	CoUninitialize();
	isInit = 0;

	return AERR_OK;
}

const AUDIO_DEV_LIST* WASAPI_GetDeviceList(void)
{
	return &deviceList;
}

AUDIO_OPTS* WASAPI_GetDefaultOpts(void)
{
	return &defOptions;
}

UINT8 WASAPI_Create(void** retDrvObj)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	DRV_WASAPI* drv = new (DRV_WASAPI);

	if (nullptr != drv) {
		drv->devState = 0;
		*(drv->devEnum.put_void()) = nullptr;
		*(drv->audDev.put_void()) = nullptr;
		*(drv->audClnt.put_void()) = nullptr;
		*(drv->audClnt2.put_void()) = nullptr;
		*(drv->audClnt3.put_void()) = nullptr;
		*(drv->rendClnt.put_void()) = nullptr;
		drv->hThread = nullptr;
		drv->hSignal = nullptr;
		drv->hMutex = nullptr;
		drv->userParam = nullptr;
		drv->FillBuffer = nullptr;

		activeDrivers ++;
		UINT8 retVal8 = OSSignal_Init(&drv->hSignal, 0);
		retVal8 |= OSMutex_Init(&drv->hMutex, 0);
		if (retVal8)
		{
			WASAPI_Destroy(drv);
			*retDrvObj = nullptr;
			return AERR_API_ERR;
		}
		*retDrvObj = std::move(drv);

		return AERR_OK;
	} else {
		return AERR_INVALID_DRV;
	}
}

UINT8 WASAPI_Destroy(void* drvObj)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	DRV_WASAPI* drv = (DRV_WASAPI*)drvObj;

	if (drv->devState != 0)
		WASAPI_Stop(drvObj);
	if (drv->hThread != nullptr)
	{
		OSThread_Cancel(drv->hThread);
		OSThread_Deinit(drv->hThread);
	}
	if (drv->hSignal != nullptr)
		OSSignal_Deinit(drv->hSignal);
	if (drv->hMutex != nullptr)
		OSMutex_Deinit(drv->hMutex);

	delete drv;
	activeDrivers --;

	return AERR_OK;
}

UINT8 WASAPI_Start(void* drvObj, UINT32 deviceID, AUDIO_OPTS* options, void* audDrvParam)
{
#if defined(DEBUG) && !defined(NDEBUG)
	WASAPI_log_ThreadId();

#endif

	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	UINT64 tempInt64;
	REFERENCE_TIME bufTime;
	UINT8 errVal;
	HRESULT retVal;
	UINT8 retVal8;
#ifdef NDEBUG
	HANDLE hWinThr;
	BOOL retValB;
#endif

	if (drv->devState != 0) {
		return 0xD0;	// already running
	}
	if (deviceID >= deviceList.devCount) {
		return AERR_INVALID_DEV;
	}

	drv->audDrvPtr = audDrvParam;
	if (options == nullptr) {
		options = &defOptions;
	}
	drv->waveFmt.wFormatTag = WAVE_FORMAT_PCM;
	drv->waveFmt.nChannels = options->numChannels;
	drv->waveFmt.nSamplesPerSec = options->sampleRate;
	drv->waveFmt.wBitsPerSample = options->numBitsPerSmpl;
	drv->waveFmt.nBlockAlign = drv->waveFmt.wBitsPerSample * drv->waveFmt.nChannels / 8;
	drv->waveFmt.nAvgBytesPerSec = drv->waveFmt.nSamplesPerSec * drv->waveFmt.nBlockAlign;
	drv->waveFmt.cbSize = 0;

	tempInt64 = (UINT64)options->sampleRate * options->usecPerBuf;
	drv->bufSmpls = (UINT32)((tempInt64 + 500000) / 1000000);
	drv->bufSize = drv->waveFmt.nBlockAlign * drv->bufSmpls;
	drv->bufCount = options->numBuffers ? options->numBuffers : 10;
	// REFERENCE_TIME uses 100-ns steps
	bufTime = (REFERENCE_TIME)10000000 * drv->bufSmpls * drv->bufCount;
	bufTime = (bufTime + options->sampleRate / 2) / options->sampleRate;

	errVal = AERR_API_ERR;
	retVal = CoInitializeEx(nullptr, COINIT_MULTITHREADED);	// call again, in case Init() was called by another thread
	if (FAILED(retVal)) {
		std::wprintf(L"Failed to init COM: %#lX\n", static_cast<unsigned long>(retVal));
		goto StartErr_DevEnum;
	}

	retVal = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(drv->devEnum.put()));
	if (FAILED(retVal)) {
		goto StartErr_DevEnum;
	}

	retVal = drv->devEnum->GetDefaultAudioEndpoint(eRender, eConsole, drv->audDev.put());
	if (retVal != S_OK) {
		std::wprintf(L"IMMDeviceEnumerator::GetDefaultAudioEndpoint method failed: %#lX\n", static_cast<unsigned long>(retVal));
		goto StartErr_DevEnum;
	}

	retVal = drv->audDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, drv->audClnt.put_void());
	if (FAILED(retVal)) {
		goto StartErr_HasDev;
	}

	retVal = drv->audClnt->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufTime, 0, &drv->waveFmt, nullptr);
	if (FAILED(retVal)) {
		goto StartErr_HasAudClient;
	}

	retVal = drv->audClnt->GetBufferSize(&drv->bufFrmCount);
	if (FAILED(retVal)) {
		goto StartErr_HasAudClient;
	}

	retVal = drv->audClnt->GetService(__uuidof(IAudioRenderClient), drv->rendClnt.put_void());
	if (FAILED(retVal)) {
		goto StartErr_HasAudClient;
	}

	OSSignal_Reset(drv->hSignal);
	retVal8 = OSThread_Init(&drv->hThread, &WasapiThread, reinterpret_cast<void*>(drv));
	if (retVal8)
	{
		errVal = 0xC8;	// CreateThread failed
		goto StartErr_HasRendClient;
	}

#ifdef NDEBUG
	hWinThr = *(HANDLE*)OSThread_GetHandle(drv->hThread);
	retValB = SetThreadPriority(hWinThr, THREAD_PRIORITY_TIME_CRITICAL);
	if (!retValB)
	{
		// Error setting priority
		// Try a lower priority, because too low priorities cause sound stuttering.
		retValB = SetThreadPriority(hWinThr, THREAD_PRIORITY_HIGHEST);
	}
#endif

	retVal = drv->audClnt->Start();

	drv->devState = 1;
	OSSignal_Signal(drv->hSignal);

	return AERR_OK;

StartErr_HasRendClient:
	drv->rendClnt.~com_ptr();	drv->rendClnt = nullptr;
StartErr_HasAudClient:
	drv->audClnt.~com_ptr();	drv->audClnt = nullptr;
StartErr_HasDev:
	drv->audDev.~com_ptr();	drv->audDev = nullptr;
StartErr_DevEnum:
	drv->devEnum.~com_ptr();	drv->devEnum = nullptr;
	CoUninitialize();
	return errVal;
}

UINT8 WASAPI_Stop(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	HRESULT retVal;

	if (drv->devState != 1)
		return 0xD8;	// is already stopped (or stopping)

	drv->devState = 2;
	if (drv->audClnt != nullptr)
		retVal = drv->audClnt->Stop();

	OSThread_Join(drv->hThread);
	OSThread_Deinit(drv->hThread);	drv->hThread = nullptr;

	drv->rendClnt.~com_ptr();	drv->rendClnt = nullptr;
	drv->audClnt.~com_ptr();	drv->audClnt = nullptr;
	drv->audDev.~com_ptr();	drv->audDev = nullptr;
	drv->devEnum.~com_ptr();	drv->devEnum = nullptr;

	CoUninitialize();
	drv->devState = 0;

	return AERR_OK;
}

UINT8 WASAPI_Pause(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	HRESULT retVal;

	if (drv->devState != 1) {
		return 0xFF;
	}

	retVal = drv->audClnt->Stop();
	return (retVal == S_OK || retVal == S_FALSE) ? AERR_OK : 0xFF;
}

UINT8 WASAPI_Resume(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	HRESULT retVal;

	if (drv->devState != 1) {
		return 0xFF;
	}

	retVal = drv->audClnt->Start();
	return (retVal == S_OK || retVal == S_FALSE) ? AERR_OK : 0xFF;
}


UINT8 WASAPI_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);

	OSMutex_Lock(drv->hMutex);
	drv->userParam = userParam;
	drv->FillBuffer = FillBufCallback;
	OSMutex_Unlock(drv->hMutex);

	return AERR_OK;
}

UINT32 WASAPI_GetBufferSize(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);

	return drv->bufSize;
}

static UINT32 GetFreeSamples(DRV_WASAPI* drv)
{
	HRESULT retVal;
	UINT paddSmpls;

	retVal = drv->audClnt->GetCurrentPadding(&paddSmpls);
	if (retVal != S_OK) {
		return 0;
	}

	return drv->bufFrmCount - paddSmpls;
}

UINT8 WASAPI_IsBusy(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	UINT32 freeSmpls;

	if (drv->FillBuffer != nullptr) {
		return AERR_BAD_MODE;
	}

	freeSmpls = GetFreeSamples(drv);
	return (freeSmpls >= drv->bufSmpls) ? AERR_OK : AERR_BUSY;
}

UINT8 WASAPI_WriteData(void* drvObj, UINT32 dataSize, void* data)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	UINT32 dataSmpls;
	HRESULT retVal;
	BYTE* bufData;

	if (dataSize > drv->bufSize) {
		return AERR_TOO_MUCH_DATA;
	}

	dataSmpls = dataSize / drv->waveFmt.nBlockAlign;
	while(GetFreeSamples(drv) < dataSmpls) {
		Sleep(1);
	}

	retVal = drv->rendClnt->GetBuffer(dataSmpls, &bufData);
	if (retVal != S_OK) {
		return AERR_API_ERR;
	}

	memcpy(bufData, data, dataSize);

	retVal = drv->rendClnt->ReleaseBuffer(dataSmpls, 0x00);

	return AERR_OK;
}


UINT32 WASAPI_GetLatency(void* drvObj)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(drvObj);
	REFERENCE_TIME latencyTime;
	HRESULT retVal;

	retVal = drv->audClnt->GetStreamLatency(&latencyTime);
	if (retVal != S_OK) {
		return 0;
	}

	return (UINT32)((latencyTime + 5000) / 10000);	// 100 ns steps -> 1 ms steps
}

static void WasapiThread(void* Arg)
{
	DRV_WASAPI* drv = reinterpret_cast<DRV_WASAPI*>(Arg);
	UINT32 didBuffers;	// number of processed buffers
	UINT32 wrtBytes;
	UINT32 wrtSmpls;
	HRESULT retVal;
	BYTE* bufData;

	OSSignal_Wait(drv->hSignal);	// wait until the initialization is done

	while(drv->devState == 1)
	{
		didBuffers = 0;

		OSMutex_Lock(drv->hMutex);
		while(GetFreeSamples(drv) >= drv->bufSmpls && drv->FillBuffer != nullptr)
		{
			retVal = drv->rendClnt->GetBuffer(drv->bufSmpls, &bufData);
			if (retVal == S_OK)
			{
				wrtBytes = drv->FillBuffer(drv->audDrvPtr, drv->userParam, drv->bufSize, (void*)bufData);
				wrtSmpls = wrtBytes / drv->waveFmt.nBlockAlign;

				retVal = drv->rendClnt->ReleaseBuffer(wrtSmpls, 0x00);
				didBuffers ++;
			}
		}
		OSMutex_Unlock(drv->hMutex);
		if (!didBuffers) {
			Sleep(1);
		}

		while(drv->FillBuffer == nullptr && drv->devState == 1) {
			Sleep(1);
		}

		//while(drv->PauseThread && drv->devState == 1) {
		//	Sleep(1);
		//}
	}
}

#if defined(DEBUG) && !defined(NDEBUG)
static void WASAPI_log_ThreadId(void)
{
	DWORD thrd_id = GetCurrentThreadId();
	std::wprintf(L"Current ThreadId: %#lX\n", thrd_id);
}

#endif
