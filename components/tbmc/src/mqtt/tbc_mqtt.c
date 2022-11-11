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

// ThingsBoard MQTT Client low layer API

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "sys/queue.h"
#include "esp_err.h"
#include "mqtt_client.h"

#include "tbc_transport_config.h"
#include "tbc_transport_storage.h"

#include "tbc_mqtt.h"

#include "tbc_utils.h"

#include "tbc_mqtt_payload_buffer.h"


typedef enum
{
     TBCM_REQUEST_ATTR = 1,
     TBCM_REQUEST_CLIENTRPC,
     TBCM_REQUEST_FWUPDATE,
     TBCM_REQUEST_PROVISION
} tbcm_request_type_t;

typedef struct tbcm_request
{
     tbcm_request_type_t type;
     int request_id;

     uint64_t timestamp; /*!< time stamp at sending request */
     void *context;
     void *on_response; /*!< tbcm_on_response_t or tbcm_on_otaupdate_response_t */
     void *on_timeout; /*!< tbcm_on_timeout_t */

     LIST_ENTRY(tbcm_request) entry;
} tbcm_request_t;

typedef LIST_HEAD(tbcm_request_list, tbcm_request) tbcm_request_list_t;

/**
 * ThingsBoard MQTT Client
 */
typedef struct tbcm_client
{
     esp_mqtt_client_handle_t mqtt_handle;

     tbc_transport_storage_t config;                       /*!< ThingsBoard MQTT config */
     void *context;
     tbcm_on_connected_t on_connected;                     /*!< Callback of connected ThingsBoard MQTT */
     tbcm_on_disconnected_t on_disconnected;               /*!< Callback of disconnected ThingsBoard MQTT */
     tbcm_on_sharedattr_received_t on_sharedattr_received; /*!< Callback of receiving ThingsBoard MQTT shared-attribute T*/
     tbcm_on_serverrpc_request_t on_serverrpc_request;     /*!< Callback of receiving ThingsBoard MQTT server-RPC request */

     volatile tbcm_state_t state; // TBMQTT_STATE state;

     SemaphoreHandle_t lock;
     int next_request_id;
     uint64_t last_check_timestamp;
     tbcm_request_list_t request_list; /*!< request list: attributes request, client side RPC & ota update request */ ////QueueHandle_t timeoutQueue;

     tbcm_payload_buffer_t buffer;     /*!< If payload may be into multiple packets, then multiple packages need to be merged, eg: F/W OTA! */
} tbcm_t;

//static int _tbcm_subscribe(tbcm_handle_t client_, const char *topic, int qos /*=0*/);
static int _tbcm_publish(tbcm_handle_t client_, const char *topic, const char *payload, int qos /*= 1*/, int retain /*= 0*/);

static void _on_MqttEventHandle(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data);
static void _on_DataEventProcess(tbcm_handle_t client_, esp_mqtt_event_handle_t event);
static void _on_PayloadProcess(void *context/*client*/, tbcm_rx_msg_info* rx_msg);

/*static*/ bool _request_is_equal(const tbcm_request_t *a, const tbcm_request_t *b);
static int _request_list_create_and_append(tbcm_handle_t client_, tbcm_request_type_t type, int request_id,
                                           void *context,
                                           void *on_response, /*tbcm_on_response_t*/
                                           void *on_timeout); /*tbcm_on_timeout_t*/
static tbcm_request_t *_request_list_search_and_remove(tbcm_handle_t client_, int request_id);
static tbcm_request_t *_request_list_search_and_remove_by_type(tbcm_handle_t client_, tbcm_request_type_t type);
static int _request_list_move_all_of_timeout(tbcm_handle_t client_, uint64_t timestamp,
                                             tbcm_request_list_t *timeout_request_list);
static tbcm_request_t *_request_create(tbcm_request_type_t type,
                                       uint32_t request_id,
                                       void *context,
                                       void *on_response, /*tbcm_on_response_t*/
                                       void *on_timeout); /*tbcm_on_timeout_t*/
static void _request_destroy(tbcm_request_t *tbcm_request);

const static char *TAG = "tb_mqtt_client";

static void *_tbc_transport_config_fill_to_mqtt_client_config(const tbc_transport_config_t *transport,
                                                      esp_mqtt_client_config_t *mqtt_config)
{
    if (!mqtt_config) {
         TBC_LOGE("mqtt_config is NULL! %s()", __FUNCTION__);
         return NULL;
    }
    if (!transport) {
         TBC_LOGE("transport is NULL! %s()", __FUNCTION__);
         return NULL;
    }

    // address
    bool tlsEnabled = false;
    if (strcmp(transport->address.schema, "mqtt") == 0) {
        mqtt_config->transport = MQTT_TRANSPORT_OVER_TCP;
        mqtt_config->port = 1883;
    } else if (strcmp(transport->address.schema, "mqtts") == 0) {
        mqtt_config->transport = MQTT_TRANSPORT_OVER_SSL;
        mqtt_config->port = 8883;
        tlsEnabled = true;
    } else if (strcmp(transport->address.schema, "ws") == 0) {
        mqtt_config->transport = MQTT_TRANSPORT_OVER_WS;
        mqtt_config->port = 80;
    } else if (strcmp(transport->address.schema, "wss") == 0) {
        mqtt_config->transport = MQTT_TRANSPORT_OVER_WSS;
        mqtt_config->port = 443;
        tlsEnabled = true;
    } else {
        ESP_LOGE(TAG, "address->schema(%s) is error!", transport->address.schema);
        return NULL;
    }
   
    if (transport->address.host) {
        mqtt_config->host = transport->address.host;
    } else {
        ESP_LOGE(TAG, "mqtt_config->host is NULL!");
        return NULL;
    }

    if (transport->address.port) {
        mqtt_config->port = transport->address.port;
    }
    if (transport->address.path) {
        mqtt_config->path = transport->address.path;
    }

    //credentials
    switch (transport->credentials.type) {
    case TBC_TRANSPORT_CREDENTIALS_TYPE_NONE: // for provision
        mqtt_config->username = transport->credentials.username; //"provision"
        break;
    case TBC_TRANSPORT_CREDENTIALS_TYPE_ACCESS_TOKEN: // Access Token
        if (!transport->credentials.token) {
             TBC_LOGE("credentials->token is NULL! %s()", __FUNCTION__);
             return NULL;
        }
        mqtt_config->username = transport->credentials.token;
        break;
        
    case TBC_TRANSPORT_CREDENTIALS_TYPE_BASIC_MQTT: // Basic MQTT Credentials.for MQTT
        if (!transport->credentials.client_id && !transport->credentials.username) {
             TBC_LOGE("credentials->client_id && credentials->username are NULL in Basic MQTT authentication! %s()",
                __FUNCTION__);
             return NULL;
        }
        mqtt_config->client_id = transport->credentials.client_id;
        mqtt_config->username = transport->credentials.username;
        mqtt_config->password = transport->credentials.password;
        break;
        
    case TBC_TRANSPORT_CREDENTIALS_TYPE_X509:      // X.509 Certificate
        if (!tlsEnabled) {
            TBC_LOGE("credentials->type(%d) and address->schema(%s) is not match! ()%s",
                transport->credentials.type, transport->address.schema, __FUNCTION__);
            return NULL;
        }
        // NOTE: transport->credentials.token: At TBC_TRANSPORT_CREDENTIALS_TYPE_X509 it's a client public key. DON'T USE IT! */
        break;
        
    default:
        ESP_LOGE(TAG, "credentials->type(%d) is error!", transport->credentials.type);
        return NULL;
    }

    //authentication
    if (tlsEnabled) {
        if (!transport->verification.cert_pem) {
            TBC_LOGE("verification->cert_pem is request but it is NULL! %s()", __FUNCTION__);
            return NULL;
        }
        mqtt_config->cert_pem = transport->verification.cert_pem;
        mqtt_config->cert_len = transport->verification.cert_len;
        mqtt_config->skip_cert_common_name_check = transport->verification.skip_cert_common_name_check;

        // SSL mutual authentication (two-way SSL)
        if (transport->credentials.type == TBC_TRANSPORT_CREDENTIALS_TYPE_X509) {
            if (!transport->authentication.client_cert_pem) {
                TBC_LOGE("authentication->client_cert_pem is request but it is NULL! %s()", __FUNCTION__);
                return NULL;
            }
            if (!transport->authentication.client_key_pem) {
                TBC_LOGE("authentication->client_key_pem is request but it is NULL! %s()", __FUNCTION__);
                return NULL;
            }
            mqtt_config->client_cert_pem         = transport->authentication.client_cert_pem;
            mqtt_config->client_cert_len         = transport->authentication.client_cert_len;
            mqtt_config->client_key_pem          = transport->authentication.client_key_pem;
            mqtt_config->client_key_len          = transport->authentication.client_key_len;
            mqtt_config->clientkey_password      = transport->authentication.client_key_password;
            mqtt_config->clientkey_password_len  = transport->authentication.client_key_password_len;
        }
    }

    return mqtt_config;
}


