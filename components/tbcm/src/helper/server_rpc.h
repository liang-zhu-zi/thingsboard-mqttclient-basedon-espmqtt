// Copyright 2022 liangzhuzhi2020@gmail.com, https://github.com/liang-zhu-zi/esp32-thingsboard-mqtt-client
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// This file is called by tbc_mqtt_helper.c/.h.

#ifndef _SERVER_RPC_OBSERVER_H_
#define _SERVER_RPC_OBSERVER_H_

#include <stdint.h>
#include <stdbool.h>

#include "tbc_utils.h"
#include "tbc_mqtt_helper.h"

#ifdef __cplusplus
extern "C" {
#endif

//====5.Server-side RPC================================================================================================
/**
 * ThingsBoard MQTT Client Helper server-RPC
 */
typedef struct server_rpc
{
     tbcmh_handle_t client;        /*!< ThingsBoard MQTT Client Helper */

     char *method; /*!< method value */
     ////char *method_key;   /*!< method key, default "method" */
     ////char *params_key;   /*!< params key, default "params" */
     ////char *results_key;  /*!< results key, default "results" */

     void *context;                           /*!< Context of callback */
     tbcmh_serverrpc_on_request_t on_request; /*!< Callback of server-rpc request */

     LIST_ENTRY(server_rpc) entry;
} server_rpc_t;

tbc_err_t _tbcmh_serverrpc_empty(tbcmh_handle_t client);

void      _tbcmh_serverrpc_on_connected(tbcmh_handle_t client);
void      _tbcmh_serverrpc_on_request(tbcmh_handle_t client, int request_id, const cJSON *object);

#ifdef __cplusplus
}
#endif //__cplusplus

#endif
