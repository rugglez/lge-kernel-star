/*
 * Copyright (c) 2007-2009 NVIDIA Corporation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * Neither the name of the NVIDIA Corporation nor the names of its contributors
 * may be used to endorse or promote products derived from this software
 * without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "mach/nvrm_linux.h" // for s_hRmGlobal
#include "nvodm_keyboard.h"
#include "nvodm_query_discovery.h"
#include "nvodm_services.h"
#include "nvodm_query_gpio.h"
#include "nvrm_gpio.h"
#include "nvec.h"

// Module debug: 0=disable, 1=enable
#define NVODM_ENABLE_PRINTF      0

#if NVODM_ENABLE_PRINTF
#define NVODM_PRINTF(x) NvOdmOsDebugPrintf x
#else
#define NVODM_PRINTF(x)
#endif

// wake from keyboard disabled for now
#define WAKE_FROM_KEYBOARD	1

/* command main category */
#define EC_KBC_COMMAND 0x5

/* number of LEDS on the keyboard */
enum {NUM_OF_LEDS = 3};

/* Keyboard specific sub-commands */
#define KBD_RESET_COMMAND 0xFF

/* Special Scan Code set 1 codes */
#define SC1_LSHIFT (0x2A)
#define SC1_RSHIFT (0x36)
#define SC1_SCROLL (0x46)
#define SC1_PREFIX_E0 (0xE0)
#define SC1_PREFIX_E1 (0xE1)

/* Scan Code Set 1 break mask */
#define SC1_BREAK_MASK (0x80)

static NvEcHandle s_NvEcHandle = NULL;  // nvec handle
NvEcEventType EventTypes[] = {NvEcEventType_Keyboard};  // get only keyboard events from EC
NvEcEvent KbdEvent = {0};
static NvOdmOsSemaphoreHandle s_hKbcKeyScanRecvSema = NULL;
static NvEcEventRegistrationHandle s_hEcEventRegistration = NULL;
static NvBool s_KeyboardDeinit = NV_FALSE;

#if WAKE_FROM_KEYBOARD
extern NvRmGpioHandle s_hGpioGlobal;
#define DEBOUNCE_TIME_MS	5 /* GPIO debounce time in ms */
typedef struct NvOdmKbdContextRec
{
	const NvOdmGpioPinInfo *GpioPinInfo;
	NvRmGpioPinHandle hPin;
	NvRmGpioInterruptHandle GpioIntrHandle;
	NvU32 PinCount;
} NvOdmKbdContext;
NvOdmKbdContext *hOdm;

static void GpioInterruptHandler(void *args)
{
	NvOdmKbdContext *Odm = (NvOdmKbdContext *)args;

	if (Odm)
	{
		NvRmGpioInterruptDone(Odm->GpioIntrHandle);
	}
}

#endif

// Shadow LED state
NvU8 s_LedState = 0;

