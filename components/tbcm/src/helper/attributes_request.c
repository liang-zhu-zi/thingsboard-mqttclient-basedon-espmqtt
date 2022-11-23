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

#include <string.h>

#include "esp_err.h"

#include "tbc_utils.h"

//#include "attributes_request.h"
#include "tbc_mqtt_helper_internal.h"

const static char *TAG = "attributes_request";

/*!< Initialize attributes_request */
static attributes_request_t *_attributes_request_create(tbcmh_handle_t client, int request_id, void *context,
                                                         tbcmh_attributesrequest_on_response_t on_response,
                                                         tbcmh_attributesrequest_on_timeout_t on_timeout)
{
    if (!on_response) {
        TBC_LOGE("on_response is NULL");
        return NULL;
    }
    
    attributes_request_t *attributesrequest = TBC_MALLOC(sizeof(attributes_request_t));
    if (!attributesrequest) {
        TBC_LOGE("Unable to malloc memeory!");
        return NULL;
    }

    memset(attributesrequest, 0x00, sizeof(attributes_request_t));
    attributesrequest->client = client;
    attributesrequest->request_id = request_id;
    attributesrequest->context = context;
    attributesrequest->on_response = on_response;
    attributesrequest->on_timeout = on_timeout;
    return attributesrequest;
}

static attributes_request_t *_attributes_request_clone_wo_listentry(attributes_request_t *src)
{
    if (!src) {
        TBC_LOGE("src is NULL");
        return NULL;
    }
    
    attributes_request_t *attributesrequest = TBC_MALLOC(sizeof(attributes_request_t));
    if (!attributesrequest) {
        TBC_LOGE("Unable to malloc memeory!");
        return NULL;
    }

    memset(attributesrequest, 0x00, sizeof(attributes_request_t));
    attributesrequest->client = src->client;
    attributesrequest->request_id = src->request_id;
    attributesrequest->context = src->context;
    attributesrequest->on_response = src->on_response;
    attributesrequest->on_timeout = src->on_timeout;
    return attributesrequest;
}

/*!< Destroys the attributes_request */
static tbc_err_t _attributes_request_destroy(attributes_request_t *attributesrequest)
{
    if (!attributesrequest) {
        TBC_LOGE("attributesrequest is NULL");
        return ESP_FAIL;
    }

    TBC_FREE(attributesrequest);
    return ESP_OK;
}

//==== Request client-side or shared device attributes from the server ================================
#define MAX_KEYS_LEN (256)