// Initializes tbcm_handle_t with network client.
tbcm_handle_t tbcm_init(void)
{
     tbcm_t *client = TBCM_MALLOC(sizeof(tbcm_t));
     if (!client) {
          TBC_LOGE("Unable to malloc memeory!");
          return NULL;
     }

     client->mqtt_handle = NULL;
     memset(&client->config, 0x00, sizeof(client->config));
     client->context = NULL;
     client->on_connected = NULL;           /*!< Callback of connected ThingsBoard MQTT */
     client->on_disconnected = NULL;        /*!< Callback of disconnected ThingsBoard MQTT */
     client->on_sharedattr_received = NULL; /*!< Callback of receiving ThingsBoard MQTT shared-attribute T*/
     client->on_serverrpc_request = NULL;   /*!< Callback of receiving ThingsBoard MQTT server-RPC request */

     client->state = TBCM_STATE_DISCONNECTED;

     client->lock = xSemaphoreCreateMutex();
     client->next_request_id = 0;
     client->last_check_timestamp = (uint64_t)time(NULL);
     memset(&client->request_list, 0x00, sizeof(client->request_list));//client->request_list = LIST_HEAD_INITIALIZER(client->request_list);

     tbcm_payload_buffer_init(&client->buffer);
     return client;
}

// Destroys tbcm_handle_t with network client.
void tbcm_destroy(tbcm_handle_t client_)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return;
     }

     if (client->mqtt_handle) {
          tbcm_disconnect(client);
          client->mqtt_handle = NULL;
     }
     if (client->lock) {
          vSemaphoreDelete(client->lock);
          client->lock = NULL;
     }

     tbcm_payload_buffer_clear(&client->buffer);

     TBCM_FREE(client);
}

// Connects to the specified ThingsBoard server and port.
// Access token is used to authenticate a client.
// Returns true on success, false otherwise.
bool tbcm_connect(tbcm_handle_t client_,
                  const tbc_transport_config_t *config,
                  void *context,
                  tbcm_on_connected_t on_connected,
                  tbcm_on_disconnected_t on_disconnected,
                  tbcm_on_sharedattr_received_t on_sharedattr_received,
                  // tbcm_on_attrrequest_response_t on_attrrequest_response,
                  // tbcm_on_attrrequest_timeout_t on_attrrequest_timeout,
                  // tbcm_on_clientrpc_response_t on_clientrpc_response,
                  // tbcm_on_clientrpc_timeout_t on_clientrpc_timeout,
                  tbcm_on_serverrpc_request_t on_serverrpc_request //,
                  /* tbcm_on_otaupdate_response_t on_otaupdate_response, */
                  /* tbcm_on_otaupdate_timeout_t on_otaupdate_timeout */) // connect()//...start()
{
     /*const char *host, int port = 1883, */
     /*min_reconnect_delay=1, timeout=120, tls=False, ca_certs=None, cert_file=None, key_file=None*/
     if (!client_ || !config) {
          TBC_LOGW("one argument isn't NULL!");
          return false;
     }

     tbcm_t *client = (tbcm_t*)client_;
     if (client->mqtt_handle) {
          TBC_LOGW("unable to re-connect mqtt client: client isn't NULL!");
          return false; //!!
     }

     tbc_transport_storage_free_fields(&client->config);
     client->context = NULL;
     client->on_connected = NULL;           /*!< Callback of connected ThingsBoard MQTT */
     client->on_disconnected = NULL;        /*!< Callback of disconnected ThingsBoard MQTT */
     client->on_sharedattr_received = NULL; /*!< Callback of receiving ThingsBoard MQTT shared-attribute T*/
     client->on_serverrpc_request = NULL;   /*!< Callback of receiving ThingsBoard MQTT server-RPC request */
     client->state = TBCM_STATE_DISCONNECTED;

     // SemaphoreHandle_t lock;
     // int next_request_id;
     // uint64_t last_check_timestamp;
     // tbcm_request_list_t request_list; /*!< request list: attributes request, client side RPC & ota update request */ ////QueueHandle_t timeoutQueue;

     esp_mqtt_client_config_t mqtt_cfg = {0};
     _tbc_transport_config_fill_to_mqtt_client_config(config, &mqtt_cfg);
     client->mqtt_handle = esp_mqtt_client_init(&mqtt_cfg);
     if (!client->mqtt_handle)
     {
          TBC_LOGW("unable to init mqtt client");
          return false;
     }
     /* The last argument may be used to pass data to the event handler, in this example mqtt_event_handler */
     esp_mqtt_client_register_event(client->mqtt_handle, ESP_EVENT_ANY_ID, _on_MqttEventHandle, client);
     int32_t result = esp_mqtt_client_start(client->mqtt_handle);
     if (result != ESP_OK)
     {
          TBC_LOGW("unable to start mqtt client");
          esp_mqtt_client_destroy(client->mqtt_handle);
          client->mqtt_handle = NULL;
          return false;
     }

     tbc_transport_storage_fill_from_config(&client->config, config);
     client->context = context;
     client->on_connected = on_connected;
     client->on_disconnected = on_disconnected;
     client->on_sharedattr_received = on_sharedattr_received;
     client->on_serverrpc_request = on_serverrpc_request;
     client->state = TBCM_STATE_CONNECTING;
     return true;
}

