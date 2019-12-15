/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Proxy Server
 *
 * Copyright 2019 Kobi Mizrachi <kmizrachi18@gmail.com>
 * Copyright 2019 Idan Freiberg <speidy@gmail.com>
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

#include "pf_cliprdr.h"
#include "pf_modules.h"
#include "pf_log.h"

#define TAG PROXY_TAG("cliprdr")

#define TEXT_FORMATS_COUNT 2
#define CHUNK_SIZE 65536

/* used for createing a fake format list response, containing only text formats */
static CLIPRDR_FORMAT g_text_formats[] = { { CF_TEXT, '\0' }, { CF_UNICODETEXT, '\0' } };

UINT cliprdr_send_response_filecontents(pfClipboard* clipboard, UINT32 streamId, ULONG index,
                                        BYTE* data, ULONG n, UINT16 flags);
UINT cliprdr_send_request_filecontents(pfClipboard* clipboard, UINT32 streamId, ULONG index,
                                       UINT32 flag, DWORD positionhigh, DWORD positionlow, ULONG n);
BOOL pf_server_cliprdr_init(pServerContext* ps)
{
	CliprdrServerContext* cliprdr;
	ps->cliprdr = cliprdr = cliprdr_server_context_new(ps->vcm);

	if (!cliprdr)
	{
		WLog_ERR(TAG, "cliprdr_server_context_new failed.");
		return FALSE;
	}

	cliprdr->rdpcontext = (rdpContext*)ps;

	/* enable all capabilities */
	cliprdr->useLongFormatNames = TRUE;
	cliprdr->streamFileClipEnabled = TRUE;
	cliprdr->fileClipNoFilePaths = TRUE;
	cliprdr->canLockClipData = TRUE;

	/* disable initialization sequence, for caps sync */
	cliprdr->autoInitializationSequence = FALSE;
	return TRUE;
}

static INLINE BOOL clipboard_is_text_format(UINT32 format)
{
	switch (format)
	{
		case CF_TEXT:
		case CF_UNICODETEXT:
			return TRUE;
	}

	return FALSE;
}

static INLINE void clipboard_create_text_only_format_list(CLIPRDR_FORMAT_LIST* list)
{
	list->msgFlags = CB_RESPONSE_OK;
	list->msgType = CB_FORMAT_LIST;
	list->dataLen = (4 + 1) * TEXT_FORMATS_COUNT;
	list->numFormats = TEXT_FORMATS_COUNT;
	list->formats = g_text_formats;
}

/* format data response PDU returns the copied text as a unicode buffer.
 * clipboard_is_copy_paste_valid returns TRUE if the length of the copied
 * text is valid according to the configuration value of `MaxTextLength`.
 */
static BOOL clipboard_is_copy_paste_valid(UINT32 maxTextLength,
                                          const CLIPRDR_FORMAT_DATA_RESPONSE* pdu, UINT32 format)
{
	size_t copy_len;

	/* no size limit */
	if (maxTextLength == 0)
		return TRUE;

	/* no data */
	if (pdu->dataLen == 0)
		return TRUE;

	WLog_INFO(TAG, "[%s]: checking format %" PRIu32 "", __FUNCTION__, format);

	switch (format)
	{
		case CF_UNICODETEXT:
			copy_len = (pdu->dataLen / 2) - 1;
			break;
		case CF_TEXT:
			copy_len = pdu->dataLen;
			break;
		default:
			WLog_WARN(TAG, "received unknown format: %" PRIu32 "", format);
			return FALSE;
	}

	if (copy_len > maxTextLength)
	{
		WLog_WARN(TAG, "text size is too large: %" PRIu32 " (max %" PRIu32 ")", copy_len,
		          maxTextLength);
		return FALSE;
	}

	return TRUE;
}

/*
 * if the requested text size is too long, we need a way to return a message to the other side of
 * the connection, indicating that the copy/paste operation failed, instead of just not forwarding
 * the response (because that destroys the state of the RDPECLIP channel). This is done by sending a
 * `format_data_response` PDU with msgFlags = CB_RESPONSE_FAIL.
 */
static INLINE void clipboard_create_failed_format_data_response(CLIPRDR_FORMAT_DATA_RESPONSE* dst)
{
	dst->requestedFormatData = NULL;
	dst->dataLen = 0;
	dst->msgType = CB_FORMAT_DATA_RESPONSE;
	dst->msgFlags = CB_RESPONSE_FAIL;
}

