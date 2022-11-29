/* 
 * EQ-3 WiFi support code.
 * 
 * Uses bootwifi to enable first-time configuration
 * Could instead use Espressiv smartconfig to connect wifi - see bottom of this file 
 * Connects to mqtt broker (no security currently) to send/receive status/commands
 * Ensure CONFIG_MQTT_SECURITY_ON is not defined in MQTT config file 
 * 
 * (C) Paul Tupper 2017
 *
 * updated by Peter Becker (C) 2019
 */
/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wpa2.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "mqtt_client.h"

#include "eq3_main.h"
#include "eq3_wifi.h"
#include "eq3_gap.h"

static const char *MQTT_TAG = "mqtt";

/* =========================================
 * MQTT support code
 */

static void connected_cb(esp_mqtt_event_handle_t event);
static void data_cb(esp_mqtt_event_handle_t event);
static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event);

static esp_mqtt_client_handle_t repclient = NULL;
static bool mqtt_config_error = false;
static char *devlist = NULL;

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event){
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(MQTT_TAG, "MQTT connected");
            connected_cb(event);
            break;
        case MQTT_EVENT_DISCONNECTED:
            repclient = NULL;
            ESP_LOGI(MQTT_TAG, "MQTT disconnected - wait for reconnect");
            break;
        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_DATA, msg_id=%d", event->msg_id);
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            data_cb(event);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(MQTT_TAG, "MQTT_EVENT_ERROR");
            break;
        default:
            ESP_LOGI(MQTT_TAG, "Other event id:%d", event->event_id);
            break;
    }
    return ESP_OK;
}

#define IN_TOPIC_LEN     30
#define OUT_TOPIC_LEN    30
#define LWT_TOPIC_LEN    38

/* Inbound topic prefix */
static char intopicbase[IN_TOPIC_LEN];
/* Outbound topic prefix */
static char outtopicbase[OUT_TOPIC_LEN];
/* LastWillTestament topic (same as outbound topic currently but keep separate in case we want to change it) */
static char lwt_topic_buff[LWT_TOPIC_LEN];

mqttconnstate ismqttconnected(void){
    if(mqtt_config_error == true)
        return MQTT_CONFIG_ERROR;
    return repclient == NULL ? MQTT_NOT_CONNECTED : MQTT_CONNECTED;
}

/* MQTT connected callback */
static void connected_cb(esp_mqtt_event_handle_t event){
    esp_log_level_set("MQTT_CLIENT", ESP_LOG_VERBOSE);

    esp_mqtt_client_handle_t client = event->client;

    char topic[38];
    char startmsg[35];
    sprintf(topic, "%s/#", intopicbase);
    repclient = client;

    /* Subscribe to /espradin/# for commands */
    esp_mqtt_client_subscribe(client, topic, 0);
    ESP_LOGI(MQTT_TAG, "[APP] Start subscribe, topic: %s", topic);

    /* Publish welcome message to /espradout */
    sprintf(topic, "%s/connect", outtopicbase);
    sprintf(startmsg, "Heating control v%s.%s%s active", EQ3_MAJVER, EQ3_MINVER, EQ3_EXTRAVER);
    esp_mqtt_client_publish(client, topic, startmsg, strlen(startmsg), 0, 0);

    ESP_LOGI(MQTT_TAG, "[APP] Start publish, topic: %s", topic);
    ESP_LOGI(MQTT_TAG, "[APP] Start publish, msg: %s", startmsg);

    if(devlist != NULL){
        sprintf(topic, "%s/devlist", outtopicbase);
        /* Publish discovered EQ-3 device list to /espradout/devlist */
        esp_mqtt_client_publish(client, topic, devlist, strlen(devlist), 0, 0);

        ESP_LOGI(MQTT_TAG, "[APP] Start publish, topic: %s", topic);
        ESP_LOGI(MQTT_TAG, "[APP] Start publish, devlist: %s", devlist);

        free(devlist);
        devlist = NULL;
    }
}

