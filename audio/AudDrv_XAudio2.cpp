// Audio Stream - XAudio2
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

#include <assert.h>
#include <functiondiscoverykeys.h>
#include <strsafe.h>

#ifdef __GNUC__
// suppress warnings about 'uuid' attribute directive and MSVC pragmas
#pragma GCC diagnostic ignored "-Wattributes"
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif

#include <xaudio2.h>

#include <xaudio2fx.h>
#include <x3daudio.h>
#include <xapofx.h>
#pragma comment(lib,"xaudio2.lib")

#include <mmsystem.h>	// for WAVEFORMATEX

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
		//std::wprintf(L"deviceId=%d: audio endpoint device endpoint ID string %s\n", id, deviceId);
	//}

	PropVariantClear(&pv);
	CoTaskMemFree(deviceId);
	return hr;
}

#define EXT_C	extern "C"


typedef struct _xaudio2_driver
{
	void* audDrvPtr;
	volatile UINT8 devState;	// 0 - not running, 1 - running, 2 - terminating
	UINT16 dummy;	// [for alignment purposes]

	WAVEFORMATEX waveFmt;
	UINT32 bufSmpls;
	UINT32 bufSize;
	UINT32 bufCount;
	UINT8* bufSpace;

	winrt::com_ptr<::IXAudio2> pXAudio2;
	IXAudio2MasteringVoice* xaMstVoice;
	IXAudio2SourceVoice* xaSrcVoice;

	XAUDIO2_BUFFER* xaBufs;

	OS_THREAD* hThread;
	OS_SIGNAL* hSignal;
	OS_MUTEX* hMutex;
	void* userParam;
	AUDFUNC_FILLBUF FillBuffer;

	UINT32 writeBuf;
} DRV_XAUD2;


EXT_C UINT8 XAudio2_IsAvailable(void);
EXT_C UINT8 XAudio2_Init(void);
EXT_C UINT8 XAudio2_Deinit(void);
EXT_C const AUDIO_DEV_LIST* XAudio2_GetDeviceList(void);
EXT_C AUDIO_OPTS* XAudio2_GetDefaultOpts(void);

EXT_C UINT8 XAudio2_Create(void** retDrvObj);
EXT_C UINT8 XAudio2_Destroy(void* drvObj);
EXT_C UINT8 XAudio2_Start(void* drvObj, UINT32 deviceID, AUDIO_OPTS* options, void* audDrvParam);
EXT_C UINT8 XAudio2_Stop(void* drvObj);
EXT_C UINT8 XAudio2_Pause(void* drvObj);
EXT_C UINT8 XAudio2_Resume(void* drvObj);

EXT_C UINT8 XAudio2_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam);
EXT_C UINT32 XAudio2_GetBufferSize(void* drvObj);
EXT_C UINT8 XAudio2_IsBusy(void* drvObj);
EXT_C UINT8 XAudio2_WriteData(void* drvObj, UINT32 dataSize, void* data);

EXT_C UINT32 XAudio2_GetLatency(void* drvObj);
static void XAudio2Thread(void* Arg);


extern "C"
{
	AUDIO_DRV audDrv_XAudio2 =
	{
		{ADRVTYPE_OUT, ADRVSIG_XAUD2, "XAudio2"},

		XAudio2_IsAvailable,
		XAudio2_Init, XAudio2_Deinit,
		XAudio2_GetDeviceList, XAudio2_GetDefaultOpts,

		XAudio2_Create, XAudio2_Destroy,
		XAudio2_Start, XAudio2_Stop,
		XAudio2_Pause, XAudio2_Resume,

		XAudio2_SetCallback, XAudio2_GetBufferSize,
		XAudio2_IsBusy, XAudio2_WriteData,

		XAudio2_GetLatency,
	};
}	// extern "C"

static AUDIO_OPTS defOptions;
static AUDIO_DEV_LIST deviceList;

static UINT8 isInit = 0;
static UINT32 activeDrivers;

UINT8 XAudio2_IsAvailable(void)
{
	return 1;
}