// Disconnects from ThingsBoard. Returns true on success.
void tbcm_disconnect(tbcm_handle_t client_) // disconnect()//...stop()
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return;
     }
     if (!client->mqtt_handle) {
          TBC_LOGW("unable to disconnect mqtt client: mqtt client is NULL!");
          return;
     }
     void *context = client->context;

     TBC_LOGI("tbcm_disconnect(): call esp_mqtt_client_stop()...");
     int32_t result = esp_mqtt_client_stop(client->mqtt_handle);
     if (result != ESP_OK) {
          TBC_LOGE("unable to stop mqtt client");
          return;
     }
     TBC_LOGI("tbcm_disconnect(): call esp_mqtt_client_destroy()...");
     result = esp_mqtt_client_destroy(client->mqtt_handle);
     if (result != ESP_OK) {
          TBC_LOGE("unable to stop mqtt client");
          return;
     }
     client->mqtt_handle = NULL;

     tbc_transport_storage_free_fields(&client->config);
     client->context = NULL;
     client->on_connected = NULL;
     client->on_disconnected = NULL;
     client->on_sharedattr_received = NULL;
     client->on_serverrpc_request = NULL;

     client->state = TBCM_STATE_DISCONNECTED;

     // SemaphoreHandle_t lock;
     // int next_request_id;
     // uint64_t last_check_timestamp;
     // remove all item in request_list
     tbcm_request_t *tbcm_request = NULL, *next;
     LIST_FOREACH_SAFE(tbcm_request, &client->request_list, entry, next) {
          // exec timeout callback
          if (tbcm_request->on_timeout) {
               tbcm_on_timeout_t on_timeout = tbcm_request->on_timeout;
               on_timeout(context, tbcm_request->request_id);
          }

          // remove from request list
          LIST_REMOVE(tbcm_request, entry);
          _request_destroy(tbcm_request);
     }
     memset(&client->request_list, 0x00, sizeof(client->request_list));
}

// Returns true if connected, false otherwise.
bool tbcm_is_connected(tbcm_handle_t client_) // isConnected
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return false;
     }
     return client->state == TBCM_STATE_CONNECTED; 
}

bool tbcm_is_connecting(tbcm_handle_t client)
{
     return client->state == TBCM_STATE_CONNECTING; 
}

bool tbcm_is_disconnected(tbcm_handle_t client_)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return false;
     }
     return client->state == TBCM_STATE_DISCONNECTED; 
}

tbcm_state_t tbcm_get_state(tbcm_handle_t client_)
{
     tbcm_t *client = (tbcm_t *)client_;
     if (!client) {
          TBC_LOGE("client is NULL");
          return TBCM_STATE_DISCONNECTED;
     }
     return client->state;
}

void tbcm_check_timeout(tbcm_handle_t client_) // Executes an event loop for PubSub client. //loop()==>checkTimeout()
{
     tbcm_t *client = (tbcm_t *)client_;
     if (!client) {
          TBC_LOGE("client is NULL");
          return;
     }

     // Too early
     uint64_t timestamp = (uint64_t)time(NULL);
     if (timestamp < client->last_check_timestamp + TB_MQTT_TIMEOUT + 2) {
          return;
     }
     client->last_check_timestamp = timestamp;

     // move timeout item to timeout_list
     tbcm_request_list_t timeout_list = LIST_HEAD_INITIALIZER(timeout_list);
     int count = _request_list_move_all_of_timeout(client, timestamp, &timeout_list);
     if (count <=0 ) {
          return;
     }

     tbcm_request_t *tbcm_request = NULL, *next;
     LIST_FOREACH_SAFE(tbcm_request, &timeout_list, entry, next) {
          // exec timeout callback
          if (tbcm_request->on_timeout) {
               tbcm_on_timeout_t on_timeout = tbcm_request->on_timeout;
               on_timeout(client->context, tbcm_request->request_id);
          }

          // remove from request list
          LIST_REMOVE(tbcm_request, entry);
          _request_destroy(tbcm_request);
     }
}

/**
 * @brief Client to send a 'Telemetry' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/telemetry'
 *      Data:  '{"key1":"value1", "key2":true, "key3": 3.0, "key4": 4}', '[{"key1":"value1"}, {"key2":true}]'
 *
 * @param telemetry  telemetry. example: {"key1":"value1", "key2":true, "key3": 3.0, "key4": 4}, (字符串要符合 json 数据格式)
 * @param qos        qos of publish message
 * @param retain     ratain flag
 *
 * @return message_id of the subscribe message on success
 *         0 if cannot publish
 *        -1 if error
 */
int tbcm_telemetry_publish(tbcm_handle_t client_, const char *telemetry,
                           int qos /*= 1*/, int retain /*= 0*/) // sendTelemetry()
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[Telemetry][Tx] %.*s", strlen(telemetry), telemetry);
     }

     int message_id = _tbcm_publish(client, TB_MQTT_TOPIC_TELEMETRY_PUBLISH, telemetry, qos, retain);
     return message_id;
}

/**
 * @brief Client to send a 'Attributes' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/attributes'
 *      Data:  '{"attribute1":"value1", "attribute2":true, "attribute3":42.0, "attribute4":73}'
 *
 * @param attributes    attributes. example: {"attribute1":"value1", "attribute2":true, "attribute3":42.0, "attribute4":73} (字符串要符合 json 数据格式)
 * @param qos           qos of publish message
 * @param retain        ratain flag
 *
 * @return message_id of the subscribe message on success
 *         0 if cannot publish
 *        -1 if error
 */
int tbcm_clientattributes_publish(tbcm_handle_t client_, const char *attributes,
                                         int qos /*= 1*/, int retain /*= 0*/) // sendAttributes() //publish client attributes
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[Client-Side Attributes][Tx] %.*s", strlen(attributes), attributes);
     }

     int message_id = _tbcm_publish(client, TB_MQTT_TOPIC_CLIENT_ATTRIBUTES_PUBLISH, attributes, qos, retain);
     return message_id;
}