/* MQTT data received (subscribed topic receives data) */
static void data_cb(esp_mqtt_event_handle_t event){
    esp_mqtt_client_handle_t client = event->client;
    char* topic = malloc (event->topic_len + 1);
    bool trvcmd = false, trvscan = false;
    if(event->current_data_offset == 0) {
        memcpy(topic, event->topic, event->topic_len);
        topic[event->topic_len] = 0;
        /* /trv is a command to an EQ3 valve */
        if (strstr (topic, "/trv") != NULL) {
            trvcmd = true;
            ESP_LOGI (MQTT_TAG, "TRV command: %s", topic);
        }
        /* /scan is a request to run a BLE scan for EQ3 valves */
        if(strstr(topic, "/scan") != NULL)
            trvscan = true;
        /* /check is a simple 'ping' check that the ESP is connected */
        if(strstr(topic, "/check") != NULL){
            char rsptopic[45];
            char msg[35];
            sprintf(rsptopic, "%s/checkresp", outtopicbase);
            sprintf(msg, "sw ver %s.%s%s", EQ3_MAJVER, EQ3_MINVER, EQ3_EXTRAVER);
            esp_mqtt_client_publish(client, rsptopic, msg, strlen(msg), 0, 0);
        }
        ESP_LOGI(MQTT_TAG, "[APP] Publish topic: %s", topic);
    }

    if(trvcmd == true){
        //char *data = malloc(event->data_len + 1);
        //ESP_LOGI (MQTT_TAG, "Topic: %s, data: %s", topic, event->data);
        const int MAX_DATA_LEN = 80;
        char data[MAX_DATA_LEN];
        memset (data, 0, sizeof (data));
        char* topicptr = topic;
        //ESP_LOGI (MQTT_TAG, "topicptr = %c", *topicptr);
        // while (*topicptr != 0 && !isxdigit ((int)*topicptr)) {
        //     ESP_LOGI (MQTT_TAG, "topicptr = %c", *topicptr);
        //     topicptr++;
        // }
        int dataidx = 0;
        bool dataerror = false;
        topicptr = strstr (topicptr, "/trv/");
        if (topicptr) {
            topicptr += 5;
            //ESP_LOGI (MQTT_TAG, "Found string \"/trv/\" %s", topicptr);
        } else {
            dataerror = true;
        }

        while (*topicptr != '/' && *topicptr != 0 && !dataerror) {
            //ESP_LOGI (MQTT_TAG, "topicptr = %c", *topicptr);
            if ((isxdigit (*topicptr) || *topicptr == ':') && dataidx < MAX_DATA_LEN - 1) {
                data[dataidx] = *topicptr;
                dataidx++;
                topicptr++;
            } else {
                dataerror = true;
            }
        }

        if (dataidx != 17) {
            dataerror = true;
            ESP_LOGI (MQTT_TAG, "Wrong address length: %d", dataidx);
        }
        
        // ESP_LOGI (MQTT_TAG, "Added address %s idx %d", data, dataidx);

        if (*topicptr == '/') {
            topicptr++;
        } else {
            dataerror = true;
        }
        
        if (!dataerror && dataidx < MAX_DATA_LEN - 1) {
            data[dataidx] = ' ';
            dataidx++;
        } else {
            dataerror = true;
        }

        // ESP_LOGI (MQTT_TAG, "Added space %s idx %d", data, dataidx);

        while (*topicptr != '\0' && !dataerror && dataidx < MAX_DATA_LEN - 1) {
            data[dataidx] = *topicptr;
            dataidx++;
            topicptr++;
        }

        if (event->data_len > 0) {
            if (!dataerror && dataidx < MAX_DATA_LEN - 1) {
                data[dataidx] = ' ';
                dataidx++;
            } else {
                dataerror = true;
            }
        }

        ESP_LOGI (MQTT_TAG, "Added command \"%s\" idx %d", data, dataidx);

        if (!dataerror && dataidx < MAX_DATA_LEN - 1 - event->data_len) {
            //memcpy ((void*)&(data[dataidx]), (void*)event->data, event->data_len);
            int i = 0;
            for (i = 0; i < event->data_len; i++) {
                data[dataidx] = event->data[i];
                dataidx++;
            }
            data[dataidx] = 0;
            ESP_LOGI (MQTT_TAG, "Handle trv mqtt msg \"%s\"", data);
            handle_request (data);
        }
        //free(data);
    }
    
    free (topic);

    if(trvscan == true){
        start_scan();
    }
    
}

/* Publish a status message */
int send_trv_status(char *status, char* mac_addr){
	ESP_LOGI(MQTT_TAG, "send_trv_status");
    if(repclient != NULL){
        char topic[38];
        if (mac_addr != NULL) {
            sprintf (topic, "%s/status/%s", outtopicbase, mac_addr);
            esp_mqtt_client_publish (repclient, topic, status, strlen (status), 0, 0);
        } else {
            ESP_LOGW (MQTT_TAG, "NULL mac address");
        }
        
    }
    return 0;
}

/* Publish a discovered device list */
int send_device_list(char *list){
    if(repclient != NULL){
        char topic[38];
        sprintf(topic, "%s/devlist", outtopicbase);
        esp_mqtt_client_publish(repclient, topic, list, strlen(list), 0, 0);
	    free(list);
    }else{
        if(devlist != NULL)
	    free(devlist);
	    ESP_LOGI(MQTT_TAG, "Queue device list message to publish");
        devlist = list;
    }
    return 0;
}

int connect_server(char *url, char *user, char *password, char *id){
    int rc = 0;
    mqtt_config_error = false;
    
    if(id == NULL || url == NULL){
        mqtt_config_error = true;
        return -1;
    }
    
    snprintf(lwt_topic_buff, LWT_TOPIC_LEN, "%sradout", id);
    snprintf(intopicbase, IN_TOPIC_LEN, "%sradin", id);
    snprintf(outtopicbase, OUT_TOPIC_LEN,  "%sradout", id);

    esp_mqtt_client_config_t settings = {
#if defined(CONFIG_MQTT_SECURITY_ON)
        .port = 8883, // encrypted
#else
        .port = 1883, // unencrypted
#endif
        .keepalive = 60,
        .lwt_msg = "Heating control offline",
        .lwt_qos = 0,
        .lwt_retain = 0,
        .event_handle = mqtt_event_handler,
        .uri = url,
        .username = user,
        .password = password,
        .client_id = id,
        .lwt_topic = lwt_topic_buff
    };

    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&settings);
    if(client){
        esp_mqtt_client_start(client);
        ESP_LOGI(MQTT_TAG, "[APP] Settings.lwt_topic: %s", settings.lwt_topic);
        ESP_LOGI(MQTT_TAG, "[APP] Settings.client_id: %s", settings.client_id);
    }else{
        ESP_LOGE(MQTT_TAG, "MQTT client failed to start");
        mqtt_config_error = true;
        rc = -1;
    }

    return rc;
}