NvBool NvOdmKeyboardInit(void)
{
    NvError NvStatus = NvError_Success;
    NvEcRequest Request = {0};
    NvEcResponse Response = {0};

    /* get nvec handle */
    NvStatus = NvEcOpen(&s_NvEcHandle, 0 /* instance */);
    if (NvStatus != NvError_Success)
    {
        goto fail;
    }

    /* reset the EC to start the keyboard scanning */
    Request.PacketType = NvEcPacketType_Request;
    Request.RequestType = NvEcRequestResponseType_Keyboard;
    Request.RequestSubtype = (NvEcRequestResponseSubtype) NvEcKeyboardSubtype_Enable;
    Request.NumPayloadBytes = 0;

    NvStatus = NvEcSendRequest(s_NvEcHandle, &Request, &Response, sizeof(Request), sizeof(Response));
    if (NvStatus != NvError_Success)
    {
        goto cleanup;
    }

    /* check if command passed */
    if (Response.Status != NvEcStatus_Success)
    {
        goto cleanup;
    }

#if WAKE_FROM_KEYBOARD
	hOdm = NvOdmOsAlloc(sizeof(NvOdmKbdContext));
	if (!hOdm) {
		goto cleanup;
	}

	/* Check the supported GPIOs */
	hOdm->GpioPinInfo = NvOdmQueryGpioPinMap(NvOdmGpioPinGroup_WakeFromECKeyboard,
					0,
					&hOdm->PinCount);

	NvRmGpioAcquirePinHandle(s_hGpioGlobal,
		hOdm->GpioPinInfo->Port,
		hOdm->GpioPinInfo->Pin,
		&hOdm->hPin);
	if (!hOdm->hPin) {
		goto cleanup;
	}

	/* register to receive GPIO events */
	NvStatus = NvRmGpioInterruptRegister(s_hGpioGlobal,
		s_hRmGlobal,
		hOdm->hPin,
		(NvOsInterruptHandler)GpioInterruptHandler,
		NvRmGpioPinMode_InputData,
		hOdm,
		&hOdm->GpioIntrHandle,
		DEBOUNCE_TIME_MS);
	if (NvStatus != NvError_Success) {
		goto cleanup;
	}

	NvStatus = NvRmGpioInterruptEnable(hOdm->GpioIntrHandle);
	if (NvStatus != NvError_Success) {
		goto cleanup;
	}

	/* enable keyboard as wake up source */
	Request.PacketType = NvEcPacketType_Request;
	Request.RequestType = NvEcRequestResponseType_Keyboard;
	Request.RequestSubtype = (NvEcRequestResponseSubtype)
	NvEcKeyboardSubtype_ConfigureWake;
	Request.NumPayloadBytes = 2;
	Request.Payload[0] = NVEC_KEYBOARD_WAKE_ENABLE_0_ACTION_ENABLE;
	Request.Payload[1] = NVEC_KEYBOARD_EVENT_TYPE_0_ANY_KEY_PRESS_ENABLE;

	NvStatus = NvEcSendRequest(s_NvEcHandle,
		&Request,
		&Response,
		sizeof(Request),
		sizeof(Response));
	if (NvStatus != NvError_Success) {
		goto cleanup;
        }

	if (Response.Status != NvEcStatus_Success) {
		goto cleanup;
	}

        /* enable key reporting on wake up */
	Request.PacketType = NvEcPacketType_Request;
	Request.RequestType = NvEcRequestResponseType_Keyboard;
	Request.RequestSubtype = (NvEcRequestResponseSubtype)
	NvEcKeyboardSubtype_ConfigureWakeKeyReport;
	Request.NumPayloadBytes = 1;
	Request.Payload[0] = NVEC_KEYBOARD_REPORT_WAKE_KEY_0_ACTION_ENABLE;

	NvStatus = NvEcSendRequest(s_NvEcHandle,
		&Request,
		&Response,
		sizeof(Request),
		sizeof(Response));
	if (NvStatus != NvError_Success) {
		goto cleanup;
        }

	if (Response.Status != NvEcStatus_Success) {
		goto cleanup;
	}
#endif

    /* create semaphore which can be used to send scan codes to the clients */
    s_hKbcKeyScanRecvSema = NvOdmOsSemaphoreCreate(0);
    if (!s_hKbcKeyScanRecvSema)
    {
        goto cleanup;
    }

    /* register for keyboard events */
    NvStatus = NvEcRegisterForEvents(
                    s_NvEcHandle,       // nvec handle
                    &s_hEcEventRegistration,
                    (NvOsSemaphoreHandle)s_hKbcKeyScanRecvSema,
                    sizeof(EventTypes)/sizeof(NvEcEventType),
                    EventTypes, // receive keyboard scan codes
                    1,          // currently buffer only 1 packet from ECI at a time
                    sizeof(NvEcEvent));
    if (NvStatus != NvError_Success)
    {
        goto cleanup;
    }

    /* success */
    return NV_TRUE;

cleanup:
#if WAKE_FROM_KEYBOARD
	NvRmGpioInterruptUnregister(s_hGpioGlobal, s_hRmGlobal, hOdm->GpioIntrHandle);
	hOdm->GpioIntrHandle = NULL;
	NvRmGpioReleasePinHandles(s_hGpioGlobal, &hOdm->hPin, hOdm->PinCount);
	NvOdmOsFree(hOdm);
	hOdm = NULL;
#endif
    (void)NvEcUnregisterForEvents(s_hEcEventRegistration);
    s_hEcEventRegistration = NULL;

    NvOdmOsSemaphoreDestroy(s_hKbcKeyScanRecvSema);
    s_hKbcKeyScanRecvSema = NULL;

    NvEcClose(s_NvEcHandle);
fail:
    s_NvEcHandle = NULL;

    return NV_FALSE;
}