/**
 * @brief Client to send a 'Attributes Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/attributes/request/$request_id'
 *      Data:  '{"clientKeys":"attribute1,attribute2", "sharedKeys":"shared1,shared2"}'
 *
 * @param payload        payload
 * @param on_attrrequest_response     Attributes response callback
 * @param on_attrrequest_timeout  Attributes response timeout callback
 * @param qos            qos of publish message
 * @param retain         ratain flag
 *
 * @return request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_attributes_request(tbcm_handle_t client_, const char *payload,
                            void *context,
                            tbcm_on_attrrequest_response_t on_attrrequest_response,
                            tbcm_on_attrrequest_timeout_t on_attrrequest_timeout,
                            int qos /*= 1*/, int retain /*= 0*/) // requestAttributes() //request client and shared attributes
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle) {
          TBC_LOGE("mqtt client is NULL");
          return -1;
     }
     if (!payload) {
          TBC_LOGW("There are no payload to request");
          return -1;
     }

     int request_id = _request_list_create_and_append(client, TBCM_REQUEST_ATTR, 0 /*request_id*/, context,
                                           on_attrrequest_response, on_attrrequest_timeout);
     if (request_id <= 0) {
          TBC_LOGE("Unable to take semaphore");
          return -1;
     }

     int size = strlen(TB_MQTT_TOPIC_ATTRIBUTES_REQUEST_PREFIX) + 20;
     char *topic = TBCM_MALLOC(size);
     if (!topic) {
          TBC_LOGE("Unable to malloc memory");
          return -1;
     }
     memset(topic, 0x00, size);
     snprintf(topic, size - 1, TB_MQTT_TOPIC_ATTRIBUTES_REQUEST_PATTERN, request_id);

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[Attributes Request][Tx] RequestID=%d, %.*s",
              request_id, strlen(payload), payload);
     }

     /*int message_id =*/ _tbcm_publish(client, topic, payload, qos, retain);
     TBCM_FREE(topic);
     return request_id; /*return message_id;*/
}

/**
 * @brief Client to send a 'Attributes Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/attributes/request/$request_id'
 *      Data:  '{"clientKeys":"attribute1,attribute2", "sharedKeys":"shared1,shared2"}'
 *
 * @param client_keys   client attribute names. A ending char is '\0'. example: "attribute1,attribute2" (字符串要自带双引号,逗号分隔!!)
 * @param shared_keys   shared attribute names. A ending char is '\0'. example: "shared1,shared2"       (字符串要自带双引号,逗号分隔!!)
 * @param on_attrrequest_response     Attributes response callback
 * @param on_attrrequest_timeout  Attributes response timeout callback
 * @param qos           qos of publish message
 * @param retain        ratain flag
 *
 * @return request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_attributes_request_ex(tbcm_handle_t client_, const char *client_keys, const char *shared_keys,
                               void *context,
                               tbcm_on_attrrequest_response_t on_attrrequest_response,
                               tbcm_on_attrrequest_timeout_t on_attrrequest_timeout,
                               int qos /*= 1*/, int retain /*= 0*/)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     int client_len = 0;
     int shared_len = 0;
     if (client_keys && strlen(client_keys) > 0) {
          client_len = strlen(client_keys);
     }
     if (shared_keys && strlen(shared_keys) > 0) {
          shared_len = strlen(shared_keys);
     }
     if ((client_len>0) && (shared_len>0)) {
          TBC_LOGW("There are no keys to request");
          return -1;
     }

     int size = strlen(TB_MQTT_KEY_ATTRIBUTES_REQUEST_CLIENTKEYS) + client_len 
               + strlen(TB_MQTT_KEY_ATTRIBUTES_REQUEST_SHAREDKEYS) + shared_len + 20;
     char *payload = TBCM_MALLOC(size);
     if (!payload)
     {
          TBC_LOGE("Unable to malloc memory");
          return -1;
     }
     memset(payload, 0x00, size);

     if ((client_len>0) && (shared_len>0)) {
         snprintf(payload, size - 1, "{\"clientKeys\":\"%s\", \"sharedKeys\":\"%s\"}", client_keys, shared_keys);
     } else if (client_len>0) {
         snprintf(payload, size - 1, "{\"clientKeys\":\"%s\"}", client_keys);
     } else if (shared_len>0) {
         snprintf(payload, size - 1, "{\"sharedKeys\":\"%s\"}", shared_keys);
     }
     int retult = tbcm_attributes_request(client, payload,
                                          context,
                                          on_attrrequest_response,
                                          on_attrrequest_timeout,
                                          qos, retain);
     TBCM_FREE(payload);
     return retult;
}

/**
 * @brief Client to send a 'Server-Side RPC Response' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/rpc/response/$request_id'
 *      Data:  '{"example_response":23.1}' ???
 *
 * @param request_id  Server-Side RPC Request ID
 * @param response    payload string (set to NULL, sending empty payload message). A ending char is '\0'.
 * @param qos         qos of publish message
 * @param retain      ratain flag
 *
 * @return message_id of the subscribe message on success
 *         0 if cannot publish
 *        -1 if error
 */
int tbcm_serverrpc_response(tbcm_handle_t client_, int request_id, const char *response,
                                   int qos /*= 1*/, int retain /*= 0*/) // sendServerRpcReply() //response server-side RPC
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }
     
     if (!client->mqtt_handle) {
          TBC_LOGE("MQTT client is NULL!");
          return -1;
     }

     int size = strlen(TB_MQTT_TOPIC_SERVERRPC_RESPONSE_PREFIX) + 20;
     char *topic = TBCM_MALLOC(size);
     if (!topic) {
          TBC_LOGE("Unable to malloc memory!");
          return -1;
     }
     memset(topic, 0x00, size);
     snprintf(topic, size - 1, TB_MQTT_TOPIC_SERVERRPC_RESPONSE_PATTERN, request_id);

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[Server-Side RPC][Tx] RequestID=%d Payload=%.*s",
              request_id, strlen(response), response);
     }

     int message_id = _tbcm_publish(client, topic, response, qos, retain);
     TBCM_FREE(topic);
     return message_id;
}

/**
 * @brief Client to send a 'Client-Side RPC Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/rpc/request/$request_id'
 *      Data:  '{"method":"getTime","params":{}}'
 *
 * @param method        RPC method name.   A ending char is '\0'. example:　"getTime"   (字符串要自带双引号!!)
 * @param params        RPC method params. A ending char is '\0'. example:　{}          (字符串是 json 格式!!)
 * @param on_clientrpc_response      Client-RPC response callback
 * @param on_clientrpc_timeout  Client-RPC response timeout callback
 * @param qos           qos of publish message
 * @param retain        ratain flag
 *
 * @return rpc_request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_clientrpc_request(tbcm_handle_t client_, const char *payload,
                               void *context,
                               tbcm_on_clientrpc_response_t on_clientrpc_response,
                               tbcm_on_clientrpc_timeout_t on_clientrpc_timeout,
                               int qos /*= 1*/, int retain /*= 0*/) // sendClientRpcCall() //request client-side RPC
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle) {
          TBC_LOGE("mqtt client is NULL!");
          return -1;
     }
     if (!payload) {
          TBC_LOGW("There are no payload to request!");
          return -1;
     }

     int request_id = _request_list_create_and_append(client, TBCM_REQUEST_CLIENTRPC, 0/*request_id*/, context,
                                           on_clientrpc_response, on_clientrpc_timeout);
     if (request_id <= 0) {
          TBC_LOGE("Unable to take semaphore");
          return -1;
     }

     int size = strlen(TB_MQTT_TOPIC_CLIENTRPC_REQUEST_PREFIX) + 20;
     char *topic = TBCM_MALLOC(size);
     if (!topic) {
          TBC_LOGE("Unable to malloc memory");
          return -1;
     }
     memset(topic, 0x00, size);
     snprintf(topic, size - 1, TB_MQTT_TOPIC_CLIENTRPC_REQUEST_PATTERN, request_id);

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[Client-Side RPC][Tx] RequestID=%d %.*s",
              request_id, strlen(payload), payload);
     }

     /*int message_id =*/ _tbcm_publish(client, topic, payload, qos, retain);
     TBCM_FREE(topic);
     return request_id; /*return message_id;*/
}

