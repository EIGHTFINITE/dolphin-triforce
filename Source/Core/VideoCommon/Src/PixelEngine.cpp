// Copyright (C) 2003 Dolphin Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official SVN repository and contact information can be found at
// http://code.google.com/p/dolphin-emu/


// http://developer.nvidia.com/object/General_FAQ.html#t6 !!!!!



#include "Common.h"
#include "VideoCommon.h"
#include "ChunkFile.h"
#include "Atomic.h"

#include "PixelEngine.h"
#include "CommandProcessor.h"

namespace PixelEngine
{

union UPEZConfReg
{
	u16 Hex;
	struct 
	{
		unsigned ZCompEnable	: 1; // Z Comparator Enable
		unsigned Function		: 3;
		unsigned ZUpdEnable		: 1;
		unsigned				: 11;
	};
};

union UPEAlphaConfReg
{
	u16 Hex;
	struct 
	{
		unsigned BMMath			: 1; // GX_BM_BLEND || GX_BM_SUBSTRACT
		unsigned BMLogic		: 1; // GX_BM_LOGIC
		unsigned Dither			: 1;
		unsigned ColorUpdEnable	: 1;
		unsigned AlphaUpdEnable	: 1;
		unsigned DstFactor		: 3;
		unsigned SrcFactor		: 3;
		unsigned Substract		: 1; // Additive mode by default
		unsigned BlendOperator	: 4;
	};
};

union UPEDstAlphaConfReg
{
	u16 Hex;
	struct 
	{
		unsigned DstAlpha		: 8;
		unsigned Enable			: 1;
		unsigned				: 7;
	};
};

union UPEAlphaModeConfReg
{
	u16 Hex;
	struct 
	{
		unsigned Threshold		: 8;
		unsigned CompareMode	: 8;
	};
};

// Not sure about this reg...
union UPEAlphaReadReg
{
	u16 Hex;
	struct 
	{
		unsigned ReadMode		: 3;
		unsigned				: 13;
	};
};

// fifo Control Register
union UPECtrlReg
{
	struct 
	{
		unsigned PETokenEnable	:	1;
		unsigned PEFinishEnable	:	1;
		unsigned PEToken		:	1; // write only
		unsigned PEFinish		:	1; // write only
		unsigned				:	12;
	};
	u16 Hex;
	UPECtrlReg() {Hex = 0; }
	UPECtrlReg(u16 _hex) {Hex = _hex; }
};

// STATE_TO_SAVE
static UPEZConfReg			m_ZConf;
static UPEAlphaConfReg		m_AlphaConf;
static UPEDstAlphaConfReg	m_DstAlphaConf;
static UPEAlphaModeConfReg	m_AlphaModeConf;
static UPEAlphaReadReg		m_AlphaRead;
static UPECtrlReg			m_Control;
//static u16					m_Token; // token value most recently encountered

static bool g_bSignalTokenInterrupt;
static bool g_bSignalFinishInterrupt;

static int et_SetTokenOnMainThread;
static int et_SetFinishOnMainThread;

u16 bbox[4];
bool bbox_active;

enum
{
    INT_CAUSE_PE_TOKEN    =  0x200, // GP Token
    INT_CAUSE_PE_FINISH   =  0x400, // GP Finished
};

void DoState(PointerWrap &p)
{
	p.Do(m_ZConf);
	p.Do(m_AlphaConf);
	p.Do(m_DstAlphaConf);
	p.Do(m_AlphaModeConf);
	p.Do(m_AlphaRead);
	p.Do(m_Control);
	p.Do(CommandProcessor::fifo.PEToken);

	p.Do(g_bSignalTokenInterrupt);
	p.Do(g_bSignalFinishInterrupt);
	
	p.Do(bbox);
	p.Do(bbox_active);
}

void UpdateInterrupts();

void SetToken_OnMainThread(u64 userdata, int cyclesLate);
void SetFinish_OnMainThread(u64 userdata, int cyclesLate);

void Init()
{
	m_Control.Hex = 0;

	et_SetTokenOnMainThread = g_VideoInitialize.pRegisterEvent("SetToken", SetToken_OnMainThread);
	et_SetFinishOnMainThread = g_VideoInitialize.pRegisterEvent("SetFinish", SetFinish_OnMainThread);

	bbox[0] = 0x80;
	bbox[1] = 0xA0;
	bbox[2] = 0x80;
	bbox[3] = 0xA0;

	bbox_active = false;
}

void Read16(u16& _uReturnValue, const u32 _iAddress)
{
	DEBUG_LOG(PIXELENGINE, "(r16) 0x%08x", _iAddress);

	switch (_iAddress & 0xFFF)
	{
		// CPU Direct Access EFB Raster State Config
	case PE_ZCONF:
		_uReturnValue = m_ZConf.Hex;
		INFO_LOG(PIXELENGINE, "(r16) ZCONF");
		break;
	case PE_ALPHACONF:
		// Most games read this early. no idea why.
		_uReturnValue = m_AlphaConf.Hex;
		INFO_LOG(PIXELENGINE, "(r16) ALPHACONF");
		break;
	case PE_DSTALPHACONF:
		_uReturnValue = m_DstAlphaConf.Hex;
		INFO_LOG(PIXELENGINE, "(r16) DSTALPHACONF");
		break;
	case PE_ALPHAMODE:
		_uReturnValue = m_AlphaModeConf.Hex;
		INFO_LOG(PIXELENGINE, "(r16) ALPHAMODE");
		break;	
	case PE_ALPHAREAD:
		_uReturnValue = m_AlphaRead.Hex;
		WARN_LOG(PIXELENGINE, "(r16) ALPHAREAD");
		break;

	case PE_CTRL_REGISTER:
		_uReturnValue = m_Control.Hex;
		INFO_LOG(PIXELENGINE, "(r16) CTRL_REGISTER : %04x", _uReturnValue);
		break;

	case PE_TOKEN_REG:
		_uReturnValue = CommandProcessor::fifo.PEToken;
		INFO_LOG(PIXELENGINE, "(r16) TOKEN_REG : %04x", _uReturnValue);
		break;

		// The return values for these BBOX registers need to be gotten from the bounding box of the object. 
		// See http://code.google.com/p/dolphin-emu/issues/detail?id=360#c74 for more details.

	// 0x80, 0xa0, 0x80, 0xa0 makes Paper Mario happy.
	case PE_BBOX_LEFT:   _uReturnValue = bbox[0]; INFO_LOG(PIXELENGINE, "R: BBOX_LEFT   = %i", bbox[0]); bbox_active = false; break;
	case PE_BBOX_RIGHT:  _uReturnValue = bbox[1]; INFO_LOG(PIXELENGINE, "R: BBOX_RIGHT  = %i", bbox[1]); bbox_active = false; break;
	case PE_BBOX_TOP:    _uReturnValue = bbox[2]; INFO_LOG(PIXELENGINE, "R: BBOX_TOP    = %i", bbox[2]); bbox_active = false; break;
	case PE_BBOX_BOTTOM: _uReturnValue = bbox[3]; INFO_LOG(PIXELENGINE, "R: BBOX_BOTTOM = %i", bbox[3]); bbox_active = false; break;

	case PE_PERF_0L:
	case PE_PERF_0H:
	case PE_PERF_1L:
	case PE_PERF_1H:
	case PE_PERF_2L:
	case PE_PERF_2H:
	case PE_PERF_3L:
	case PE_PERF_3H:
	case PE_PERF_4L:
	case PE_PERF_4H:
	case PE_PERF_5L:
	case PE_PERF_5H:
		INFO_LOG(PIXELENGINE, "(r16) perf counter @ %08x", _iAddress);
		break;

	default:
		INFO_LOG(PIXELENGINE, "(r16) unknown @ %08x", _iAddress);
		_uReturnValue = 1;
		break;
	}
}

void Write16(const u16 _iValue, const u32 _iAddress)
{
	switch (_iAddress & 0xFFF)
	{
		// CPU Direct Access EFB Raster State Config
	case PE_ZCONF:
		m_ZConf.Hex = _iValue;
		INFO_LOG(PIXELENGINE, "(w16) ZCONF: %02x", _iValue);
		break;
	case PE_ALPHACONF:
		m_AlphaConf.Hex = _iValue;
		INFO_LOG(PIXELENGINE, "(w16) ALPHACONF: %02x", _iValue);
		break;
	case PE_DSTALPHACONF:
		m_DstAlphaConf.Hex = _iValue;
		INFO_LOG(PIXELENGINE, "(w16) DSTALPHACONF: %02x", _iValue);
		break;
	case PE_ALPHAMODE:
		m_AlphaModeConf.Hex = _iValue;
		INFO_LOG(PIXELENGINE, "(w16) ALPHAMODE: %02x", _iValue);
		break;
	case PE_ALPHAREAD:
		m_AlphaRead.Hex = _iValue;
		INFO_LOG(PIXELENGINE, "(w16) ALPHAREAD: %02x", _iValue);
		break;

	case PE_CTRL_REGISTER:	
		{
			UPECtrlReg tmpCtrl(_iValue);

			if (tmpCtrl.PEToken)	g_bSignalTokenInterrupt = false;
			if (tmpCtrl.PEFinish)	g_bSignalFinishInterrupt = false;

			m_Control.PETokenEnable  = tmpCtrl.PETokenEnable;
			m_Control.PEFinishEnable = tmpCtrl.PEFinishEnable;
			m_Control.PEToken = 0;		// this flag is write only
			m_Control.PEFinish = 0;		// this flag is write only

			DEBUG_LOG(PIXELENGINE, "(w16) CTRL_REGISTER: 0x%04x", _iValue);
			UpdateInterrupts();
		}
		break;

	case PE_TOKEN_REG:
		//LOG(PIXELENGINE,"WEIRD: program wrote token: %i",_iValue);
		PanicAlert("(w16) WTF? PowerPC program wrote token: %i", _iValue);
		//only the gx pipeline is supposed to be able to write here
		//g_token = _iValue;
		break;

	default:
		WARN_LOG(PIXELENGINE, "(w16) unknown %04x @ %08x", _iValue, _iAddress);
		break;
	}
}

void Write32(const u32 _iValue, const u32 _iAddress)
{
	WARN_LOG(PIXELENGINE, "(w32) 0x%08x @ 0x%08x IGNORING...",_iValue,_iAddress);
}

bool AllowIdleSkipping()
{
	return !g_VideoInitialize.bUseDualCore|| (!m_Control.PETokenEnable && !m_Control.PEFinishEnable);
}

void UpdateInterrupts()
{
	// check if there is a token-interrupt
	if (g_bSignalTokenInterrupt	& m_Control.PETokenEnable)
		g_VideoInitialize.pSetInterrupt(INT_CAUSE_PE_TOKEN, true);
	else
		g_VideoInitialize.pSetInterrupt(INT_CAUSE_PE_TOKEN, false);

	// check if there is a finish-interrupt
	if (g_bSignalFinishInterrupt & m_Control.PEFinishEnable)
		g_VideoInitialize.pSetInterrupt(INT_CAUSE_PE_FINISH, true);
	else
		g_VideoInitialize.pSetInterrupt(INT_CAUSE_PE_FINISH, false);
}

// TODO(mb2): Refactor SetTokenINT_OnMainThread(u64 userdata, int cyclesLate).
//			  Think about the right order between tokenVal and tokenINT... one day maybe.
//			  Cleanup++

// Called only if BPMEM_PE_TOKEN_INT_ID is ack by GP
void SetToken_OnMainThread(u64 userdata, int cyclesLate)
{
	//if (userdata >> 16)
	//{
		g_bSignalTokenInterrupt = true;	
		//_dbg_assert_msg_(PIXELENGINE, (CommandProcessor::fifo.PEToken == (userdata&0xFFFF)), "WTF? BPMEM_PE_TOKEN_INT_ID's token != BPMEM_PE_TOKEN_ID's token" );
		INFO_LOG(PIXELENGINE, "VIDEO Plugin raises INT_CAUSE_PE_TOKEN (btw, token: %04x)", CommandProcessor::fifo.PEToken);
		UpdateInterrupts();
	//}
	//else
	//	LOGV(PIXELENGINE, 1, "VIDEO Plugin wrote token: %i", CommandProcessor::fifo.PEToken);
}

void SetFinish_OnMainThread(u64 userdata, int cyclesLate)
{
	g_bSignalFinishInterrupt = 1;	
	UpdateInterrupts();
}

// SetToken
// THIS IS EXECUTED FROM VIDEO THREAD
void SetToken(const u16 _token, const int _bSetTokenAcknowledge)
{
	// TODO?: set-token-value and set-token-INT could be merged since set-token-INT own the token value.
	if (_bSetTokenAcknowledge) // set token INT
	{
		// This seems smelly...
		CommandProcessor::IncrementGPWDToken(); // for DC watchdog hack since PEToken seems to be a frame-finish too
		g_VideoInitialize.pScheduleEvent_Threadsafe(
			0, et_SetTokenOnMainThread, _token | (_bSetTokenAcknowledge << 16));
	}
	else // set token value
	{
		// we do it directly from videoThread because of
		// Super Monkey Ball
		// XXX: No 16-bit atomic store available, so cheat and use 32-bit.
		// That's what we've always done. We're counting on fifo.PEToken to be
		// 4-byte padded.
        Common::AtomicStore(*(volatile u32*)&CommandProcessor::fifo.PEToken, _token);
	}
}

// SetFinish
// THIS IS EXECUTED FROM VIDEO THREAD (BPStructs.cpp) when a new frame has been drawn
void SetFinish()
{
	CommandProcessor::IncrementGPWDToken(); // for DC watchdog hack
	g_VideoInitialize.pScheduleEvent_Threadsafe(
		0, et_SetFinishOnMainThread, 0);
	INFO_LOG(PIXELENGINE, "VIDEO Set Finish");
}

} // end of namespace PixelEngine