void NvOdmKeyboardDeInit(void)
{
#if WAKE_FROM_KEYBOARD
	NvRmGpioInterruptUnregister(s_hGpioGlobal, s_hRmGlobal, hOdm->GpioIntrHandle);
	hOdm->GpioIntrHandle = NULL;
	NvRmGpioReleasePinHandles(s_hGpioGlobal, &hOdm->hPin, hOdm->PinCount);
	hOdm->PinCount = 0;
	NvOdmOsFree(hOdm);
	hOdm = NULL;
#endif

    (void)NvEcUnregisterForEvents(s_hEcEventRegistration);
    s_hEcEventRegistration = NULL;

    s_KeyboardDeinit = NV_TRUE;
    NvOdmOsSemaphoreSignal(s_hKbcKeyScanRecvSema);
    NvOdmOsSemaphoreDestroy(s_hKbcKeyScanRecvSema);
    s_hKbcKeyScanRecvSema = NULL;

    NvEcClose(s_NvEcHandle);
    s_NvEcHandle = NULL;
}

/* Gets the actual scan code for a key press */
NvBool NvOdmKeyboardGetKeyData(NvU32 *pKeyScanCode, NvU8 *pScanCodeFlags, NvU32 Timeout)
{
    NvError NvStatus = NvError_Success;
    NvU32 OutCode, OutCodeBytes, i;
    NvU8 ScanCodeFlags;

    if (!pKeyScanCode || !pScanCodeFlags || s_KeyboardDeinit)
    {
        return NV_FALSE;
    }

    if (Timeout != 0)
    {
        /* Use the timeout value */
        if (!NvOdmOsSemaphoreWaitTimeout(s_hKbcKeyScanRecvSema, Timeout))
            return NV_FALSE; // timed out
    }
    else
    {
        /* wait till we receive a scan code from the EC */
        NvOdmOsSemaphoreWait(s_hKbcKeyScanRecvSema);
    }

    // stop scanning
    if (s_KeyboardDeinit)
        return NV_FALSE;

    if (s_hEcEventRegistration)
    {
        NvStatus = NvEcGetEvent(s_hEcEventRegistration, &KbdEvent, sizeof(NvEcEvent));
        if (NvStatus != NvError_Success)
        {
            NV_ASSERT(!"Could not receive scan code");
            return NV_FALSE;
        }
        if (KbdEvent.NumPayloadBytes == 0)
        {
            NV_ASSERT(!"Received keyboard event with no scan codes");
            return NV_FALSE;
        }

        // Pack scan code bytes from payload buffer into 32-bit dword
        OutCode = (NvU32)KbdEvent.Payload[0];
        OutCodeBytes = 1;
        ScanCodeFlags = 0;

        if (KbdEvent.NumPayloadBytes == 1)
            NVODM_PRINTF(("EC Payload = 0x%x", KbdEvent.Payload[0]));
        else
        {
            for (i = 0; i < KbdEvent.NumPayloadBytes; i++)
                NVODM_PRINTF(("EC Payload = 0x%x", KbdEvent.Payload[i]));
        }

        for (i = 1; i < KbdEvent.NumPayloadBytes; i++)
        {
            if (KbdEvent.Payload[i-1] == SC1_PREFIX_E0)
            {
                // Temporary clear break flag just to check for extended shifts.
                // If detected, remove the entire extended shift sequence, as
                // it has no effect on SC1-to-VK translation
                NvU8 sc = KbdEvent.Payload[i] & (~SC1_BREAK_MASK);
                if ((sc == SC1_LSHIFT) || (sc == SC1_RSHIFT))
                {
                    OutCode = OutCode >> 8;
                    OutCodeBytes--;
                    continue;
                }
                else if (KbdEvent.Payload[i] == SC1_SCROLL)
                {
                    // If extended ScrollLock = Ctrl+Break, detected store it,
                    // set both make/break flags, and abort buffer packing, as
                    // the following bytes are just the break part of sequence
                    OutCode = (OutCode << 8) | ((NvU32)KbdEvent.Payload[i]);
                    OutCodeBytes++;
                    ScanCodeFlags = NV_ODM_SCAN_CODE_FLAG_MAKE |
                                    NV_ODM_SCAN_CODE_FLAG_BREAK;
                    break;
                }
            }
            if (KbdEvent.Payload[i] == SC1_PREFIX_E1)
            {
                // If 2nd half of Pause key is detected, set both make/break
                // flags, and abort buffer packing, as the following bytes
                // are just the break part of sequence
                ScanCodeFlags = NV_ODM_SCAN_CODE_FLAG_MAKE |
                                NV_ODM_SCAN_CODE_FLAG_BREAK;
                break;
            }
            // If not intercepted by special cases, pack scan code byte into
            // the output dword
            OutCode = (OutCode << 8) | ((NvU32)KbdEvent.Payload[i]);
            OutCodeBytes++;
        }

        // After above packing all SC1 sequences are shrinked to 1-3 byte
        // scan codes; 3-byte scan code always has both make/break flags
        // already set; 2- and 1- byte scan code have break flag in low byte
        // of low word
        if (!ScanCodeFlags)
        {
            switch (OutCodeBytes)
            {
                case 2:
                case 1:
                    ScanCodeFlags = (OutCode & ((NvU32)SC1_BREAK_MASK)) ?
                                    NV_ODM_SCAN_CODE_FLAG_BREAK :
                                    NV_ODM_SCAN_CODE_FLAG_MAKE;
                    OutCode &= ~((NvU32)SC1_BREAK_MASK);
                    break;

                case 0:
                    // Dummy sequence, no actual keystrokes (FIXME - assert ?)
                    return NV_FALSE;

                default:
                    NV_ASSERT(!"Not an SC1 payload - failed to pack");
                    return NV_FALSE;
            }
        }
        *pScanCodeFlags = ScanCodeFlags;
        *pKeyScanCode = OutCode;
        return NV_TRUE;
    }

    return NV_FALSE;
}

