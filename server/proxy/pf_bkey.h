/**
 * FreeRDP: A Remote Desktop Protocol Implementation
 * FreeRDP Proxy Server BioKey channel
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

#ifndef FREERDP_SERVER_PROXY_PFBKEY_H
#define FREERDP_SERVER_PROXY_PFBKEY_H

#include <freerdp/client/passthrough.h>
#include <freerdp/server/passthrough.h>

#include "pf_context.h"

#define BKEY_CHANNEL_NAME "bkey66"

BOOL pf_server_bkey_init(pServerContext* ps);
void pf_bkey_pipeline_init(PassthroughClientContext* client, PassthroughServerContext* server,
                             proxyData* pdata);

#endif /* FREERDP_SERVER_PROXY_PFBKEY_H */