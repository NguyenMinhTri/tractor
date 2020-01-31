//DONE: receive a command from iotc via mqtt and print it
//DONE: use mac address as device id
//TODO: recreate jwt token on disconnection

//TODO: get the url for the last version available
//TODO: update itself


//TODO: use state topic for logging

//TODO: CREATE a python script to control official public versions:
// 1 - check last version on configuration file and up it accordingly: major, minor, patch (could be 3 separated variables)
// 2 - build and copy the bin file to a safe place with version on the filename
// 3 - upload the bin file to the cloud storage

//TODO: force an update to a specific version with command
//TODO: create configuration of update limits?
//TODO: create a way for each device to use unique certificates to connect to iotc


#include <stdlib.h>
#include <time.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "jsmn.h"
#include "lwip/apps/sntp.h"
#include "driver/gpio.h"

#include <iotc.h>
#include <iotc_jwt.h>


#define TAG "TractorApp"
#define APP_VERSION_MAJOR CONFIG_APP_VERSION_MAJOR
#define APP_VERSION_MINOR CONFIG_APP_VERSION_MINOR
#define APP_VERSION_PATCH CONFIG_APP_VERSION_PATCH


#define HX_SCK CONFIG_HX_WRITE
#define HX_DT CONFIG_HX_READ

#define IOTC_UNUSED(x) (void)(x)
#define DEVICE_PATH "projects/%s/locations/%s/registries/%s/devices/%s"
#define PUBLISH_TOPIC_EVENT "/devices/%s/events"
//#define PUBLISH_TOPIC_STATE "/devices/%s/state"
#define SUBSCRIBE_TOPIC_COMMAND "/devices/%s/commands/#"
//#define SUBSCRIBE_TOPIC_CONFIG "/devices/%s/config"

#define COMMAND_UPDATE_NOW 777

extern const uint8_t ec_pv_key_start[] asm("_binary_private_key_pem_start");
char *DEVICE_ID, *subscribe_topic_command, *subscribe_topic_config;
iotc_mqtt_qos_t iotc_example_qos = IOTC_MQTT_QOS_AT_LEAST_ONCE;
static iotc_timed_task_handle_t delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
iotc_context_handle_t iotc_context = IOTC_INVALID_CONTEXT_HANDLE;


#define WIFI_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define WIFI_RETRIES 5
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_num = 0;


static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
		esp_wifi_connect();
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
		if (s_retry_num < WIFI_RETRIES) {
			esp_wifi_connect();
			xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
			s_retry_num++;
			ESP_LOGI(TAG, "retry to connect to the AP");
		}
		ESP_LOGI(TAG, "connect to the AP fail");
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
		ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
		ESP_LOGI(TAG, "got ip:"
				IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

void wifi_station_init(void) {
	s_wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
	ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
	wifi_config_t wifi_config = {.sta = {.ssid = WIFI_SSID, .password = WIFI_PASS},};
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
}


static void time_init(void) {
	ESP_LOGI(TAG, "Initializing SNTP");
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "time.google.com");
	sntp_setservername(0, "time.apple.com");
	sntp_init();
}

static void nvs_init(void) {
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
}

static void hx711_init(void) {
	gpio_pad_select_gpio(HX_SCK);
	gpio_set_direction(HX_SCK, GPIO_MODE_OUTPUT);
	gpio_pad_select_gpio(HX_DT);
	gpio_set_direction(HX_DT, GPIO_MODE_INPUT);
}

static void update_time(void) {
	time_t now = 0;
	struct tm timeinfo = {0};
	while (timeinfo.tm_year < (2016 - 1900)) {
		ESP_LOGI(TAG, "waiting for system time to be set");
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		time(&now);
		localtime_r(&now, &timeinfo);
	}
	
	char buffer[26];
	strftime(buffer, 26, "%Y-%m-%dT%H:%M:%S", &timeinfo);
	ESP_LOGI(TAG, "system time is %s UTC", buffer);
}


long hx_read(unsigned char samples) {
	long result = 0;
	const unsigned char gain = 1;
	for (unsigned char s = 0; s < samples; s++) {
		unsigned long value = 0;
		gpio_set_level(HX_SCK, 0);
		while (gpio_get_level(HX_DT) == 1);
		for (char i = 0; i < 24 + gain; i++) {
			gpio_set_level(HX_SCK, 1);
			value <<= 1u;
			gpio_set_level(HX_SCK, 0);
			if (gpio_get_level(HX_DT)) value++;
		}
		value >>= gain;
		if (value >> 23u) value |= 0xFF000000;
		result += (long) value / samples;
	}
	gpio_set_level(HX_SCK, 1); //hx sleep
	return result;
}