static UINT proxy_format_data_response(pfClipboard* clipboard,
                                       const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	/* proxy using the other side of the connection */
	if (clipboard->owner == CLIPBOARD_OWNER_SERVER)
		return clipboard->client->ClientFormatDataResponse(clipboard->client, response);

	return clipboard->server->ServerFormatDataResponse(clipboard->server, response);
}

static BOOL clipboard_handle_file_list(pfClipboard* clipboard,
                                       const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	FILEDESCRIPTOR* files;
	UINT rc;
	UINT32 files_count;
	size_t index;

	if (response->requestedFormatData == NULL)
	{
		/* empty file list? */
		return TRUE;
	}

	WLog_INFO(TAG, "received file list");
	rc = cliprdr_parse_file_list(response->requestedFormatData, response->dataLen, &files,
	                             &files_count);

	if (rc != NO_ERROR)
	{
		WLog_ERR(TAG, "failed to parse file list: error: 0%x", rc);
		return FALSE;
	}

	/* update state */
	clipboard->OnReceivedFileList(clipboard, files, files_count);

	CLIPRDR_FORMAT_DATA_RESPONSE r;
	r.msgType = CB_FORMAT_DATA_RESPONSE;
	r.msgFlags = response->msgFlags;

	for (index = 0; index < files_count; index++)
	{
		proxyPreFileCopyEventInfo event;

		event.client_to_server = TRUE;
		event.total_size = files[index].nFileSizeLow; // TODO: fix this

		if (!pf_modules_run_filter(FILTER_TYPE_CLIPBOARD_FILE_METADATA,
		                           (rdpContext*)clipboard->server->rdpcontext, &event))
			return FALSE;

		if (event.new_name != NULL)
		{
			if (!pf_clipboard_state_change_file_name(clipboard, index, event.new_name))
			{
				free(event.new_name);
				return FALSE;
			}

			free(event.new_name);
		}
	}

	rc =
	    cliprdr_serialize_file_list(files, files_count, (BYTE**)&r.requestedFormatData, &r.dataLen);
	if (rc != NO_ERROR)
		return FALSE;

	WLog_INFO(TAG, "successfully parsed file list: count=%d", files_count);
	return proxy_format_data_response(clipboard, &r) == CHANNEL_RC_OK;
}

static UINT clipboard_handle_format_data_response(pfClipboard* clipboard,
                                                  const CLIPRDR_FORMAT_DATA_RESPONSE* response,
                                                  UINT32 maxTextLength)
{
	WLog_INFO(TAG, __FUNCTION__);

	if (pf_clipboard_state_is_file_list_format(clipboard))
	{
		if (!clipboard_handle_file_list(clipboard, response))
			return ERROR_INTERNAL_ERROR;

		return CHANNEL_RC_OK;
	}

	if (clipboard_is_text_format(clipboard->requestedFormatId))
	{
		if (!clipboard_is_copy_paste_valid(maxTextLength, response, clipboard->requestedFormatId))
		{
			CLIPRDR_FORMAT_DATA_RESPONSE resp;
			clipboard_create_failed_format_data_response(&resp);
			return proxy_format_data_response(clipboard, &resp);
		}
	}

	return proxy_format_data_response(clipboard, response);
}