UINT8 XAudio2_Init(void)
{
	HRESULT retVal;

	UINT32 flags = 0u;
#if defined(_DEBUG)
	flags |= XAUDIO2_DEBUG_ENGINE;
#endif

	if (isInit) {
		return AERR_WASDONE;
	}

	deviceList.devCount = 0;
	deviceList.devNames = nullptr;
	
	retVal = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	if (!(retVal == S_OK || retVal == S_FALSE)) {
		std::wprintf(L"Failed to init COM: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	//
	// Create a XAudio2
	//
	winrt::com_ptr<::IXAudio2> pXAudio2;

	retVal = XAudio2Create(pXAudio2.put(), flags);
	if (retVal != S_OK)
	{
		CoUninitialize();
		return AERR_API_ERR;
	}

#if defined(_DEBUG)
	// To see the trace output, you need to view ETW logs for this application:
	//    Go to Control Panel, Administrative Tools, Event Viewer.
	//    View->Show Analytic and Debug Logs.
	//    Applications and Services Logs / Microsoft / Windows / XAudio2. 
	//    Right click on Microsoft Windows XAudio2 debug logging, Properties, then Enable Logging, and hit OK 
	XAUDIO2_DEBUG_CONFIGURATION debug = {};
	debug.TraceMask = XAUDIO2_LOG_ERRORS | XAUDIO2_LOG_WARNINGS;
	debug.BreakMask = XAUDIO2_LOG_ERRORS;
	pXAudio2->SetDebugConfiguration(&debug, 0);
#endif

	//
	// Create a deviceEnumerator
	//
	winrt::com_ptr<IMMDeviceEnumerator> pDeviceEnumerator;

	retVal = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(pDeviceEnumerator.put()));
	if (FAILED(retVal))
	{
		std::wprintf(L"Failed to init deviceEnumerator: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	//
	// Create a deviceCollection
	//
	winrt::com_ptr<::IMMDeviceCollection> pDeviceCollection;

	retVal = pDeviceEnumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, pDeviceCollection.put());
	if (FAILED(retVal))
	{
		std::wprintf(L"Failed to init deviceCollection: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	UINT nDevices = 0u;
	retVal = pDeviceCollection->GetCount(&nDevices);
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
		retVal = DeviceNameGet(pDeviceCollection, i, widechar_device_name, sizeof widechar_device_name);
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

	pXAudio2.~com_ptr();	pXAudio2 = nullptr;

	CoUninitialize();

	memset(&defOptions, 0x00, sizeof(AUDIO_OPTS));
	defOptions.sampleRate = 44100;
	defOptions.numChannels = 2;
	defOptions.numBitsPerSmpl = 16;
	defOptions.usecPerBuf = 10000;	// 10 ms per buffer
	defOptions.numBuffers = 10;	// 100 ms latency

	activeDrivers = 0;
	isInit = 1;

	CoUninitialize();
	return AERR_OK;
}

UINT8 XAudio2_Deinit(void)
{
	UINT32 curDev;

	if (! isInit) {
		return AERR_WASDONE;
	}

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

const AUDIO_DEV_LIST* XAudio2_GetDeviceList(void)
{
	return &deviceList;
}

AUDIO_OPTS* XAudio2_GetDefaultOpts(void)
{
	return &defOptions;
}

UINT8 XAudio2_Create(void** retDrvObj)
{
	DRV_XAUD2* drv = new (DRV_XAUD2);

	if (nullptr != drv) {
		UINT8 retVal8;

		drv->devState = 0u;
		*(drv->pXAudio2.put_void()) = nullptr;
		drv->xaMstVoice = nullptr;
		drv->xaSrcVoice = nullptr;
		drv->xaBufs = nullptr;
		drv->hThread = nullptr;
		drv->hSignal = nullptr;
		drv->hMutex = nullptr;
		drv->userParam = nullptr;
		drv->FillBuffer = nullptr;

		activeDrivers ++;
		retVal8  = OSSignal_Init(&drv->hSignal, 0);
		retVal8 |= OSMutex_Init(&drv->hMutex, 0);
		if (retVal8)
		{
			XAudio2_Destroy(drv);
			*retDrvObj = nullptr;
			return AERR_API_ERR;
		}
		*retDrvObj = std::move(drv);

		return AERR_OK;
	} else {
		return AERR_API_ERR;
	}
}

UINT8 XAudio2_Destroy(void* drvObj)
{
	DRV_XAUD2* drv = (DRV_XAUD2*)drvObj;

	if (drv->devState != 0) {
		XAudio2_Stop(drvObj);
	}
	if (drv->hThread != nullptr)
	{
		OSThread_Cancel(drv->hThread);
		OSThread_Deinit(drv->hThread);
	}
	if (drv->hSignal != nullptr) {
		OSSignal_Deinit(drv->hSignal);
	}
	if (drv->hMutex != nullptr) {
		OSMutex_Deinit(drv->hMutex);
	}
	free(drv);
	activeDrivers --;

	return AERR_OK;
}

UINT8 XAudio2_Start(void* drvObj, UINT32 deviceID, AUDIO_OPTS* options, void* audDrvParam)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);
	UINT64 tempInt64;
	UINT32 curBuf;
	XAUDIO2_BUFFER* tempXABuf;
	HRESULT retVal;
	UINT8 retVal8;
#ifdef NDEBUG
	HANDLE hWinThr;
	BOOL retValB;
#endif
	UINT32 flags = 0u;
#if defined(_DEBUG)
	flags |= XAUDIO2_DEBUG_ENGINE;
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

	retVal = CoInitializeEx(nullptr, COINIT_MULTITHREADED);	// call again, in case Init() was called by another thread
	if (!(retVal == S_OK || retVal == S_FALSE)) {
		std::wprintf(L"Failed to init COM: %#lX\n", static_cast<unsigned long>(retVal));
		return AERR_API_ERR;
	}

	//
	// Create a XAudio2
	//
	retVal = XAudio2Create(drv->pXAudio2.put(), flags);
	if (retVal != S_OK) {
		return AERR_API_ERR;
	}

	//drv->pXAudio2.copy_from(pXAudio2.get());
	retVal = drv->pXAudio2->CreateMasteringVoice(&drv->xaMstVoice, \
		XAUDIO2_DEFAULT_CHANNELS, drv->waveFmt.nSamplesPerSec, 0x00, \
		nullptr, nullptr);
	if (retVal != S_OK) {
		return AERR_API_ERR;
	}

	retVal = drv->pXAudio2->CreateSourceVoice(&drv->xaSrcVoice, \
		&drv->waveFmt, 0x00, XAUDIO2_DEFAULT_FREQ_RATIO, nullptr, nullptr, nullptr);
	if (retVal != S_OK) {
		return AERR_API_ERR;
	}

	OSSignal_Reset(drv->hSignal);
	retVal8 = OSThread_Init(&drv->hThread, &XAudio2Thread, drv);
	if (retVal8) {
		return 0xC8;	// CreateThread failed
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

	drv->xaBufs = (XAUDIO2_BUFFER*)calloc(drv->bufCount, sizeof(XAUDIO2_BUFFER));
	drv->bufSpace = (UINT8*)malloc(drv->bufCount * drv->bufSize);
	if ((nullptr != drv->xaBufs) && (nullptr != drv->bufSpace)) {
		for (curBuf = 0; curBuf < drv->bufCount; curBuf++)
		{
			tempXABuf = &drv->xaBufs[curBuf];
			tempXABuf->AudioBytes = drv->bufSize;
			tempXABuf->pAudioData = &drv->bufSpace[drv->bufSize * curBuf];
			tempXABuf->pContext = nullptr;
			tempXABuf->Flags = 0x00;
		}
		drv->writeBuf = 0;
	} else {
		return AERR_BAD_MODE;
	}

	retVal = drv->xaSrcVoice->Start(0x00, XAUDIO2_COMMIT_NOW);
	if (retVal != S_OK) {
		return AERR_API_ERR;
	}

	drv->devState = 1;
	OSSignal_Signal(drv->hSignal);

	return AERR_OK;
}

UINT8 XAudio2_Stop(void* drvObj)
{
	DRV_XAUD2* drv = (DRV_XAUD2*)drvObj;
	HRESULT retVal;

	if (drv->devState != 1) {
		return 0xD8;	// is already stopped (or stopping)
	}

	drv->devState = 2;
	if (drv->xaSrcVoice != nullptr) {
		retVal = drv->xaSrcVoice->Stop(0x00, XAUDIO2_COMMIT_NOW);
	}

	OSThread_Join(drv->hThread);
	OSThread_Deinit(drv->hThread);	drv->hThread = nullptr;

	drv->xaMstVoice->DestroyVoice();	/*drv->xaMstVoice.~com_ptr();*/	drv->xaMstVoice = nullptr;
	drv->xaSrcVoice->DestroyVoice();	/*drv->xaSrcVoice.~com_ptr();*/	drv->xaSrcVoice = nullptr;
	drv->pXAudio2.~com_ptr();			drv->pXAudio2 = nullptr;

	free(drv->xaBufs);		drv->xaBufs = nullptr;
	free(drv->bufSpace);	drv->bufSpace = nullptr;

	CoUninitialize();
	drv->devState = 0;

	return AERR_OK;
}

UINT8 XAudio2_Pause(void* drvObj)
{
	DRV_XAUD2* drv = (DRV_XAUD2*)drvObj;
	HRESULT retVal;

	if (drv->devState != 1) {
		return 0xFF;
	}

	retVal = drv->xaSrcVoice->Stop(0x00, XAUDIO2_COMMIT_NOW);
	return (retVal == S_OK) ? AERR_OK : 0xFF;
}

UINT8 XAudio2_Resume(void* drvObj)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);
	HRESULT retVal;

	if (drv->devState != 1) {
		return 0xFF;
	}

	retVal = drv->xaSrcVoice->Start(0x00, XAUDIO2_COMMIT_NOW);
	return (retVal == S_OK) ? AERR_OK : 0xFF;
}


UINT8 XAudio2_SetCallback(void* drvObj, AUDFUNC_FILLBUF FillBufCallback, void* userParam)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);

	OSMutex_Lock(drv->hMutex);
	drv->userParam = userParam;
	drv->FillBuffer = FillBufCallback;
	OSMutex_Unlock(drv->hMutex);

	return AERR_OK;
}

