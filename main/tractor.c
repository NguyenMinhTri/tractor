#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "jsmn.h"
#include <time.h>
#include "lwip/apps/sntp.h"
#include "driver/gpio.h"

#include <iotc.h>
#include <iotc_jwt.h>


#define HX_SCK CONFIG_HX_WRITE
#define HX_DT CONFIG_HX_READ

static const char *TAG = "TractorApp";

//private key for the iotc
extern const uint8_t ec_pv_key_start[] asm("_binary_private_key_pem_start");
//extern const uint8_t ec_pv_key_end[] asm("_binary_private_key_pem_end");

#define IOTC_UNUSED(x) (void)(x)
#define DEVICE_PATH "projects/%s/locations/%s/registries/%s/devices/%s"
#define PUBLISH_TOPIC_EVENT "/devices/%s/events"
//#define PUBLISH_TOPIC_STATE "/devices/%s/state"
//#define SUBSCRIBE_TOPIC_COMMAND "/devices/%s/commands/#"
//#define SUBSCRIBE_TOPIC_CONFIG "/devices/%s/config"
//#define CONFIG_GIOT_PROJECT_ID
//#define CONFIG_GIOT_LOCATION
//#define CONFIG_GIOT_REGISTRY_ID
//#define CONFIG_GIOT_DEVICE_ID

char *subscribe_topic_command, *subscribe_topic_config;
iotc_mqtt_qos_t iotc_example_qos = IOTC_MQTT_QOS_AT_LEAST_ONCE;
static iotc_timed_task_handle_t delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
iotc_context_handle_t iotc_context = IOTC_INVALID_CONTEXT_HANDLE;


#define WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define WIFI_RETRIES  5
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
	
	ESP_LOGI(TAG, "wifi_init_sta finished.");
	ESP_LOGI(TAG, "connect to ap SSID:%s password:%s", WIFI_SSID, WIFI_PASS);
}


static void time_init(void) {
	ESP_LOGI(TAG, "Initializing SNTP");
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "time.google.com");
	sntp_init();
}

static void nvs_init(void) {
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
}