static UINT clipboard_handle_file_contents_request(proxyData* pdata, pfClipboard* clipboard,
                                                   const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	UINT rc = CHANNEL_RC_OK;
	CliprdrServerContext* server = clipboard->server;
	CliprdrClientContext* client = clipboard->client;
	fileStream* current;
	BOOL all_sent;
	UINT32 n = request->cbRequested;
	BYTE* requested_data = NULL;

	WLog_INFO(TAG, __FUNCTION__);

	if (!clipboard->OnReceivedFileContentsRequest(clipboard, request))
		return ERROR_INTERNAL_ERROR;

	/*
	 * if there's no need to buffer the data, or the request is a request for file size,
	 * proxy the request as is.
	 */
	if (!pdata->config->BufferFileData || request->dwFlags == FILECONTENTS_SIZE)
	{
		if (clipboard->owner == CLIPBOARD_OWNER_SERVER)
			return server->ServerFileContentsRequest(server, request);
		else
			return client->ClientFileContentsRequest(client, request);
	}

	if (pdata->config->TextOnly)
		return CHANNEL_RC_OK;

	current = pf_clipboard_get_stream(clipboard, request->listIndex);
	if (!current)
		return ERROR_INTERNAL_ERROR;

	if (current->passed_filter == FALSE)
	{
		/* filter failed, respond with FAIL flag. */
		pf_clipboard_stream_free(clipboard, request->listIndex);
		return cliprdr_send_response_filecontents(clipboard, request->streamId, request->listIndex,
		                                          NULL, 0, CB_RESPONSE_FAIL);
	}

	requested_data = pf_clipboard_get_chunk(current, request, &n, &all_sent);

	WLog_INFO(TAG, "remote server requested max %" PRIu32 " bytes", request->cbRequested);
	WLog_INFO(TAG, "sending %" PRIu64 " bytes to remote server", n);

	rc = cliprdr_send_response_filecontents(clipboard, request->streamId, request->listIndex,
	                                        requested_data, n, CB_RESPONSE_OK);

	if (all_sent)
	{
		WLog_INFO(TAG, "finished sending file to remote server: index=%" PRIu16 "",
		          request->listIndex);

		pf_clipboard_stream_free(clipboard, request->listIndex);
	}

	return rc;
}

/* server callbacks */
static UINT clipboard_ClientCapabilities(CliprdrServerContext* context,
                                         const CLIPRDR_CAPABILITIES* capabilities)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return client->ClientCapabilities(client, capabilities);
}

static UINT clipboard_TempDirectory(CliprdrServerContext* context,
                                    const CLIPRDR_TEMP_DIRECTORY* tempDirectory)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return client->TempDirectory(client, tempDirectory);
}

static UINT clipboard_ClientFormatList(CliprdrServerContext* context,
                                       const CLIPRDR_FORMAT_LIST* formatList)
{
	proxyData* pdata = (proxyData*)context->custom;
	pClientContext* pc = pdata->pc;
	CliprdrClientContext* client = pc->cliprdr;
	pfClipboard* clipboard = pdata->ps->clipboard;
	WLog_INFO(TAG, __FUNCTION__);

	if (pdata->config->TextOnly)
	{
		/* send a format list that allows only text */
		CLIPRDR_FORMAT_LIST list;
		clipboard_create_text_only_format_list(&list);
		return client->ClientFormatList(client, &list);
	}

	if (!clipboard->OnReceivedFormatList(clipboard, formatList))
		return ERROR_INTERNAL_ERROR;

	return client->ClientFormatList(client, formatList);
}

static UINT clipboard_ClientFormatListResponse(CliprdrServerContext* context,
                                               const CLIPRDR_FORMAT_LIST_RESPONSE* response)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return client->ClientFormatListResponse(client, response);
}

static UINT clipboard_ClientLockClipboardData(CliprdrServerContext* context,
                                              const CLIPRDR_LOCK_CLIPBOARD_DATA* lock)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	pdata->ps->clipboard->clipDataId = lock->clipDataId;
	pdata->ps->clipboard->haveClipDataId = TRUE;
	return client->ClientLockClipboardData(client, lock);
}

static UINT clipboard_ClientUnlockClipboardData(CliprdrServerContext* context,
                                                const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlock)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return client->ClientUnlockClipboardData(client, unlock);
}

static UINT clipboard_ClientFormatDataRequest(CliprdrServerContext* context,
                                              const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);

	pdata->pc->clipboard->requestedFormatId = formatDataRequest->requestedFormatId;

	if (pdata->config->TextOnly && !clipboard_is_text_format(formatDataRequest->requestedFormatId))
	{
		CLIPRDR_FORMAT_DATA_RESPONSE resp;
		clipboard_create_failed_format_data_response(&resp);
		return server->ServerFormatDataResponse(server, &resp);
	}

	return client->ClientFormatDataRequest(client, formatDataRequest);
}

static UINT clipboard_ClientFormatDataResponse(CliprdrServerContext* context,
                                               const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* pfc = pdata->ps->clipboard;

	WLog_INFO(TAG, __FUNCTION__);
	return clipboard_handle_format_data_response(pfc, response, pdata->config->MaxTextLength);
}

static UINT clipboard_ClientFileContentsRequest(CliprdrServerContext* context,
                                                const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* clipboard = pdata->pc->clipboard;
	WLog_INFO(TAG, __FUNCTION__);

	/* server file contents request using client's clipboard state */
	return clipboard_handle_file_contents_request(pdata, clipboard, request);
}