UINT32 XAudio2_GetBufferSize(void* drvObj)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);

	return drv->bufSize;
}

UINT8 XAudio2_IsBusy(void* drvObj)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);
	XAUDIO2_VOICE_STATE xaVocState;

	if (drv->FillBuffer != nullptr) {
		return AERR_BAD_MODE;
	}

	drv->xaSrcVoice->GetState(&xaVocState);
	return (xaVocState.BuffersQueued < drv->bufCount) ? AERR_OK : AERR_BUSY;
}

UINT8 XAudio2_WriteData(void* drvObj, UINT32 dataSize, void* data)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);
	XAUDIO2_BUFFER* tempXABuf;
	XAUDIO2_VOICE_STATE xaVocState;
	HRESULT retVal;

	if (dataSize > drv->bufSize) {
		return AERR_TOO_MUCH_DATA;
	}

	drv->xaSrcVoice->GetState(&xaVocState);
	while(xaVocState.BuffersQueued >= drv->bufCount)
	{
		Sleep(1);
		drv->xaSrcVoice->GetState(&xaVocState);
	}

	tempXABuf = &drv->xaBufs[drv->writeBuf];
	memcpy((void*)tempXABuf->pAudioData, data, dataSize);
	tempXABuf->AudioBytes = dataSize;

	retVal = drv->xaSrcVoice->SubmitSourceBuffer(tempXABuf, nullptr);
	drv->writeBuf ++;
	if (drv->writeBuf >= drv->bufCount) {
		drv->writeBuf -= drv->bufCount;
	}

	return AERR_OK;
}