static void update_time(void) {
	// try and wait for time to be set
	time_t now = 0;
	struct tm timeinfo = {0};
	while (timeinfo.tm_year < (2016 - 1900)) {
		ESP_LOGI(TAG, "Waiting for system time to be set...");
		vTaskDelay(2000 / portTICK_PERIOD_MS);
		time(&now);
		localtime_r(&now, &timeinfo);
	}
	ESP_LOGI(TAG, "Time is set...");
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


void publish_telemetry_event(iotc_context_handle_t context_handle, iotc_timed_task_handle_t timed_task, void *user_data) {
	IOTC_UNUSED(timed_task);
	IOTC_UNUSED(user_data);
	char *publish_topic = NULL;
	asprintf(&publish_topic, PUBLISH_TOPIC_EVENT, CONFIG_GIOT_DEVICE_ID);
	char *publish_message = NULL;
	asprintf(&publish_message, "{\"m\":%ld}", hx_read(5));
	ESP_LOGI(TAG, "publishing msg `%s` to topic: `%s`", publish_message, publish_topic);
	
	iotc_publish(context_handle, publish_topic, publish_message, iotc_example_qos,
			/*callback=*/NULL, /*user_data=*/NULL);
	free(publish_topic);
	free(publish_message);
}

//void iotc_mqttlogic_subscribe_callback(
//    iotc_context_handle_t in_context_handle,
//    iotc_sub_call_type_t call_type,
//    const iotc_sub_call_params_t *const params,
//    iotc_state_t state,
//    void *user_data)
//{
//    IOTC_UNUSED(in_context_handle);
//    IOTC_UNUSED(call_type);
//    IOTC_UNUSED(state);
//    IOTC_UNUSED(user_data);
//    if (params != NULL && params->message.topic != NULL) {
//        ESP_LOGI(TAG, "Subscription Topic: %s\n", params->message.topic);
//        char *sub_message = (char *)malloc(params->message.temporary_payload_data_length + 1);
//        if (sub_message == NULL) {
//            ESP_LOGE(TAG, "Failed to allocate memory");
//            return;
//        }
//        memcpy(sub_message, params->message.temporary_payload_data, params->message.temporary_payload_data_length);
//        sub_message[params->message.temporary_payload_data_length] = '\0';
//        ESP_LOGI(TAG, "Message Payload: %s \n", sub_message);
//        if (strcmp(subscribe_topic_command, params->message.topic) == 0) {
//            int value;
//            sscanf(sub_message, "{\"outlet\": %d}", &value);
//            ESP_LOGI(TAG, "value: %d\n", value);
//            if (value == 1) {
//                gpio_set_level(OUTPUT_GPIO, true);
//            } else if (value == 0) {
//                gpio_set_level(OUTPUT_GPIO, false);
//            }
//        }
//        free(sub_message);
//    }
//}

void on_connection_state_changed(iotc_context_handle_t in_context_handle, void *data, iotc_state_t state) {
	iotc_connection_data_t *conn_data = (iotc_connection_data_t *) data;
	
	switch (conn_data->connection_state) {
		/* IOTC_CONNECTION_STATE_OPENED means that the connection has been
		   established and the IoTC Client is ready to send/recv messages */
		case IOTC_CONNECTION_STATE_OPENED:
			ESP_LOGI(TAG, "iotc connection has opened successfully with known state %d", state);
			
			/* Publish immediately upon connect. 'publish_function' is defined
			   in this example file and invokes the IoTC API to publish a
			   message. */
//        asprintf(&subscribe_topic_command, SUBSCRIBE_TOPIC_COMMAND, CONFIG_GIOT_DEVICE_ID);
//        ESP_LOGI(TAG, "subscribe to topic: \"%s\"\n", subscribe_topic_command);
//        iotc_subscribe(in_context_handle, subscribe_topic_command, IOTC_MQTT_QOS_AT_LEAST_ONCE,
//                       &iotc_mqttlogic_subscribe_callback, NULL);
//
//        asprintf(&subscribe_topic_config, SUBSCRIBE_TOPIC_CONFIG, CONFIG_GIOT_DEVICE_ID);
//        ESP_LOGI(TAG, "subscribe to topic: \"%s\"\n", subscribe_topic_config);
//        iotc_subscribe(in_context_handle, subscribe_topic_config, IOTC_MQTT_QOS_AT_LEAST_ONCE,
//                       &iotc_mqttlogic_subscribe_callback, NULL);
			
			/* Create a timed task to publish every 10 seconds. */
			delayed_publish_task = iotc_schedule_timed_task(in_context_handle, publish_telemetry_event, 1, 1, NULL);
			break;
			
			/* IOTC_CONNECTION_STATE_OPEN_FAILED is set when there was a problem
			   when establishing a connection to the server. The reason for the error
			   is contained in the 'state' variable. Here we log the error state and
			   exit out of the application. */
			
			/* Publish immediately upon connect. 'publish_function' is defined
			   in this example file and invokes the IoTC API to publish a
			   message. */
		case IOTC_CONNECTION_STATE_OPEN_FAILED:
			ESP_LOGI(TAG, "iotc connection has failed to open with known state %d", state);
			
			/* exit it out of the application by stopping the event loop. */
			iotc_events_stop();
			break;
			
			/* IOTC_CONNECTION_STATE_CLOSED is set when the IoTC Client has been
			   disconnected. The disconnection may have been caused by some external
			   issue, or user may have requested a disconnection. In order to
			   distinguish between those two situation it is advised to check the state
			   variable value. If the state == IOTC_STATE_OK then the application has
			   requested a disconnection via 'iotc_shutdown_connection'. If the state !=
			   IOTC_STATE_OK then the connection has been closed from one side. */
		case IOTC_CONNECTION_STATE_CLOSED:
			free(subscribe_topic_command);
			free(subscribe_topic_config);
			/* When the connection is closed it's better to cancel some of previously
			   registered activities. Using cancel function on handler will remove the
			   handler from the timed queue which prevents the registered handle to be
			   called when there is no connection. */
			if (delayed_publish_task != IOTC_INVALID_TIMED_TASK_HANDLE) {
				iotc_cancel_timed_task(delayed_publish_task);
				delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
			}
			
			if (state == IOTC_STATE_OK) {
				/* The connection has been closed intentionally. Therefore, stop
				   the event processing loop as there's nothing left to do
				   in this example. */
				iotc_events_stop();
			} else {
				ESP_LOGI(TAG, "iotc connection closed with unknown state %d", state);
				/* The disconnection was unforeseen.  Try reconnect to the server
				with previously set configuration, which has been provided
				to this callback in the conn_data structure. */
				iotc_connect(in_context_handle, conn_data->username, conn_data->password, conn_data->client_id, conn_data->connection_timeout, conn_data->keepalive_timeout, &on_connection_state_changed);
			}
			break;
		
		default:
			ESP_LOGI(TAG, "iotc connection changed to unknown state %d", state);
			break;
	}
}

static void mqtt_task(void *pvParameters) {
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
	
	/* Generate the client authentication JWT, which will serve as the MQTT
	 * password. */
	char jwt[IOTC_JWT_SIZE] = {0};
	size_t bytes_written = 0;
	iotc_state_t state = iotc_create_iotcore_jwt(CONFIG_GIOT_PROJECT_ID, 3600, &iotc_connect_private_key_data, jwt, IOTC_JWT_SIZE, &bytes_written);
	
	if (IOTC_STATE_OK != state) {
		ESP_LOGI(TAG, "iotc_create_iotcore_jwt returned with error: %ul", state);
		vTaskDelete(NULL);
	}
	
	char *device_path = NULL;
	asprintf(&device_path, DEVICE_PATH, CONFIG_GIOT_PROJECT_ID, CONFIG_GIOT_LOCATION, CONFIG_GIOT_REGISTRY_ID, CONFIG_GIOT_DEVICE_ID);
	iotc_connect(iotc_context, NULL, jwt, device_path, connection_timeout, keepalive_timeout, &on_connection_state_changed);
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
	ESP_LOGI(TAG, "mqtt task terminated");
}

void app_main() {
	gpio_pad_select_gpio(HX_SCK);
	gpio_set_direction(HX_SCK, GPIO_MODE_OUTPUT);
	gpio_pad_select_gpio(HX_DT);
	gpio_set_direction(HX_DT, GPIO_MODE_INPUT);
	
	nvs_init();
	wifi_station_init();
	time_init();
	update_time();
	
	xTaskCreate(&mqtt_task, "mqtt_task", 8192, NULL, 5, NULL);
}