/**
 * @brief Client to send a 'Client-Side RPC Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/rpc/request/$request_id'
 *      Data:  '{"method":"getTime","params":{}}'
 *
 * @param method        RPC method name.   A ending char is '\0'. example:　"getTime"   (字符串要自带双引号!!)
 * @param params        RPC method params. A ending char is '\0'. example:　{}          (字符串是 json 格式!!)
 * @param on_clientrpc_response      Client-RPC response callback
 * @param on_clientrpc_timeout  Client-RPC response timeout callback
 * @param qos           qos of publish message
 * @param retain        ratain flag
 *
 * @return rpc_request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_clientrpc_request_ex(tbcm_handle_t client_, const char *method, const char *params,
                                     void *context,
                                     tbcm_on_clientrpc_response_t on_clientrpc_response,
                                     tbcm_on_clientrpc_timeout_t on_clientrpc_timeout,
                                     int qos /*= 1*/, int retain /*= 0*/)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     int size = strlen(TB_MQTT_KEY_RPC_METHOD) + strlen(method) + strlen(TB_MQTT_KEY_RPC_PARAMS) + strlen(params) + 20;
     char *payload = TBCM_MALLOC(size);
     if (!payload) {
          TBC_LOGE("Unable to malloc memory");
          return -1;
     }
     memset(payload, 0x00, size);
     snprintf(payload, size - 1, "{\"method\":\"%s\",\"params\":%s}", method, params); //{%s}
     int retult = tbcm_clientrpc_request(client, payload,
                                         context,
                                         on_clientrpc_response,
                                         on_clientrpc_timeout,
                                         qos, retain);
     TBCM_FREE(payload);
     return retult;
}
 
 /**
  * @brief Client to send a 'claiming_device_using_device_side_key' publish message to the broker
  *
  * Notes:
  * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
  * - A ThingsBoard MQTT Protocol message example:
  *      Topic: 'v1/devices/me/claim'
  *      Data:  '{"secretKey":"value", "durationMs":60000}'
  *
  * @param claiming   example: {"secretKey":"value", "durationMs":60000}, (字符串要符合 json 数据格式)
  * @param qos        qos of publish message
  * @param retain     ratain flag
  *
  * @return message_id of the subscribe message on success
  *         0 if cannot publish
  *        -1 if error
  */
 int tbcm_claiming_device_publish(tbcm_handle_t client_, const char *claiming,
                            int qos /*= 1*/, int retain /*= 0*/)
 {
      tbcm_t *client = (tbcm_t*)client_;
      if (!client) {
           TBC_LOGE("client is NULL!");
           return -1;
      }
 
      if (client->config.log_rxtx_package) {
         TBC_LOGI("[Claiming][Tx] %.*s", strlen(claiming), claiming);
      }
 
      int message_id = _tbcm_publish(client, TB_MQTT_TOPIC_CLAIMING_DEVICE, claiming, qos, retain);
      return message_id;
 }

/**
 * @brief Client to send a 'Provisoin Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic:   '/provision/request'
 *      payload: '{"deviceName": "DEVICE_NAME", "provisionDeviceKey": "u7piawkboq8v32dmcmpp", "provisionDeviceSecret": "jpmwdn8ptlswmf4m29bw"}'
 *
 * @param payload           
 * @param on_provision_response    f/w update response callback
 * @param on_provision_timeout     f/w update response timeout callback
 * @param qos            qos of publish message
 * @param retain         ratain flag
 *
 * @return rpc_request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_provision_request(tbcm_handle_t client_, const char *payload,
                          void *context,
                          tbcm_on_provision_response_t on_provision_response,
                          tbcm_on_provision_timeout_t on_provision_timeout,
                          int qos /*= 1*/, int retain /*= 0*/)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle) {
          TBC_LOGE("mqtt client is NULL");
          return -1;
     }
     if (!payload) {
          TBC_LOGW("There are no payload to request");
          return -1;
     }

     int request_id = _request_list_create_and_append(client, TBCM_REQUEST_PROVISION, -1, context,
                                           on_provision_response, on_provision_timeout);
     if (request_id <= 0) {
          TBC_LOGE("Unable to take semaphore");
          return -1;
     }

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[FW update][Tx] RequestID=%d %.*s",
              request_id, strlen(payload), payload);
     }

     /*int message_id =*/ _tbcm_publish(client, TB_MQTT_TOPIC_PROVISION_REQUESTC, payload, qos, retain);
     return request_id; /*return message_id;*/
}

/**
 * @brief Client to send a 'Client-Side RPC Request' publish message to the broker
 *
 * Notes:
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 * - A ThingsBoard MQTT Protocol message example:
 *      Topic: 'v1/devices/me/rpc/request/$request_id'
 *      Data:  '{"method":"getTime","params":{}}'
 *
 * @param request_id_   0 on first f/w request(chunk_id is 0), otherwise if it is result of last tbcm_otaupdate_request()
 * @param chunk_id      0,1,2,3,...           
 * @param on_otaupdate_response    f/w update response callback
 * @param on_otaupdate_timeout     f/w update response timeout callback
 * @param qos            qos of publish message
 * @param retain         ratain flag
 *
 * @return rpc_request_id of the subscribe message on success
 *        -1 if error
 */
int tbcm_otaupdate_request(tbcm_handle_t client_,
                          int request_id_, int chunk_id, const char *payload, //?payload
                          void *context,
                          tbcm_on_otaupdate_response_t on_otaupdate_response,
                          tbcm_on_otaupdate_timeout_t on_otaupdate_timeout,
                          int qos /*= 1*/, int retain /*= 0*/)
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle) {
          TBC_LOGE("mqtt client is NULL");
          return -1;
     }
     /*if (!payload) {
          TBC_LOGW("There are no payload to request");
          return -1;
     }*/

     int request_id = _request_list_create_and_append(client, TBCM_REQUEST_FWUPDATE, request_id_, context,
                                           on_otaupdate_response, on_otaupdate_timeout);
     if (request_id <= 0) {
          TBC_LOGE("Unable to take semaphore");
          return -1;
     }

     int size = strlen(TB_MQTT_TOPIC_FW_REQUEST_PATTERN) + 20;
     char *topic = TBCM_MALLOC(size);
     if (!topic) {
          TBC_LOGE("Unable to malloc memory");
          return -1;
     }
     memset(topic, 0x00, size);
     snprintf(topic, size - 1, TB_MQTT_TOPIC_FW_REQUEST_PATTERN, request_id, chunk_id);

     if (client->config.log_rxtx_package) {
        TBC_LOGI("[FW update][Tx] RequestID=%d %.*s",
              request_id, strlen(payload), payload);
     }

     /*int message_id =*/ _tbcm_publish(client, topic, payload, qos, retain);
     TBCM_FREE(topic);
     return request_id; /*return message_id;*/
}