UINT cliprdr_send_request_filecontents(pfClipboard* clipboard, UINT32 streamId, ULONG index,
                                       UINT32 flag, DWORD positionhigh, DWORD positionlow, ULONG n)
{
	CLIPRDR_FILE_CONTENTS_REQUEST request = { 0 };

	if (!clipboard || !clipboard->server || !clipboard->server->ClientFileContentsRequest)
		return ERROR_INTERNAL_ERROR;

	// TODO: encapsulate
	clipboard->requestedDwFlags = flag;

	request.streamId = streamId;
	request.listIndex = index;
	request.dwFlags = flag;
	request.nPositionLow = positionlow;
	request.nPositionHigh = positionhigh;
	request.cbRequested = n;
	request.clipDataId = clipboard->clipDataId;
	request.haveClipDataId = clipboard->haveClipDataId;
	request.msgFlags = 0;
	request.msgType = CB_FILECONTENTS_REQUEST;

	if (clipboard->owner == CLIPBOARD_OWNER_SERVER)
		return clipboard->server->ServerFileContentsRequest(clipboard->server, &request);
	else
		return clipboard->client->ClientFileContentsRequest(clipboard->client, &request);
}

UINT cliprdr_send_response_filecontents(pfClipboard* clipboard, UINT32 streamId, ULONG index,
                                        BYTE* data, ULONG n, UINT16 flags)
{
	CLIPRDR_FILE_CONTENTS_RESPONSE resp = { 0 };

	if (!clipboard || !clipboard->client || !clipboard->client->ClientFileContentsResponse)
		return ERROR_INTERNAL_ERROR;

	resp.streamId = streamId;
	resp.cbRequested = n;
	resp.msgFlags = flags;
	resp.msgType = CB_FILECONTENTS_RESPONSE;
	resp.requestedData = data;

	if (clipboard->owner == CLIPBOARD_OWNER_SERVER)
		return clipboard->client->ClientFileContentsResponse(clipboard->client, &resp);
	else
		return clipboard->server->ServerFileContentsResponse(clipboard->server, &resp);
}

static UINT
clipboard_handle_filecontents_size_response(pfClipboard* clipboard,
                                            const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	UINT16 index = clipboard->requestedFileIndex;
	fileStream* stream;

	stream = pf_clipboard_get_current_stream(clipboard);
	if (!stream)
		return ERROR_BAD_CONFIGURATION;

	if (response->cbRequested != sizeof(UINT64))
		return ERROR_INVALID_DATA;

	/* if requested file is empty, respond immediately with size of file. */
	if (stream->m_lSize.QuadPart == 0)
		return cliprdr_send_response_filecontents(clipboard, response->streamId, index,
		                                          (BYTE*)&stream->m_lSize.QuadPart, 8,
		                                          CB_RESPONSE_OK);

	/* request first size of data */
	return cliprdr_send_request_filecontents(clipboard, response->streamId, index,
	                                         FILECONTENTS_RANGE, stream->m_lOffset.u.HighPart,
	                                         stream->m_lOffset.u.LowPart, CHUNK_SIZE);
}

static UINT
clipboard_handle_filecontents_range_response(pfClipboard* clipboard,
                                             const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	fileStream* stream;
	UINT16 index = clipboard->requestedFileIndex;
	UINT64 total_size;

	stream = pf_clipboard_get_current_stream(clipboard);
	if (!stream || !stream->data)
		return ERROR_BAD_CONFIGURATION;

	total_size = stream->m_lSize.QuadPart;
	WLog_INFO(TAG, "received file contents response from peer: data len=%" PRIu32 "",
	          response->cbRequested);

	if (stream->m_lOffset.QuadPart < total_size)
	{
		/* request more data */
		return cliprdr_send_request_filecontents(clipboard, response->streamId, index,
		                                         FILECONTENTS_RANGE, stream->m_lOffset.u.HighPart,
		                                         stream->m_lOffset.u.LowPart, CHUNK_SIZE);
	}
	else if (stream->m_lOffset.QuadPart == total_size)
	{
		rdpContext* ps = (rdpContext*)clipboard->server->custom;
		proxyFileCopyEventInfo event;
		event.data = Stream_Buffer(stream->data);
		event.data_len = total_size;
		event.client_to_server = (clipboard->owner == CLIPBOARD_OWNER_SERVER);

		WLog_INFO(TAG, "constructed file in memory: index=%d", index);
		stream->passed_filter = pf_modules_run_filter(FILTER_TYPE_CLIPBOARD_FILE_DATA, ps, &event);

		/* respond to remote server with file size */
		return cliprdr_send_response_filecontents(clipboard, response->streamId, index,
		                                          (BYTE*)&total_size, 8, CB_RESPONSE_OK);
	}

	return ERROR_INTERNAL_ERROR;
}