void iotc_mqtt_publish_message(iotc_context_handle_t context_handle, iotc_timed_task_handle_t timed_task, void *user_data) {
	IOTC_UNUSED(timed_task);
	IOTC_UNUSED(user_data);
	char *publish_topic, *publish_message;
	asprintf(&publish_topic, PUBLISH_TOPIC_EVENT, DEVICE_ID);
	asprintf(&publish_message, "{\"m\":%ld}", hx_read(5));
	ESP_LOGI(TAG, "publishing msg `%s` to topic: `%s`", publish_message, publish_topic);
	
	iotc_publish(context_handle, publish_topic, publish_message, iotc_example_qos, /*callback=*/NULL, /*user_data=*/NULL);
	free(publish_topic);
	free(publish_message);
	
//	UBaseType_t water_mark = uxTaskGetStackHighWaterMark(NULL);
//	ESP_LOGI(TAG, "wm: %d", water_mark);
//
//	heap_caps_print_heap_info(MALLOC_CAP_8BIT);
	
}

void
iotc_mqtt_subscription_event_handler(iotc_context_handle_t in_context_handle, iotc_sub_call_type_t call_type, const iotc_sub_call_params_t *const params, iotc_state_t state, void *user_data) {
	IOTC_UNUSED(in_context_handle);
	IOTC_UNUSED(state);
	IOTC_UNUSED(user_data);
	
//	if these are null, nothing will work
	if (params == NULL && params->message.topic == NULL) {
		return;
	}
	
	if (call_type == IOTC_SUB_CALL_SUBACK) {
		ESP_LOGI(TAG, "mqtt subscribed successfully");
		return;
	}
	
	if (call_type == IOTC_SUB_CALL_MESSAGE) {
		char *sub_message = (char *) malloc(params->message.temporary_payload_data_length + 1);
		if (sub_message == NULL) {
			ESP_LOGE(TAG, "failed to allocate memory to receive message from topic `%s`", params->message.topic);
			return;
		}
		memcpy(sub_message, params->message.temporary_payload_data, params->message.temporary_payload_data_length);
		sub_message[params->message.temporary_payload_data_length] = '\0';
//		ESP_LOGI(TAG, "payload received: `%s`", sub_message);
		
		int command = 0;
		sscanf(sub_message, "%d", &command);
		
		if (command == COMMAND_UPDATE_NOW) {
			ESP_LOGI(TAG, "command acknowledged: will update now");
		} else {
			ESP_LOGI(TAG, "command not acknowledged: %d", command);
		}
		
		free(sub_message);
	}
}


void iotc_mqtt_connection_event_handler(iotc_context_handle_t in_context_handle, void *data, iotc_state_t state) {
	iotc_connection_data_t *conn_data = (iotc_connection_data_t *) data;
	
	switch (conn_data->connection_state) {
//		IOTC_CONNECTION_STATE_OPENED means that the connection has been
//		established and the IoTC Client is ready to send/recv messages
		case IOTC_CONNECTION_STATE_OPENED:
			ESP_LOGI(TAG, "iotc connection has opened successfully with known state %d", state);
			
//			subscribe to command topic
//			for now we use this for commanding an update
			asprintf(&subscribe_topic_command, SUBSCRIBE_TOPIC_COMMAND, DEVICE_ID);
			ESP_LOGI(TAG, "subscribing to topic: `%s`", subscribe_topic_command);
			iotc_subscribe(in_context_handle, subscribe_topic_command, IOTC_MQTT_QOS_AT_LEAST_ONCE, &iotc_mqtt_subscription_event_handler, NULL);

//			asprintf(&subscribe_topic_config, SUBSCRIBE_TOPIC_CONFIG, CONFIG_GIOT_DEVICE_ID);
//			ESP_LOGI(TAG, "subscribe to topic: \"%s\"\n", subscribe_topic_config);
//			iotc_subscribe(in_context_handle, subscribe_topic_config, IOTC_MQTT_QOS_AT_LEAST_ONCE, &iotc_mqttlogic_subscribe_callback, NULL);

//			Create a timed task to publish every 10 seconds.
			delayed_publish_task = iotc_schedule_timed_task(in_context_handle, iotc_mqtt_publish_message, 10, 1, NULL);
			break;

//		IOTC_CONNECTION_STATE_OPEN_FAILED is set when there was a problem
//		when establishing a connection to the server. The reason for the error
//		is contained in the 'state' variable. Here we log the error state and
//		exit out of the application.
		case IOTC_CONNECTION_STATE_OPEN_FAILED:
			ESP_LOGI(TAG, "iotc connection has failed to open with state %d", state);
			esp_restart();
			break;

//		IOTC_CONNECTION_STATE_CLOSED is set when the IoTC Client has been
//		disconnected. The disconnection may have been caused by some external
//		issue, or user may have requested a disconnection. In order to
//		distinguish between those two situation it is advised to check the state
//		variable value. If the state == IOTC_STATE_OK then the application has
//		requested a disconnection via 'iotc_shutdown_connection'. If the state !=
//		IOTC_STATE_OK then the connection has been closed from one side.
		case IOTC_CONNECTION_STATE_CLOSED:
			esp_restart();
////			When the connection is closed it's better to cancel some of previously
////			registered activities. Using cancel function on handler will remove the
////			handler from the timed queue which prevents the registered handle to be
////			called when there is no connection.
////			if (delayed_publish_task != IOTC_INVALID_TIMED_TASK_HANDLE) {
//			iotc_cancel_timed_task(delayed_publish_task);
////				delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
////			}
//
//			if (state == IOTC_STATE_OK) {
////				The connection has been closed intentionally. Therefore, stop
////				the event processing loop as there's nothing left to do.
//				iotc_events_stop();
//			} else {
////				The disconnection was unforeseen. Try reconnect to the server
////				with new configuration.
//				ESP_LOGI(TAG, "iotc connection closed with state %d", state);
//				esp_restart();
//
//			}
			break;
		
		default:
			ESP_LOGI(TAG, "iotc connection changed to unknown state %d", state);
			esp_restart();
			break;
	}
}