/**
 * @brief Subscribe the client to defined topic with defined qos
 *
 * Notes:
 * - Client must be connected to send subscribe message
 * - This API is could be executed from a user task or
 * from a mqtt event callback i.e. internal mqtt task
 * (API is protected by internal mutex, so it might block
 * if a longer data receive operation is in progress.
 *
 * @param client    mqtt client handle
 * @param topic
 * @param qos
 *
 * @return message_id of the subscribe message on success
 *         -1 on failure
 */
/*static*/ int _tbcm_subscribe(tbcm_handle_t client_, const char *topic, int qos /*=0*/) // subscribe()
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle || !topic)
     {
          return -1;
     }

     return esp_mqtt_client_subscribe(client->mqtt_handle, topic, qos);
}

/**
 * @brief Client to send a publish message to the broker
 *
 * Notes:
 * - Client doesn't have to be connected to send publish message
 *   (although it would drop all qos=0 messages, qos>1 messages would be enqueued)
 * - It is thread safe, please refer to `esp_mqtt_client_subscribe` for details
 *
 * @param topic     topic string
 * @param payload   payload string (set to NULL, sending empty payload message)
 * @param qos       qos of publish message
 * @param retain    ratain flag
 *
 * @return message_id of the subscribe message on success
 *         0 if cannot publish
 *        -1 if error
 */
static int _tbcm_publish(tbcm_handle_t client_, const char *topic, const char *payload, int qos /*= 1*/, int retain /*= 0*/) // publish()
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     if (!client->mqtt_handle || !topic)
     {
          return -1;
     }

     int len = (payload == NULL) ? 0 : strlen(payload); //// +1
     return esp_mqtt_client_publish(client->mqtt_handle, topic, payload, len, qos, retain); ////return msg_id or -1(failure)
}