NvBool NvOdmKeyboardToggleLights(NvU32 LedId)
{
    NvError NvStatus = NvError_Success;
    NvEcRequest Request = {0};
    NvEcResponse Response = {0};
    NvU8 NewLedState[NUM_OF_LEDS] = {0}, i = 0;

    /* return if EC handle is not available */
    if (!s_NvEcHandle)
        return NV_FALSE;

    /* get the current state for each LED and toggle it */
    for (i = 0; i < NUM_OF_LEDS; i++)
    {
        NewLedState[i] = s_LedState & (LedId & (1 << i));

        if (LedId & (1 << i))
        {
            NewLedState[i] = (~NewLedState[i]) & 0x1;
        }
    }

    /* update the new LED states to be programmed */
    s_LedState = 0;
    for (i = 0; i < NUM_OF_LEDS; i++)
    {
        s_LedState |= (NewLedState[i] << i);
    }

    NVODM_PRINTF(("NvOdmKeyboardToggleLights: LED State = 0x%x", s_LedState));

    /* issue the Set LED command */
    Request.PacketType = NvEcPacketType_Request;
    Request.RequestType = NvEcRequestResponseType_Keyboard;
    Request.RequestSubtype = (NvEcRequestResponseSubtype) NvEcKeyboardSubtype_SetLeds;
    Request.NumPayloadBytes = 1;
    Request.Payload[0] = (NvU8)s_LedState;

    NvStatus = NvEcSendRequest(s_NvEcHandle, &Request, &Response, sizeof(Request), sizeof(Response));
    if (NvStatus != NvError_Success)
    {
        NVODM_PRINTF(("NvOdmKeyboardToggleLights: NvEcSendRequest time out"));
        return NV_FALSE;
    }

    /* check if command passed */
    if (Response.Status != NvEcStatus_Success)
    {
        return NV_FALSE;
    }

    return NV_TRUE;
}

NvBool NvOdmKeyboardPowerHandler(NvBool PowerDown)
{
    return NV_TRUE;
}

/* -----------Stub Implemetation for Hold Switch Adaptation, since we do not need these-------*/

NvBool NvOdmHoldSwitchInit(void)
{
    // firefly should not concerned about the "hold" key
    return NV_FALSE;
}

void NvOdmHoldSwitchDeInit(void)
{
    // do nothing
}