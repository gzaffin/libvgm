#ifndef __EMUSTRUCTS_H__
#define __EMUSTRUCTS_H__

#ifdef __cplusplus
extern "C"
{
#endif

#include <stdtype.h>
#include "snddef.h"

typedef struct _device_definition DEV_DEF;
typedef struct _device_info DEV_INFO;
typedef struct _device_generic_config DEV_GEN_CFG;


typedef void (*DEVCB_SRATE_CHG)(void* info, UINT32 newSRate);

typedef UINT8 (*DEVFUNC_START)(const DEV_GEN_CFG* cfg, DEV_INFO* retDevInf);
typedef void (*DEVFUNC_CTRL)(void* info);
typedef void (*DEVFUNC_UPDATE)(void* info, UINT32 samples, DEV_SMPL** outputs);
typedef void (*DEVFUNC_OPTMASK)(void* info, UINT32 optionBits);
typedef void (*DEVFUNC_PANALL)(void* info, INT16* channelPanVal);
typedef void (*DEVFUNC_SRCCB)(void* info, DEVCB_SRATE_CHG smpRateChgCallback, void* paramPtr);

typedef UINT8 (*DEVFUNC_READ_A8D8)(void* info, UINT8 addr);
typedef UINT16 (*DEVFUNC_READ_A8D16)(void* info, UINT8 addr);

typedef void (*DEVFUNC_WRITE_A8D8)(void* info, UINT8 addr, UINT8 data);
typedef void (*DEVFUNC_WRITE_A8D16)(void* info, UINT8 addr, UINT16 data);
typedef void (*DEVFUNC_WRITE_A16D8)(void* info, UINT16 addr, UINT8 data);
typedef void (*DEVFUNC_WRITE_A16D16)(void* info, UINT16 addr, UINT16 data);
typedef void (*DEVFUNC_WRITE_MEMSIZE)(void* info, UINT32 memsize);
typedef void (*DEVFUNC_WRITE_BLOCK)(void* info, UINT32 offset, UINT32 length, const UINT8* data);

#define RWF_WRITE		0x00
#define RWF_READ		0x01
#define RWF_QUICKWRITE	(0x02 | RWF_WRITE)
#define RWF_QUICKREAD	(0x02 | RWF_READ)
#define RWF_REGISTER	0x00	// register r/w
#define RWF_MEMORY		0x10	// memory (RAM) r/w

#define DEVRW_A8D8		0x11	//  8-bit address,  8-bit data
#define DEVRW_A8D16		0x12	//  8-bit address, 16-bit data
#define DEVRW_A16D8		0x21	// 16-bit address,  8-bit data
#define DEVRW_A16D16	0x22	// 16-bit address, 16-bit data
#define DEVRW_BLOCK		0x80	// write sample ROM/RAM
#define DEVRW_MEMSIZE	0x81	// set ROM/RAM size

typedef struct _devdef_readwrite_function
{
	UINT8 funcType;	// function type, see RWF_ constants
	UINT8 rwType;	// read/write function type, see DEVRW_ constants
	UINT16 user;	// user-defined value
	void* funcPtr;
} DEVDEF_RWFUNC;

// generic device data structure
// MUST be the first variable included in all device-specifc structures
typedef struct _device_data
{
	void* chipInf;	// pointer to CHIP_INF (depends on specific chip)
} DEV_DATA;
struct _device_definition
{
	const char* name;	// name of the device
	const char* author;	// author/origin of emulation
	UINT32 coreID;		// 4-character identifier ID to distinguish between
						// multiple emulators of a device
	
	DEVFUNC_START Start;
	DEVFUNC_CTRL Stop;
	DEVFUNC_CTRL Reset;
	DEVFUNC_UPDATE Update;
	
	DEVFUNC_OPTMASK SetOptionBits;
	DEVFUNC_OPTMASK SetMuteMask;
	DEVFUNC_PANALL SetPanning;
	DEVFUNC_SRCCB SetSRateChgCB;	// used to set callback function for realtime sample rate changes
	
	UINT32 rwFuncCount;
	const DEVDEF_RWFUNC* rwFuncs;
};	// DEV_DEF
struct _device_info
{
	DEV_DATA* dataPtr;	// points to chip data structure
	UINT32 sampleRate;
	const DEV_DEF* devDef;
};	// DEV_INFO


#define DEVRI_SRMODE_NATIVE		0x00
#define DEVRI_SRMODE_CUSTOM		0x01
#define DEVRI_SRMODE_HIGHEST	0x02
struct _device_generic_config
{
	UINT32 emuCore;		// emulation core (4-character code, 0 = default)
	UINT8 srMode;		// sample rate mode
	
	// TODO: add UINT8 flags (to replace bit 31 of clock)
	UINT32 clock;		// chip clock
	UINT32 smplRate;	// sample rate for SRMODE_CUSTOM/DEVRI_SRMODE_HIGHEST
						// Note: Some cores ignore the srMode setting and always use smplRate.
};	// DEV_GEN_CFG


#ifdef __cplusplus
}
#endif

#endif	// __EMUSTRUCTS_H__