static UINT clipboard_handle_file_contents_response(pfClipboard* clipboard,
                                                    const proxyConfig* config,
                                                    const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	WLog_INFO(TAG, __FUNCTION__);

	/* if TextOnly is set to TRUE, ignore any file contents response PDUs. */
	if (config->TextOnly)
		return CHANNEL_RC_OK;

	if (!config->BufferFileData)
	{
		if (clipboard->owner == CLIPBOARD_OWNER_SERVER)
			return clipboard->client->ClientFileContentsResponse(clipboard->client, response);
		else
			return clipboard->server->ServerFileContentsResponse(clipboard->server, response);
	}

	if (response->msgFlags == CB_RESPONSE_FAIL)
		return ERROR_INTERNAL_ERROR;

	if (!clipboard->OnReceivedFileContentsResponse(clipboard, response))
		return ERROR_INTERNAL_ERROR;

	switch (clipboard->requestedDwFlags)
	{
		case FILECONTENTS_SIZE:
			return clipboard_handle_filecontents_size_response(clipboard, response);
		case FILECONTENTS_RANGE:
			return clipboard_handle_filecontents_range_response(clipboard, response);
		default:
			return ERROR_BAD_ARGUMENTS;
	}
}

static UINT clipboard_ClientFileContentsResponse(CliprdrServerContext* context,
                                                 const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* clipboard = pdata->ps->clipboard;
	WLog_INFO(TAG, __FUNCTION__);

	return clipboard_handle_file_contents_response(clipboard, pdata->config, response);
}

/* client callbacks */

static UINT clipboard_ServerCapabilities(CliprdrClientContext* context,
                                         const CLIPRDR_CAPABILITIES* capabilities)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return server->ServerCapabilities(server, capabilities);
}

static UINT clipboard_MonitorReady(CliprdrClientContext* context,
                                   const CLIPRDR_MONITOR_READY* monitorReady)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return server->MonitorReady(server, monitorReady);
}

static UINT clipboard_ServerFormatList(CliprdrClientContext* context,
                                       const CLIPRDR_FORMAT_LIST* formatList)
{
	proxyData* pdata = (proxyData*)context->custom;
	pServerContext* ps = pdata->ps;
	CliprdrServerContext* server = ps->cliprdr;
	pfClipboard* clipboard = pdata->pc->clipboard;

	WLog_INFO(TAG, __FUNCTION__);

	if (pdata->config->TextOnly)
	{
		CLIPRDR_FORMAT_LIST list;
		clipboard_create_text_only_format_list(&list);
		return server->ServerFormatList(server, &list);
	}

	if (!clipboard->OnReceivedFormatList(clipboard, formatList))
		return ERROR_INTERNAL_ERROR;

	return server->ServerFormatList(server, formatList);
}

static UINT clipboard_ServerFormatListResponse(CliprdrClientContext* context,
                                               const CLIPRDR_FORMAT_LIST_RESPONSE* formatList)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return server->ServerFormatListResponse(server, formatList);
}

static UINT clipboard_ServerLockClipboardData(CliprdrClientContext* context,
                                              const CLIPRDR_LOCK_CLIPBOARD_DATA* lock)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	pdata->pc->clipboard->clipDataId = lock->clipDataId;
	pdata->pc->clipboard->haveClipDataId = TRUE;
	return server->ServerLockClipboardData(server, lock);
}

static UINT clipboard_ServerUnlockClipboardData(CliprdrClientContext* context,
                                                const CLIPRDR_UNLOCK_CLIPBOARD_DATA* unlock)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);
	return server->ServerUnlockClipboardData(server, unlock);
}