static void iotc_mqtt_connection_task(void *pvParameters) {
	/* Format the key type descriptors so the client understands
	 which type of key is being represented. In this case, a PEM encoded
	 byte array of a ES256 key. */
	iotc_crypto_key_data_t iotc_connect_private_key_data;
	iotc_connect_private_key_data.crypto_key_signature_algorithm = IOTC_CRYPTO_KEY_SIGNATURE_ALGORITHM_ES256;
	iotc_connect_private_key_data.crypto_key_union_type = IOTC_CRYPTO_KEY_UNION_TYPE_PEM;
	iotc_connect_private_key_data.crypto_key_union.key_pem.key = (char *) ec_pv_key_start;
	
	/* initialize iotc library and create a context to use to connect to the
	* GCP IoT Core Service. */
	const iotc_state_t error_init = iotc_initialize();
	
	if (IOTC_STATE_OK != error_init) {
		ESP_LOGI(TAG, "iotc failed to initialize, error: %d\n", error_init);
		vTaskDelete(NULL);
	}
	
	/*  Create a connection context. A context represents a Connection
		on a single socket, and can be used to publish and subscribe
		to numerous topics. */
	iotc_context = iotc_create_context();
	if (IOTC_INVALID_CONTEXT_HANDLE >= iotc_context) {
		ESP_LOGI(TAG, "iotc failed to create context, error: %d\n", -iotc_context);
		vTaskDelete(NULL);
	}
	
	/*  Queue a connection request to be completed asynchronously.
		The 'on_connection_state_changed' parameter is the name of the
		callback function after the connection request completes, and its
		implementation should handle both successful connections and
		unsuccessful connections as well as disconnections. */
	const uint16_t connection_timeout = 0;
	const uint16_t keepalive_timeout = 20;
	
//	iotc jwt effective timeout = timeout + 600 seconds
//	iotc jwt max timeout is 24h = 86400 seconds
	const uint32_t jwt_timeout = 86400 - 600;
	
	
	/* Generate the client authentication JWT, which will serve as the MQTT
	 * password. */
	char jwt[IOTC_JWT_SIZE] = {0};
	size_t bytes_written = 0;
	iotc_state_t state = iotc_create_iotcore_jwt(CONFIG_GIOT_PROJECT_ID, jwt_timeout, &iotc_connect_private_key_data, jwt, IOTC_JWT_SIZE, &bytes_written);
	
	if (IOTC_STATE_OK != state) {
		ESP_LOGI(TAG, "iotc_create_iotcore_jwt returned with error: %ul", state);
		vTaskDelete(NULL);
	}
	
	char *device_path = NULL;
	asprintf(&device_path, DEVICE_PATH, CONFIG_GIOT_PROJECT_ID, CONFIG_GIOT_LOCATION, CONFIG_GIOT_REGISTRY_ID, DEVICE_ID);
	
	
	// this line is the connection at last
	iotc_connect(iotc_context, NULL, jwt, device_path, connection_timeout, keepalive_timeout, &iotc_mqtt_connection_event_handler);
	free(device_path);
	/* The IoTC Client was designed to be able to run on single threaded devices.
		As such it does not have its own event loop thread. Instead you must
		regularly call the function iotc_events_process_blocking() to process
		connection requests, and for the client to regularly check the sockets for
		incoming data. This implementation has the loop operate endlessly. The loop
		will stop after closing the connection, using iotc_shutdown_connection() as
		defined in on_connection_state_change logic, and exit the event handler
		handler by calling iotc_events_stop(); */
	iotc_events_process_blocking();
	iotc_delete_context(iotc_context);
	iotc_shutdown();
	vTaskDelete(NULL);
}

static void app_device_init(void) {
	unsigned char MAC[6];
	esp_efuse_mac_get_default(MAC);
	asprintf(&DEVICE_ID, "%02x%02x%02x%02x%02x%02x", MAC[0], MAC[1], MAC[2], MAC[3], MAC[4], MAC[5]);
	ESP_LOGI(TAG, "DEVICE ID: %s", DEVICE_ID);
	ESP_LOGI(TAG, "APP VERSION: %d.%d.%d", APP_VERSION_MAJOR, APP_VERSION_MINOR, APP_VERSION_PATCH);
}

void app_main() {
	
	app_device_init();
	
	nvs_init();
	wifi_station_init();
	time_init();
	update_time();
	hx711_init();
	
	xTaskCreate(&iotc_mqtt_connection_task, "iotc_mqtt_task", 6144, NULL, 5, NULL);
}