// The callback for when a MQTT event is received.
static void _on_MqttEventHandle(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
     tbcm_t *client = (tbcm_t*)handler_args;
     esp_mqtt_event_handle_t event = event_data;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return;// -1;
     }
     if (!event) {
          TBC_LOGE("event is NULL!");
          return;// -1;
     }

     int msg_id;
     switch (event->event_id) {
     case MQTT_EVENT_BEFORE_CONNECT:
          TBC_LOGI("MQTT_EVENT_BEFORE_CONNECT, msg_id=%d, topic_len=%d, data_len=%d",
              event->msg_id, event->topic_len, event->data_len);
          break;

     case MQTT_EVENT_CONNECTED:
          TBC_LOGI("MQTT_EVENT_CONNECTED");
          TBC_LOGI("client->mqtt_handle = %p", client->mqtt_handle);
          msg_id = _tbcm_subscribe(client, TB_MQTT_TOPIC_SHARED_ATTRIBUTES, 0);
          TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s", msg_id, TB_MQTT_TOPIC_SHARED_ATTRIBUTES);
          msg_id = _tbcm_subscribe(client, TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_SUBSCRIRBE, 0);
          TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s", msg_id, TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_SUBSCRIRBE);
          msg_id = _tbcm_subscribe(client, TB_MQTT_TOPIC_SERVERRPC_REQUEST_SUBSCRIBE, 0);
          TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s", msg_id, TB_MQTT_TOPIC_SERVERRPC_REQUEST_SUBSCRIBE);
          msg_id = _tbcm_subscribe(client, TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_SUBSCRIBE, 0);
          TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s", msg_id, TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_SUBSCRIBE);
          msg_id = _tbcm_subscribe(client, TB_MQTT_TOPIC_FW_RESPONSE_SUBSCRIBE, 0);
          TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s", msg_id, TB_MQTT_TOPIC_FW_RESPONSE_SUBSCRIBE);
          client->state = TBCM_STATE_CONNECTED;
          TBC_LOGI("before call on_connected()...");
          ////if(xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) == pdTRUE) {
          if (client->on_connected) {
               client->on_connected(client->context);
          }
          ////xSemaphoreGive(client->lock);
          ////}
          TBC_LOGI("after call on_connected()");
          break;

     case MQTT_EVENT_DISCONNECTED:
          TBC_LOGI("MQTT_EVENT_DISCONNECTED");
          client->state = TBCM_STATE_DISCONNECTED;
          ////if(xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) == pdTRUE) {
          if (client->on_disconnected) {
               client->on_disconnected(client->context);
          }
          ////xSemaphoreGive(client->lock);
          ////}
          break;

     case MQTT_EVENT_SUBSCRIBED:
          TBC_LOGI("MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
          break;
     case MQTT_EVENT_UNSUBSCRIBED:
          TBC_LOGI("MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
          break;
     case MQTT_EVENT_PUBLISHED:
          TBC_LOGI("MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
          break;

     case MQTT_EVENT_DATA:
          TBC_LOGI("MQTT_EVENT_DATA");
          ////TBC_LOGI("TOPIC=%.*s", event->topic_len, event->topic);
          ////TBC_LOGI("DATA=%.*s", event->data_len, event->data);
          _on_DataEventProcess(client, event);
          break;

     case MQTT_EVENT_ERROR:
          TBC_LOGW("MQTT_EVENT_ERROR");
          break;
     default:
          TBC_LOGW("Other event id:%d", event->event_id);
          break;
     }
     return;// ESP_OK;
}

// Processes MQTT_EVENT_DATA message
static void _on_DataEventProcess(tbcm_handle_t client_, esp_mqtt_event_handle_t event) //_onDataEventProcess(); //MQTT_EVENT_DATA
{

     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return;
     }
     if (!event) {
          TBC_LOGE("!event is NULL!");
          return;
     }

     // If payload may be into multiple packets, then multiple packages need to be merged, eg: F/W OTA!
     tbcm_rx_msg_info rx_msg = {0};
     rx_msg.topic = event->topic;          /*!< Topic associated with this event */
     rx_msg.payload = event->data;         /*!< Data associated with this event */
     rx_msg.topic_len = event->topic_len;  /*!< Length of the topic for this event associated with this event */
     rx_msg.payload_len = event->data_len;                       /*!< Length of the data for this event */
     rx_msg.total_payload_len = event->total_data_len;           /*!< Total length of the data (longer data are supplied with multiple events) */
     rx_msg.current_payload_offset = event->current_data_offset; /*!< Actual offset for the data associated with this event */
     tbcm_payload_buffer_pocess(&client->buffer, &rx_msg, _on_PayloadProcess, client_);
}

static void _on_PayloadProcess(void *context/*client*/, tbcm_rx_msg_info* rx_msg)
{
    tbcm_t *client = (tbcm_t*)context/*client*/;
    if (!client) {
         TBC_LOGE("client is NULL!");
         return;
    }

    const char *topic = rx_msg->topic;          /*!< Topic associated with this event */
    const char *payload = rx_msg->payload;      /*!< Data associated with this event */
    const int topic_len = rx_msg->topic_len;    /*!< Length of the topic for this event associated with this event */
    const int payload_len = rx_msg->payload_len;                       /*!< Length of the data for this event */
    const int total_payload_len = rx_msg->total_payload_len;           /*!< Total length of the data (longer data are supplied with multiple events) */
    const int current_payload_offset = rx_msg->current_payload_offset; /*!< Actual offset for the data associated with this event */

    if (strncmp(topic, TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_PREFIX, strlen(TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_PREFIX)) == 0) {
         // 3.TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_PREFIX

         char temp[32] = {0};
         strncpy(temp, topic+strlen(TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_PREFIX), topic_len-strlen(TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_PREFIX));
         int request_id = atoi(temp);
         if (client->config.log_rxtx_package) {
             TBC_LOGI("[Attributes Request][Rx] RequestID=%d %.*s",
                  request_id, payload_len, payload);
         }
    
         tbcm_request_t *tbcm_request = _request_list_search_and_remove(client, request_id);
         if (tbcm_request) {
              if (tbcm_request->on_response) {
                   tbcm_on_response_t on_response = tbcm_request->on_response;
                   on_response(client->context, request_id, payload, payload_len);
              }
              _request_destroy(tbcm_request);
              tbcm_request = NULL;
         } else {
              TBC_LOGE("Unable to find attributes requset(%d), (%.*s, %.*s)", request_id,
                      topic_len, topic, payload_len, payload);
              return; // -1;
         }
    
    } else if (strncmp(topic, TB_MQTT_TOPIC_SHARED_ATTRIBUTES, strlen(TB_MQTT_TOPIC_SHARED_ATTRIBUTES)) == 0) {
                                            //if (strcmp(topic, TB_MQTT_TOPIC_SHARED_ATTRIBUTES) == 0) {
         // 1.TB_MQTT_TOPIC_SHARED_ATTRIBUTES
    
         if (client->config.log_rxtx_package) {
             TBC_LOGI("[Subscribe Shared Attributes][Rx] %.*s", payload_len, payload);
         }
    
         if (client->on_sharedattr_received) {
              client->on_sharedattr_received(client->context, payload, payload_len);
         } else {
              TBC_LOGW("Unable to find shared-attributes, (%.*s, %.*s)",
                      topic_len, topic, payload_len, payload);
         }
    
    } else if (strncmp(topic, TB_MQTT_TOPIC_SERVERRPC_REQUEST_PREFIX, strlen(TB_MQTT_TOPIC_SERVERRPC_REQUEST_PREFIX)) == 0) {
         // 2.TB_MQTT_TOPIC_SERVERRPC_REQUEST_PREFIX

         char temp[32] = {0};
         strncpy(temp, topic+strlen(TB_MQTT_TOPIC_SERVERRPC_REQUEST_PREFIX), topic_len-strlen(TB_MQTT_TOPIC_SERVERRPC_REQUEST_PREFIX));
         int request_id = atoi(temp);
         if (client->config.log_rxtx_package) {
             TBC_LOGI("[Server-Side RPC][Rx] RequestID=%d Payload=%.*s",
                  request_id, payload_len, payload);
         }
    
         if (client->on_serverrpc_request) {
              client->on_serverrpc_request(client->context, request_id, payload, payload_len);
         } else {
              TBC_LOGW("Unable to find server-rpc request(%d), (%.*s, %.*s)", request_id,
                      topic_len, topic, payload_len, payload);
         }
    
    } else if (strncmp(topic, TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_PREFIX, strlen(TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_PREFIX)) == 0) {
         // 4.TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_PREFIX
    
         char temp[32] = {0};
         strncpy(temp, topic+strlen(TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_PREFIX), topic_len-strlen(TB_MQTT_TOPIC_CLIENTRPC_RESPONSE_PREFIX));
         int request_id = atoi(temp);
         if (client->config.log_rxtx_package) {
             TBC_LOGI("[Client-Side RPC][Rx] RequestID=%d %.*s",
                  request_id, payload_len, payload);
         }
    
         tbcm_request_t *tbcm_request = _request_list_search_and_remove(client, request_id);
         if (tbcm_request) {
              if (tbcm_request->on_response) {
                   tbcm_on_response_t on_response = tbcm_request->on_response;
                   on_response(client->context, request_id, payload, payload_len);
              }
              _request_destroy(tbcm_request);
              tbcm_request = NULL;
         } else {
              TBC_LOGE("Unable to find client-RPC requset(%d), (%.*s, %.*s)", request_id,
                      topic_len, topic, payload_len, payload);
              return; // -1;
         }
    
    } else if (strncmp(topic, TB_MQTT_TOPIC_FW_RESPONSE_PREFIX, strlen(TB_MQTT_TOPIC_FW_RESPONSE_PREFIX)) == 0) {
         // 5.TB_MQTT_TOPIC_FW_RESPONSE_PREFIX
         int request_id = 0;
         sscanf(topic, TB_MQTT_TOPIC_FW_RESPONSE_PATTERN, &request_id);

         int chunk_id = -1;
         const char *chunk_str = strstr(topic, "/chunk/");
         if (chunk_str) {
             char temp[32] = {0};
             int offset = (uint32_t)chunk_str - (uint32_t)topic;
             strncpy(temp, topic+offset+strlen("/chunk/"), topic_len-offset-strlen("/chunk/"));
             chunk_id = atoi(temp);
         }

         if (client->config.log_rxtx_package) {
             TBC_LOGI("[FW update][Rx] RequestID=%d payload_len=%d",
                   request_id, payload_len);
         }
    
         tbcm_request_t *tbcm_request = _request_list_search_and_remove(client, request_id);
         if (tbcm_request) {
              tbcm_on_otaupdate_response_t on_otaupdate_response = tbcm_request->on_response;
              if (on_otaupdate_response) {
                   on_otaupdate_response(client->context, request_id, chunk_id, payload, payload_len);
              }
              _request_destroy(tbcm_request);
              tbcm_request = NULL;
         } else {
              TBC_LOGE("Unable to find FW update requset(%d), (%.*s, %.*s)",
                        request_id,
                        topic_len, topic, payload_len, payload);
              return; // -1;
         }
    
    } else if (strncmp(topic, TB_MQTT_TOPIC_PROVISION_RESPONSE, strlen(TB_MQTT_TOPIC_PROVISION_RESPONSE)) == 0) {
         // 6.TB_MQTT_TOPIC_PROVISION_RESPONSE
         if (client->config.log_rxtx_package) {
             TBC_LOGI("[Provision][Rx] request_type=%d, payload_len=%d",
                   TBCM_REQUEST_PROVISION, payload_len);
         }

         tbcm_request_t *tbcm_request = _request_list_search_and_remove_by_type(client, TBCM_REQUEST_PROVISION);
         if (tbcm_request) {
              tbcm_on_provision_response_t on_provision_response = tbcm_request->on_response;
              if (on_provision_response) {
                   on_provision_response(client->context, tbcm_request->request_id, payload, payload_len);
              }
              _request_destroy(tbcm_request);
              tbcm_request = NULL;
         } else {
              TBC_LOGE("Unable to find Provision request_type(%d), (%.*s, %.*s)",
                        TBCM_REQUEST_PROVISION,
                        topic_len, topic, payload_len, payload);
              return; // -1;
         }
    
    }  else {
         // Payload is too long, then Serial.*/
         TBC_LOGW("[Unkown-Msg][Rx] topic=%.*s, payload=%.*s, payload_len=%d, total_payload_len=%d, current_payload_offset=%d",
                   topic_len, topic, payload_len, payload, payload_len, total_payload_len, current_payload_offset);
    }

}

/*static*/ bool _request_is_equal(const tbcm_request_t *a, const tbcm_request_t *b)
{
     if (!a & !b) {
          return true;
     }
     if (!a || !b) {
          return false;
     }
     if (a->type != b->type) {
          return false;
     } else if (a->request_id != b->request_id) {
          return false;
     } else { // if (a->timestamp != b->timestamp)
          return a->timestamp == b->timestamp;
     }
}

//return request_id on successful, otherwise return -1
static int _request_list_create_and_append(tbcm_handle_t client_, tbcm_request_type_t type, int request_id,
                                           void *context,
                                           void *on_response, /*tbcm_on_response_t*/
                                           void *on_timeout) /*tbcm_on_timeout_t*/
{
     tbcm_t *client = (tbcm_t*)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return -1;
     }

     // Take semaphore
     if (xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore!");
          return -1;
     }

     // Get request ID
     if (request_id <= 0) {
          do {
               if (client->next_request_id <= 0)
                    client->next_request_id = 1;
               else
                    client->next_request_id++;
          } while (client->next_request_id <= 0);

          request_id = client->next_request_id;
     }

     // If no response & no timeout, then it doesn't append to request list!
     if (!on_response && !on_timeout) {
          // Give semaphore
          xSemaphoreGive(client->lock);
          TBC_LOGI("Don't append to request list if no response & no timeout!");
          return request_id;
     }

     // Create request
     tbcm_request_t *tbcm_request = _request_create(type, request_id, context, on_response, on_timeout);
     if (!tbcm_request) {
          // Give semaphore
          xSemaphoreGive(client->lock);
          TBC_LOGE("Unable to create request: No memory!");
          return -1;
     }

     // Insert request to list
     tbcm_request_t *it, *last = NULL;
     if (LIST_FIRST(&client->request_list) == NULL) {
          // Insert head
          LIST_INSERT_HEAD(&client->request_list, tbcm_request, entry);
     } else {
          // Insert last
          LIST_FOREACH(it, &client->request_list, entry) {
               last = it;
          }
          if (it == NULL) {
               assert(last);
               LIST_INSERT_AFTER(last, tbcm_request, entry);
          }
     }

     // Give semaphore
     xSemaphoreGive(client->lock);
     return request_id;
}