UINT32 XAudio2_GetLatency(void* drvObj)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(drvObj);
	XAUDIO2_VOICE_STATE xaVocState;
	UINT32 bufBehind;
	UINT32 bytesBehind;
	UINT32 curBuf;

	drv->xaSrcVoice->GetState(&xaVocState);

	bufBehind = xaVocState.BuffersQueued;
	curBuf = drv->writeBuf;
	bytesBehind = 0;
	while(bufBehind > 0)
	{
		if (curBuf == 0) {
			curBuf = drv->bufCount;
		}
		curBuf --;
		bytesBehind += drv->xaBufs[curBuf].AudioBytes;
		bufBehind --;
	}
	return bytesBehind * 1000 / drv->waveFmt.nAvgBytesPerSec;
}

static void XAudio2Thread(void* Arg)
{
	DRV_XAUD2* drv = reinterpret_cast<DRV_XAUD2*>(Arg);
	XAUDIO2_VOICE_STATE xaVocState;
	XAUDIO2_BUFFER* tempXABuf;
	UINT32 didBuffers;	// number of processed buffers

	OSSignal_Wait(drv->hSignal);	// wait until the initialization is done

	while(drv->devState == 1)
	{
		didBuffers = 0;

		OSMutex_Lock(drv->hMutex);
		drv->xaSrcVoice->GetState(&xaVocState);
		while(xaVocState.BuffersQueued < drv->bufCount && drv->FillBuffer != nullptr)
		{
			tempXABuf = &drv->xaBufs[drv->writeBuf];
			tempXABuf->AudioBytes = drv->FillBuffer(drv->audDrvPtr, drv->userParam,
										drv->bufSize, (void*)tempXABuf->pAudioData);

			HRESULT retVal = drv->xaSrcVoice->SubmitSourceBuffer(tempXABuf, nullptr);
			drv->writeBuf ++;
			if (drv->writeBuf >= drv->bufCount)
				drv->writeBuf -= drv->bufCount;
			didBuffers ++;

			drv->xaSrcVoice->GetState(&xaVocState);
		}
		OSMutex_Unlock(drv->hMutex);
		if (! didBuffers) {
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