static UINT clipboard_ServerFormatDataRequest(CliprdrClientContext* context,
                                              const CLIPRDR_FORMAT_DATA_REQUEST* formatDataRequest)
{
	proxyData* pdata = (proxyData*)context->custom;
	CliprdrServerContext* server = pdata->ps->cliprdr;
	CliprdrClientContext* client = pdata->pc->cliprdr;
	WLog_INFO(TAG, __FUNCTION__);

	pdata->ps->clipboard->requestedFormatId = formatDataRequest->requestedFormatId;

	if (pdata->config->TextOnly && !clipboard_is_text_format(formatDataRequest->requestedFormatId))
	{
		/* proxy's client needs to return a failed response directly to the client */
		CLIPRDR_FORMAT_DATA_RESPONSE resp;
		clipboard_create_failed_format_data_response(&resp);
		return client->ClientFormatDataResponse(client, &resp);
	}

	return server->ServerFormatDataRequest(server, formatDataRequest);
}

static UINT clipboard_ServerFormatDataResponse(CliprdrClientContext* context,
                                               const CLIPRDR_FORMAT_DATA_RESPONSE* response)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* pfc = pdata->pc->clipboard;

	WLog_INFO(TAG, __FUNCTION__);
	return clipboard_handle_format_data_response(pfc, response, pdata->config->MaxTextLength);
}

static UINT clipboard_ServerFileContentsRequest(CliprdrClientContext* context,
                                                const CLIPRDR_FILE_CONTENTS_REQUEST* request)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* clipboard = pdata->ps->clipboard;

	WLog_INFO(TAG, __FUNCTION__);

	/* server file contents request using server's clipboard state */
	return clipboard_handle_file_contents_request(pdata, clipboard, request);
}

static UINT clipboard_ServerFileContentsResponse(CliprdrClientContext* context,
                                                 const CLIPRDR_FILE_CONTENTS_RESPONSE* response)
{
	proxyData* pdata = (proxyData*)context->custom;
	pfClipboard* clipboard = pdata->pc->clipboard;
	WLog_INFO(TAG, __FUNCTION__);

	return clipboard_handle_file_contents_response(clipboard, pdata->config, response);
}

void pf_cliprdr_register_callbacks(CliprdrClientContext* cliprdr_client,
                                   CliprdrServerContext* cliprdr_server, proxyData* pdata)
{
	pServerContext* ps = pdata->ps;

	/* Set server and client side references to proxy data */
	cliprdr_server->custom = (void*)pdata;
	cliprdr_client->custom = (void*)pdata;

	/* Set server callbacks */
	cliprdr_server->ClientCapabilities = clipboard_ClientCapabilities;
	cliprdr_server->TempDirectory = clipboard_TempDirectory;
	cliprdr_server->ClientFormatList = clipboard_ClientFormatList;
	cliprdr_server->ClientFormatListResponse = clipboard_ClientFormatListResponse;
	cliprdr_server->ClientLockClipboardData = clipboard_ClientLockClipboardData;
	cliprdr_server->ClientUnlockClipboardData = clipboard_ClientUnlockClipboardData;
	cliprdr_server->ClientFormatDataRequest = clipboard_ClientFormatDataRequest;
	cliprdr_server->ClientFormatDataResponse = clipboard_ClientFormatDataResponse;
	cliprdr_server->ClientFileContentsRequest = clipboard_ClientFileContentsRequest;
	cliprdr_server->ClientFileContentsResponse = clipboard_ClientFileContentsResponse;

	/* Set client callbacks */
	cliprdr_client->ServerCapabilities = clipboard_ServerCapabilities;
	cliprdr_client->MonitorReady = clipboard_MonitorReady;
	cliprdr_client->ServerFormatList = clipboard_ServerFormatList;
	cliprdr_client->ServerFormatListResponse = clipboard_ServerFormatListResponse;
	cliprdr_client->ServerLockClipboardData = clipboard_ServerLockClipboardData;
	cliprdr_client->ServerUnlockClipboardData = clipboard_ServerUnlockClipboardData;
	cliprdr_client->ServerFormatDataRequest = clipboard_ServerFormatDataRequest;
	cliprdr_client->ServerFormatDataResponse = clipboard_ServerFormatDataResponse;
	cliprdr_client->ServerFileContentsRequest = clipboard_ServerFileContentsRequest;
	cliprdr_client->ServerFileContentsResponse = clipboard_ServerFileContentsResponse;

	ps->clipboard = pf_clipboard_state_new(cliprdr_server, cliprdr_client, CLIPBOARD_OWNER_SERVER);
	if (!ps->clipboard)
	{
		WLog_ERR(TAG, "cliprdr_server_context_new: pf_clipboard_state_new failed!");
	}
}