static tbcm_request_t *_request_list_search_and_remove(tbcm_handle_t client_, int request_id)
{
     tbcm_t *client = (tbcm_t *)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return NULL;
     }

     // Take semaphore
     if (xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore!");
          return NULL;
     }

     // Search item
     tbcm_request_t *tbcm_request = NULL;
     LIST_FOREACH(tbcm_request, &client->request_list, entry) {
          if (tbcm_request && tbcm_request->request_id == request_id) {
               break;
          }
     }

     /// Remove form list
     if (tbcm_request) {
          LIST_REMOVE(tbcm_request, entry);
     } else {
          TBC_LOGW("Unable to remove request:%d!", request_id);
     }

     // Give semaphore
     xSemaphoreGive(client->lock);
     return tbcm_request;
}


static tbcm_request_t *_request_list_search_and_remove_by_type(tbcm_handle_t client_, tbcm_request_type_t type)
{
     tbcm_t *client = (tbcm_t *)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return NULL;
     }

     // Take semaphore
     if (xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore!");
          return NULL;
     }

     // Search item
     tbcm_request_t *tbcm_request = NULL;
     LIST_FOREACH(tbcm_request, &client->request_list, entry) {
          if (tbcm_request && tbcm_request->type == type) {
               break;
          }
     }

     /// Remove form list
     if (tbcm_request) {
          LIST_REMOVE(tbcm_request, entry);
     } else {
          TBC_LOGW("Unable to remove request: type=%d!", type);
     }

     // Give semaphore
     xSemaphoreGive(client->lock);
     return tbcm_request;
}

// return  count of timeout_request_list
static int _request_list_move_all_of_timeout(tbcm_handle_t client_, uint64_t timestamp,
                              tbcm_request_list_t *timeout_request_list)
{
     tbcm_t *client = (tbcm_t *)client_;
     if (!client) {
          TBC_LOGE("client is NULL!");
          return 0;
     }
     if (!timeout_request_list) {
          TBC_LOGE("timeout_request_list is NULL!");
          return 0;
     }

     // Take semaphore
     if (xSemaphoreTake(client->lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore!");
          return 0;
     }

     // Search & move item
     int count = 0;
     tbcm_request_t *tbcm_request = NULL, *next;
     LIST_FOREACH_SAFE(tbcm_request, &client->request_list, entry, next) {
          if (tbcm_request && tbcm_request->timestamp + TB_MQTT_TIMEOUT <= timestamp) {
               // remove from request list
               LIST_REMOVE(tbcm_request, entry);

               // append to timeout list
               tbcm_request_t *it, *last = NULL;
               if (LIST_FIRST(timeout_request_list) == NULL) {
                    LIST_INSERT_HEAD(timeout_request_list, tbcm_request, entry);
                    count++;
               } else {
                    LIST_FOREACH(it, timeout_request_list, entry) {
                         last = it;
                    }
                    if (it == NULL) {
                         assert(last);
                         LIST_INSERT_AFTER(last, tbcm_request, entry);
                         count++;
                    }
               }
          }
     }

     // Give semaphore
     xSemaphoreGive(client->lock);
     return count;
}

//return tbcm_request on successful, otherwise return NULL.
static tbcm_request_t *_request_create(tbcm_request_type_t type,
                                       uint32_t request_id,
                                       void *context,
                                       void *on_response, /*tbcm_on_response_t*/
                                       void *on_timeout) /*tbcm_on_timeout_t*/
{
     tbcm_request_t *tbcm_request = TBCM_MALLOC(sizeof(tbcm_request_t));
     if (!tbcm_request) {
          TBC_LOGE("Unable to malloc memory!");
          return NULL;
     }

     memset(tbcm_request, 0x00, sizeof(tbcm_request_t));
     tbcm_request->type = type;
     tbcm_request->request_id = request_id;
     tbcm_request->timestamp = (uint64_t)time(NULL);
     tbcm_request->context = context;
     tbcm_request->on_response = on_response;
     tbcm_request->on_timeout = on_timeout;

     return tbcm_request;
}

static void _request_destroy(tbcm_request_t *tbcm_request)
{
     if (!tbcm_request) {
          TBC_LOGE("Invalid argument!");
          return; 
     }

     tbcm_request->type = 0;
     tbcm_request->request_id = 0;
     tbcm_request->timestamp = 0;
     tbcm_request->context = NULL;
     tbcm_request->on_response = NULL;
     tbcm_request->on_timeout = NULL;
     TBCM_FREE(tbcm_request);
}

