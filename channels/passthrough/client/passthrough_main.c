/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * Passthrough virtual channel
 *
 * Copyright 2019 Kobi Mizrachi <kmizrachi18@gmail.com>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <winpr/crt.h>
#include <winpr/print.h>

#include <freerdp/types.h>
#include <freerdp/constants.h>
#include <freerdp/client/passthrough.h>

#include "passthrough_main.h"

PassthroughClientContext* passthrough_get_client_interface(passthroughPlugin* passthrough)
{
	PassthroughClientContext* pInterface;

	if (!passthrough)
		return NULL;

	pInterface = (PassthroughClientContext*)passthrough->channelEntryPoints.pInterface;
	return pInterface;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT passthrough_send_data(PassthroughClientContext* context, const BYTE* data, UINT32 len)
{
	UINT status;
	wStream* s;

	/*
	 * the underline buffer `data` wasn't allocated by the client, therefore isOwner must be set to
	 * FALSE to avoid a double free issue.
	 */
	s = Stream_New((BYTE*)data, len);
	if (!s)
	{
		WLog_ERR(TAG, "malloc failed!");
		return CHANNEL_RC_NO_MEMORY;
	}

	s->isOwner = FALSE;

	passthroughPlugin* passthrough = (passthroughPlugin*)context->handle;

	if (!passthrough)
	{
		status = CHANNEL_RC_BAD_INIT_HANDLE;
	}
	else
	{
		status = passthrough->channelEntryPoints.pVirtualChannelWriteEx(
		    passthrough->InitHandle, passthrough->OpenHandle, (PCHAR)Stream_Buffer(s), len, s);
	}

	if (status != CHANNEL_RC_OK)
		WLog_ERR(TAG, "VirtualChannelWrite failed with %s [%08" PRIX32 "]",
		         WTSErrorToString(status), status);

	if (!context->async_write)
	{
		WLog_DBG(TAG, "waiting for write to finish");
		WaitForSingleObject(passthrough->write_complete, INFINITE);
		ResetEvent(passthrough->write_complete);
	}

	return status;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT passthrough_virtual_channel_event_data_received(passthroughPlugin* passthrough,
                                                            void* pData, UINT32 dataLength,
                                                            UINT32 totalLength, UINT32 dataFlags)
{
	WLog_DBG(TAG, __FUNCTION__);
	UINT error = CHANNEL_RC_OK;
	PassthroughClientContext* context = passthrough_get_client_interface(passthrough);
	IFCALLRET(context->DataReceived, error, context, pData, dataLength);
	return error;
}

static VOID VCAPITYPE passthrough_virtual_channel_open_event_ex(LPVOID lpUserParam,
                                                                DWORD openHandle, UINT event,
                                                                LPVOID pData, UINT32 dataLength,
                                                                UINT32 totalLength,
                                                                UINT32 dataFlags)
{
	UINT error = CHANNEL_RC_OK;
	passthroughPlugin* passthrough = (passthroughPlugin*)lpUserParam;

	if (!passthrough || (passthrough->OpenHandle != openHandle))
	{
		WLog_ERR(TAG, "error no match");
		return;
	}

	switch (event)
	{
		case CHANNEL_EVENT_DATA_RECEIVED:
			if ((error = passthrough_virtual_channel_event_data_received(
			         passthrough, pData, dataLength, totalLength, dataFlags)))
				WLog_ERR(TAG, "failed with error %" PRIu32 "", error);

			break;

		case CHANNEL_EVENT_WRITE_COMPLETE:
			SetEvent(passthrough->write_complete);
			break;

		case CHANNEL_EVENT_USER:
			break;
	}

	if (error && passthrough->context->rdpcontext)
		setChannelError(passthrough->context->rdpcontext, error,
		                "passthrough_virtual_channel_client_thread reported an error");
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT passthrough_virtual_channel_event_connected(passthroughPlugin* passthrough,
                                                        LPVOID pData, UINT32 dataLength)
{
	UINT32 status;
	status = passthrough->channelEntryPoints.pVirtualChannelOpenEx(
	    passthrough->InitHandle, &passthrough->OpenHandle, passthrough->channelDef.name,
	    passthrough_virtual_channel_open_event_ex);

	if (status != CHANNEL_RC_OK)
	{
		WLog_ERR(TAG, "pVirtualChannelOpen failed with %s [%08" PRIX32 "]",
		         WTSErrorToString(status), status);
		return status;
	}

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT passthrough_virtual_channel_event_disconnected(passthroughPlugin* passthrough)
{
	UINT rc;

	if (passthrough->OpenHandle == 0)
		return CHANNEL_RC_OK;

	rc = passthrough->channelEntryPoints.pVirtualChannelCloseEx(passthrough->InitHandle,
	                                                            passthrough->OpenHandle);

	if (CHANNEL_RC_OK != rc)
	{
		WLog_ERR(TAG, "pVirtualChannelClose failed with %s [%08" PRIX32 "]", WTSErrorToString(rc),
		         rc);
		return rc;
	}

	passthrough->OpenHandle = 0;

	return CHANNEL_RC_OK;
}

/**
 * Function description
 *
 * @return 0 on success, otherwise a Win32 error code
 */
static UINT passthrough_virtual_channel_event_terminated(passthroughPlugin* passthrough)
{
	passthrough->InitHandle = 0;

	if (passthrough->write_complete)
		CloseHandle(passthrough->write_complete);

	free(passthrough->context);
	free(passthrough);
	return CHANNEL_RC_OK;
}

static VOID VCAPITYPE passthrough_virtual_channel_init_event_ex(LPVOID lpUserParam,
                                                                LPVOID pInitHandle, UINT event,
                                                                LPVOID pData, UINT dataLength)
{
	UINT error = CHANNEL_RC_OK;
	passthroughPlugin* passthrough = (passthroughPlugin*)lpUserParam;

	if (!passthrough || (passthrough->InitHandle != pInitHandle))
	{
		WLog_ERR(TAG, "error no match");
		return;
	}

	switch (event)
	{
		case CHANNEL_EVENT_CONNECTED:
			if ((error =
			         passthrough_virtual_channel_event_connected(passthrough, pData, dataLength)))
				WLog_ERR(TAG,
				         "passthrough_virtual_channel_event_connected failed with error %" PRIu32
				         "!",
				         error);

			break;

		case CHANNEL_EVENT_DISCONNECTED:
			if ((error = passthrough_virtual_channel_event_disconnected(passthrough)))
				WLog_ERR(TAG,
				         "passthrough_virtual_channel_event_disconnected failed with error %" PRIu32
				         "!",
				         error);

			break;

		case CHANNEL_EVENT_TERMINATED:
			if ((error = passthrough_virtual_channel_event_terminated(passthrough)))
				WLog_ERR(TAG,
				         "passthrough_virtual_channel_event_terminated failed with error %" PRIu32
				         "!",
				         error);

			break;
	}

	if (error && passthrough->context->rdpcontext)
		setChannelError(passthrough->context->rdpcontext, error,
		                "passthrough_virtual_channel_init_event reported an error");
}

#define VirtualChannelEntryEx plex_VirtualChannelEntryEx

BOOL VCAPITYPE VirtualChannelEntryEx(PCHANNEL_ENTRY_POINTS pEntryPoints, PVOID pInitHandle)
{
	ADDIN_ARGV* args;
	char* channel_name;
	UINT rc;
	passthroughPlugin* passthrough;
	PassthroughClientContext* context = NULL;
	CHANNEL_ENTRY_POINTS_FREERDP_EX* pEntryPointsEx;
	passthrough = (passthroughPlugin*)calloc(1, sizeof(passthroughPlugin));

	if (!passthrough)
	{
		WLog_ERR(TAG, "calloc failed!");
		return FALSE;
	}

	if (!(passthrough->write_complete = CreateEvent(NULL, TRUE, FALSE, NULL)))
	{
		WLog_ERR(TAG, "CreateEvent failed!");
		goto error;
	}

	passthrough->channelDef.options = CHANNEL_OPTION_INITIALIZED;

	pEntryPointsEx = (CHANNEL_ENTRY_POINTS_FREERDP_EX*)pEntryPoints;
	args = (ADDIN_ARGV*)pEntryPointsEx->pExtendedData;
	channel_name = (char*)args->argv[1];

	/* check channel length */
	if (strlen(channel_name) > CHANNEL_NAME_LEN)
	{
		WLog_ERR(TAG, "channel name length is invalid");
		goto error;
	}

	strncpy(passthrough->channelDef.name, channel_name, CHANNEL_NAME_LEN);

	if ((pEntryPointsEx->cbSize >= sizeof(CHANNEL_ENTRY_POINTS_FREERDP_EX)) &&
	    (pEntryPointsEx->MagicNumber == FREERDP_CHANNEL_MAGIC_NUMBER))
	{
		context = (PassthroughClientContext*)calloc(1, sizeof(PassthroughClientContext));

		if (!context)
		{
			WLog_ERR(TAG, "calloc failed!");
			goto error;
		}

		context->handle = (void*)passthrough;
		context->custom = NULL;
		context->DataReceived = NULL;
		context->SendData = passthrough_send_data;
		context->async_write = TRUE;
		passthrough->context = context;
		context->rdpcontext = pEntryPointsEx->context;
	}

	passthrough->log = WLog_Get("com.freerdp.channels.passthrough.client");
	WLog_Print(passthrough->log, WLOG_DEBUG, "VirtualChannelEntryEx");
	CopyMemory(&(passthrough->channelEntryPoints), pEntryPoints,
	           sizeof(CHANNEL_ENTRY_POINTS_FREERDP_EX));
	passthrough->InitHandle = pInitHandle;
	rc = passthrough->channelEntryPoints.pVirtualChannelInitEx(
	    passthrough, context, pInitHandle, &passthrough->channelDef, 1,
	    VIRTUAL_CHANNEL_VERSION_WIN2000, passthrough_virtual_channel_init_event_ex);

	if (CHANNEL_RC_OK != rc)
	{
		WLog_ERR(TAG, "pVirtualChannelInit failed with %s [%08" PRIX32 "]", WTSErrorToString(rc),
		         rc);
		goto error;
	}

	passthrough->channelEntryPoints.pInterface = context;
	return TRUE;

error:
	if (passthrough->write_complete)
		CloseHandle(passthrough->write_complete);

	free(passthrough->context);
	free(passthrough);
	return FALSE;
}