////return request_id on successful, otherwise return -1/ESP_FAIL
int tbcmh_attributesrequest_send(tbcmh_handle_t client,
                                 void *context,
                                 tbcmh_attributesrequest_on_response_t on_response,
                                 tbcmh_attributesrequest_on_timeout_t on_timeout,
                                 int count, /*const char *key,*/...)
{
     if (!client) {
          TBC_LOGE("client is NULL! %s()", __FUNCTION__);
          return ESP_FAIL;
     }
     if (count <= 0) {
          TBC_LOGE("count(%d) is error! %s()", count, __FUNCTION__);
          return ESP_FAIL;
     }

     // Take semaphore, malloc client_keys & shared_keys
     if (xSemaphoreTake(client->_lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore! %s()", __FUNCTION__);
          return ESP_FAIL;
     }
     char *client_keys = TBC_MALLOC(MAX_KEYS_LEN);
     char *shared_keys = TBC_MALLOC(MAX_KEYS_LEN);
     if (!client_keys || !shared_keys) {
          goto attributesrequest_fail;
     }
     memset(client_keys, 0x00, MAX_KEYS_LEN);
     memset(shared_keys, 0x00, MAX_KEYS_LEN);

     // Get client_keys & shared_keys
     int i = 0;
     va_list ap;
     va_start(ap, count);
next_attribute_key:
     while (i<count) {
          i++;
          const char *key = va_arg(ap, const char*);

          // Search item in clientattribute
          client_attribute_t *clientattribute = NULL;
          LIST_FOREACH(clientattribute, &client->clientattribute_list, entry) {
               if (clientattribute && strcmp(clientattribute->key, key)==0) {
                    // copy key to client_keys
                    if (strlen(client_keys)==0) {
                         strncpy(client_keys, key, MAX_KEYS_LEN-1);
                    } else {
                         strncat(client_keys, ",", MAX_KEYS_LEN-1);                         
                         strncat(client_keys, key, MAX_KEYS_LEN-1);
                    }
                    goto next_attribute_key;
               }
          }

          // Search item in sharedattribute
          shared_attribute_t *sharedattribute = NULL;
          LIST_FOREACH(sharedattribute, &client->sharedattribute_list, entry) {
               if (sharedattribute && strcmp(sharedattribute->key, key)==0) {
                    // copy key to shared_keys
                    if (strlen(shared_keys)==0) {
                         strncpy(shared_keys, key, MAX_KEYS_LEN-1);
                    } else {
                         strncat(shared_keys, ",", MAX_KEYS_LEN-1);                         
                         strncat(shared_keys, key, MAX_KEYS_LEN-1);
                    }
                    goto next_attribute_key;
               }
          }

          TBC_LOGW("Unable to find attribute in request:%s! %s()", key, __FUNCTION__);
     }
     va_end(ap);

     // Send msg to server
     int request_id = _request_list_create_and_append(client, TBCMH_REQUEST_ATTRIBUTES, 0/*request_id*/);
     if (request_id <= 0) {
          TBC_LOGE("Unable to take semaphore");
          return -1;
     }
     int msg_id = tbcm_attributes_request_ex(client->tbmqttclient, client_keys, shared_keys,
                               request_id,
                               1/*qos*/, 0/*retain*/);
     if (msg_id<0) {
          TBC_LOGE("Init tbcm_attributes_request failure! %s()", __FUNCTION__);
          goto attributesrequest_fail;
     }

     // Create attributesrequest
     attributes_request_t *attributesrequest = _attributes_request_create(client, request_id, context, on_response, on_timeout);
     if (!attributesrequest) {
          TBC_LOGE("Init attributesrequest failure! %s()", __FUNCTION__);
          goto attributesrequest_fail;
     }

     // Insert attributesrequest to list
     attributes_request_t *it, *last = NULL;
     if (LIST_FIRST(&client->attributesrequest_list) == NULL) {
          // Insert head
          LIST_INSERT_HEAD(&client->attributesrequest_list, attributesrequest, entry);
     } else {
          // Insert last
          LIST_FOREACH(it, &client->attributesrequest_list, entry) {
               last = it;
          }
          if (it == NULL) {
               assert(last);
               LIST_INSERT_AFTER(last, attributesrequest, entry);
          }
     }

     // Give semaphore
     xSemaphoreGive(client->_lock);
     TBC_FREE(client_keys);
     TBC_FREE(shared_keys);
     return request_id;

attributesrequest_fail:
     xSemaphoreGive(client->_lock);
     if (!client_keys) {
          TBC_FREE(client_keys);
     }
     if (!shared_keys) {
          TBC_FREE(shared_keys);
     }
     return ESP_FAIL;
}

// TODO: merge to tbcmh_attributesrequest_send()
////return request_id on successful, otherwise return -1/ESP_FAIL
int _tbcmh_attributesrequest_send_4_ota_sharedattributes(tbcmh_handle_t client,
                                  void *context,
                                  tbcmh_attributesrequest_on_response_t on_response,
                                  tbcmh_attributesrequest_on_timeout_t on_timeout,
                                  int count, /*const char *key,*/...)
{
      // this funciton is in client->_lock !

      if (!client) {
           TBC_LOGE("client is NULL! %s()", __FUNCTION__);
           return ESP_FAIL;
      }
      if (count <= 0) {
           TBC_LOGE("count(%d) is error! %s()", count, __FUNCTION__);
           return ESP_FAIL;
      }
 
      // Take semaphore, malloc client_keys & shared_keys
      //if (xSemaphoreTake(client->_lock, (TickType_t)0xFFFFF) != pdTRUE) {
      //     TBC_LOGE("Unable to take semaphore! %s()", __FUNCTION__);
      //     return ESP_FAIL;
      //}
      char *shared_keys = TBC_MALLOC(MAX_KEYS_LEN);
      if (!shared_keys) {
           goto attributesrequest_fail;
      }
      memset(shared_keys, 0x00, MAX_KEYS_LEN);
 
      // Get shared_keys
      int i = 0;
      va_list ap;
      va_start(ap, count);
      while (i<count) {
        i++;
        const char *key = va_arg(ap, const char*);
        if (strlen(key)>0) {
            // copy key to shared_keys
            if (strlen(shared_keys)==0) {
                strncpy(shared_keys, key, MAX_KEYS_LEN-1);
            } else {
                strncat(shared_keys, ",", MAX_KEYS_LEN-1);                         
                strncat(shared_keys, key, MAX_KEYS_LEN-1);
            }
        }
      }
      va_end(ap);
 
      // Send msg to server
      int request_id = _request_list_create_and_append(client, TBCMH_REQUEST_ATTRIBUTES, 0/*request_id*/);
      if (request_id <= 0) {
           TBC_LOGE("Unable to create request");
           goto attributesrequest_fail;
      }
      int msg_id = tbcm_attributes_request_ex(client->tbmqttclient, NULL, shared_keys,
                                request_id,
                                1/*qos*/, 0/*retain*/);
      if (msg_id<0) {
           TBC_LOGE("Init tbcm_attributes_request failure! %s()", __FUNCTION__);
           goto attributesrequest_fail;
      }
 
      // Create attributesrequest
      attributes_request_t *attributesrequest = _attributes_request_create(client, request_id, context, on_response, on_timeout);
      if (!attributesrequest) {
           TBC_LOGE("Init attributesrequest failure! %s()", __FUNCTION__);
           goto attributesrequest_fail;
      }
 
      // Insert attributesrequest to list
      attributes_request_t *it, *last = NULL;
      if (LIST_FIRST(&client->attributesrequest_list) == NULL) {
           // Insert head
           LIST_INSERT_HEAD(&client->attributesrequest_list, attributesrequest, entry);
      } else {
           // Insert last
           LIST_FOREACH(it, &client->attributesrequest_list, entry) {
                last = it;
           }
           if (it == NULL) {
                assert(last);
                LIST_INSERT_AFTER(last, attributesrequest, entry);
           }
      }
 
      // Give semaphore
      //xSemaphoreGive(client->_lock);
      TBC_FREE(shared_keys);
      return request_id;
 
 attributesrequest_fail:
      //xSemaphoreGive(client->_lock);
      if (!shared_keys) {
           TBC_FREE(shared_keys);
      }
      return ESP_FAIL;
}

tbc_err_t _tbcmh_attributesrequest_empty(tbcmh_handle_t client)
{
   if (!client) {
        TBC_LOGE("client is NULL! %s()", __FUNCTION__);
        return ESP_FAIL;
   }

   // TODO: How to add lock??
   // Take semaphore
   // if (xSemaphoreTake(client->_lock, (TickType_t)0xFFFFF) != pdTRUE) {
   //      TBC_LOGE("Unable to take semaphore!");
   //      return ESP_FAIL;
   // }

   // remove all item in attributesrequest_list
   attributes_request_t *attributesrequest = NULL, *next;
   LIST_FOREACH_SAFE(attributesrequest, &client->attributesrequest_list, entry, next) {
        // exec timeout callback
        if (attributesrequest->on_timeout) {
            attributesrequest->on_timeout(attributesrequest->client, attributesrequest->context, attributesrequest->request_id); //(none/resend/destroy/_destroy_all_attributes)?
        }

        // remove from attributesrequest list and destory
        LIST_REMOVE(attributesrequest, entry);
        _attributes_request_destroy(attributesrequest);
   }
   memset(&client->attributesrequest_list, 0x00, sizeof(client->attributesrequest_list));

   // Give semaphore
   // xSemaphoreGive(client->_lock);
   return ESP_OK;
}

void _tbcmh_attributesrequest_on_connected(tbcmh_handle_t client)
{
    // This function is in semaphore/client->_lock!!!

    if (!client) {
         TBC_LOGE("client is NULL! %s()", __FUNCTION__);
         return;
    }

    int msg_id = tbcm_subscribe(client->tbmqttclient, TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_SUBSCRIRBE, 0);
    TBC_LOGI("sent subscribe successful, msg_id=%d, topic=%s",
                msg_id, TB_MQTT_TOPIC_ATTRIBUTES_RESPONSE_SUBSCRIRBE);
}

void _tbcmh_attributesrequest_on_response(tbcmh_handle_t client, int request_id, const cJSON *object)
{
     if (!client || !object) {
          TBC_LOGE("client or object is NULL! %s()", __FUNCTION__);
          return;
     }

     // Remove it from request list
     _request_list_search_and_remove(client, request_id);

     // Take semaphore
     if (xSemaphoreTake(client->_lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore! %s()", __FUNCTION__);
          return;
     }

     // Search attributesrequest
     attributes_request_t *attributesrequest = NULL; 
     LIST_FOREACH(attributesrequest, &client->attributesrequest_list, entry) {
          if (attributesrequest && (attributesrequest->request_id==request_id)) {
               break;
          }
     }
     if (!attributesrequest) {
          // Give semaphore
          xSemaphoreGive(client->_lock);
          TBC_LOGW("Unable to find attribute request:%d! %s()", request_id, __FUNCTION__);
          return;
     }

     // Cache and remove attributesrequest
     attributes_request_t *cache = _attributes_request_clone_wo_listentry(attributesrequest);
     LIST_REMOVE(attributesrequest, entry);
     _attributes_request_destroy(attributesrequest);
     // Give semaphore
     xSemaphoreGive(client->_lock);

     // foreach item to set value of clientattribute in lock/unlodk.  Don't call tbcmh's funciton in set value callback!
     if (cJSON_HasObjectItem(object, TB_MQTT_KEY_ATTRIBUTES_RESPONSE_CLIENT)) {
          _tbcmh_clientattribute_on_received(client, cJSON_GetObjectItem(object, TB_MQTT_KEY_ATTRIBUTES_RESPONSE_CLIENT));
     }
     // foreach item to set value of sharedattribute in lock/unlodk.  Don't call tbcmh's funciton in set value callback!
     if (cJSON_HasObjectItem(object, TB_MQTT_KEY_ATTRIBUTES_RESPONSE_SHARED)) {
          _tbcmh_sharedattribute_on_received(client, cJSON_GetObjectItem(object, TB_MQTT_KEY_ATTRIBUTES_RESPONSE_SHARED));
     }

     // Do response
     if (cache->on_response) {
        cache->on_response(cache->client, cache->context, cache->request_id); //(none/resend/destroy/_destroy_all_attributes)?
     }

     // Free cache
     _attributes_request_destroy(cache);

     return;
}

void _tbcmh_attributesrequest_on_timeout(tbcmh_handle_t client, int request_id)
{
     if (!client) {
          TBC_LOGE("client is NULL! %s()", __FUNCTION__);
          return;
     }

     // Take semaphore
     if (xSemaphoreTake(client->_lock, (TickType_t)0xFFFFF) != pdTRUE) {
          TBC_LOGE("Unable to take semaphore! %s()", __FUNCTION__);
          return;
     }

     // Search attributesrequest
     attributes_request_t *attributesrequest = NULL;
     LIST_FOREACH(attributesrequest, &client->attributesrequest_list, entry) {
          if (attributesrequest && (attributesrequest->request_id==request_id)) {
               break;
          }
     }
     if (!attributesrequest) {
          // Give semaphore
          xSemaphoreGive(client->_lock);
          TBC_LOGW("Unable to find attribute request:%d! %s()", request_id, __FUNCTION__);
          return;
     }

     // Cache and remove attributesrequest
     attributes_request_t *cache = _attributes_request_clone_wo_listentry(attributesrequest);
     LIST_REMOVE(attributesrequest, entry);
     _attributes_request_destroy(attributesrequest);
     // Give semaphore
     xSemaphoreGive(client->_lock);

     // Do timeout
     if (cache->on_timeout) {
         cache->on_timeout(cache->client, cache->context, cache->request_id); //(none/resend/destroy/_destroy_all_attributes)?
     }

     // Free attributesrequest
     _attributes_request_destroy(cache);

     return;
}

