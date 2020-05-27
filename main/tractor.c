#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <jsmn.h>
#include <lwip/apps/sntp.h>
#include <driver/gpio.h>
#include <iotc.h>
#include <iotc_jwt.h>
#include <esp_http_client.h>
#include <esp_ota_ops.h>
#include <esp_phy_init.h>
#include <esp_netif.h>

#define TAG "TractorApp"
static const esp_app_desc_t *APP_DESC;

//hx711
#define HX_SCK CONFIG_HX_WRITE_GPIO_PIN
#define HX_DT CONFIG_HX_READ_GPIO_PIN

//wifi
#define WIFI_SSID CONFIG_ESP_WIFI_SSID
#define WIFI_PASS CONFIG_ESP_WIFI_PASSWORD
#define WIFI_RETRIES 5
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static int s_retry_num = 0;

//iotc
#define IOTC_UNUSED(x) (void)(x)
#define DEVICE_PATH "projects/%s/locations/%s/registries/%s/devices/%s"
#define PUBLISH_TOPIC_EVENT "/devices/%s/events"
//#define PUBLISH_TOPIC_STATE "/devices/%s/state"
#define SUBSCRIBE_TOPIC_COMMAND "/devices/%s/commands/#"
//#define SUBSCRIBE_TOPIC_CONFIG "/devices/%s/config"
static char *DEVICE_ID, *subscribe_topic_command, *subscribe_topic_config;
static iotc_mqtt_qos_t iotc_example_qos = IOTC_MQTT_QOS_AT_LEAST_ONCE;
static iotc_timed_task_handle_t delayed_publish_task = IOTC_INVALID_TIMED_TASK_HANDLE;
static iotc_context_handle_t iotc_context = IOTC_INVALID_CONTEXT_HANDLE;

//remote commands
#define COMMAND_UPDATE_NOW 777

//ota
#define BUFFSIZE 1024
#define HASH_LEN 32
//#define OTA_URL_SIZE 256
#define DIAGNOSTIC_PIN CONFIG_DIAGNOSTIC_GPIO_PIN
//#define OTA_CHECK_VERSION_ALREADY_LOADED CONFIG_OTA_CHECK_VERSION_ALREADY_LOADED
char *ota_buffer[BUFFSIZE+1];
char *ota_url;

//certs
extern const uint8_t ec_pv_key_start[] asm("_binary_private_key_pem_start");
extern const uint8_t google_root_ca_pem_start[] asm("_binary_google_roots_pem_start");


static void app_device_init(void) {
	unsigned char MAC[6];
	esp_efuse_mac_get_default(MAC);
	asprintf(&DEVICE_ID, "%02x%02x%02x%02x%02x%02x", MAC[0], MAC[1], MAC[2], MAC[3], MAC[4], MAC[5]);
	ESP_LOGI(TAG, "DEVICE ID: %s", DEVICE_ID);
	
	APP_DESC = esp_ota_get_app_description();
	ESP_LOGI(TAG, "APP VERSION: %s", APP_DESC->version);
	
	gpio_pad_select_gpio(HX_SCK);
	gpio_set_direction(HX_SCK, GPIO_MODE_OUTPUT);
	gpio_pad_select_gpio(HX_DT);
	gpio_set_direction(HX_DT, GPIO_MODE_INPUT);
}

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

static void wifi_station_init(void) {
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
	sntp_setservername(0, "pool.ntp.org");
	sntp_setservername(1, "time.google.com");
	sntp_setservername(2, "time.apple.com");
	sntp_init();
	
	time_t now = 0;
	struct tm timeinfo = {0};
	while (timeinfo.tm_year < (2020-1900)) {
		ESP_LOGI(TAG, "setting system time...");
		vTaskDelay(5000 / portTICK_PERIOD_MS);
		time(&now);
		localtime_r(&now, &timeinfo);
	}
	
	char buffer[26];
	strftime(buffer, 26, "%Y-%m-%dT%H:%M:%S", &timeinfo);
	ESP_LOGI(TAG, "system time is %s UTC", buffer);
}

static void nvs_init(void) {
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
}

static long hx_read(unsigned char samples) {
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

static void ota_update_task(void *pvParameters) {
	ESP_LOGI(TAG, "starting OTA");
	
	esp_err_t err;
	
	const esp_partition_t *boot_partition = esp_ota_get_boot_partition();
	const esp_partition_t *running_partition = esp_ota_get_running_partition();
	const esp_partition_t *update_partition = esp_ota_get_next_update_partition(running_partition);
	
	ESP_LOGI(TAG, "Boot partition type %d subtype %d offset 0x%08x", boot_partition->type, boot_partition->subtype, boot_partition->address);
	ESP_LOGI(TAG, "Running partition type %d subtype %d offset 0x%08x", running_partition->type, running_partition->subtype, running_partition->address);
	ESP_LOGI(TAG, "Update partition type %d subtype %d offset 0x%08x", update_partition->type, update_partition->subtype, update_partition->address);
	
	if (update_partition == NULL) {
		ESP_LOGE(TAG, "null update partition, terminating update");
		vTaskDelete(NULL);
	}
	
	esp_http_client_config_t http_config = {
			.url = ota_url,
			.cert_pem = (char *) google_root_ca_pem_start,
//			.event_handler = http_event_handler,
	};
	
	esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
	if (http_client == NULL) {
		ESP_LOGE(TAG, "failed to initialise http connection");
		vTaskDelete(NULL);
	}
	
	err = esp_http_client_open(http_client, 0);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "failed to open HTTP connection: %s %d", esp_err_to_name(err), err);
		vTaskDelete(NULL);
	}
	int headers_count = esp_http_client_fetch_headers(http_client);
	ESP_LOGE(TAG, "headers %d", headers_count);
	
	//init ota writer
	esp_ota_handle_t ota_handle;
	err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_begin failed (%s) %d", esp_err_to_name(err), err);
		vTaskDelete(NULL);
	}
	ESP_LOGI(TAG, "esp_ota_begin succeeded");
	
	
	int binary_file_length = 0;
	while (1) {
		int data_read = esp_http_client_read(http_client, (char *) ota_buffer, BUFFSIZE);
//		ESP_LOGI(TAG, "readed %d", data_read);
		
		if (data_read < 0) {
			ESP_LOGE(TAG, "ssl data read error");
			vTaskDelete(NULL);
		}
		
		else if (data_read > 0) {
			err = esp_ota_write(ota_handle, ota_buffer, data_read);
			if (err != ESP_OK) {
				ESP_LOGE(TAG, "error writing to ota partition %s %d", esp_err_to_name(err), err);
				vTaskDelete(NULL);
			}
			binary_file_length += data_read;
			ESP_LOGI(TAG, "written image length %d", binary_file_length);
		}
		
		else if (data_read == 0) {
			if (esp_http_client_is_complete_data_received(http_client) == true) {
				break;
			}
		}
	}
	
	ESP_LOGI(TAG, "total write binary data length: %d", binary_file_length);
	
	if (esp_http_client_is_complete_data_received(http_client) != true) {
		ESP_LOGE(TAG, "error in receiving complete file");
		vTaskDelete(NULL);
	}
	
	err = esp_ota_end(ota_handle);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_end failed %s %d!", esp_err_to_name(err), err);
		vTaskDelete(NULL);
	}
	
	// erase wifi nvs data calibration (phy) to force recalibration of wifi with the new version on restart
	err = esp_phy_erase_cal_data_in_nvs();
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "error erasing phy nvs: %s %d", esp_err_to_name(err), err);
	}
	
	err = esp_ota_set_boot_partition(update_partition);
	if (err != ESP_OK) {
		ESP_LOGE(TAG, "esp_ota_set_boot_partition failed %s %d", esp_err_to_name(err), err);
		vTaskDelete(NULL);
	}

	
	ESP_LOGI(TAG, "ota successfull rebooting");
	esp_restart();
}

void iotc_mqtt_publish_message(iotc_context_handle_t context_handle, iotc_timed_task_handle_t timed_task, void *user_data) {
	IOTC_UNUSED(timed_task);
	IOTC_UNUSED(user_data);
	char *publish_topic, *publish_message;
	asprintf(&publish_topic, PUBLISH_TOPIC_EVENT, DEVICE_ID);
	asprintf(&publish_message, "{\"k\":\"%s\",\"v\":%ld}", "loadcell", hx_read(5));
	ESP_LOGI(TAG, "publishing msg `%s` to topic: `%s`", publish_message, publish_topic);
	
	iotc_publish(context_handle, publish_topic, publish_message, iotc_example_qos, /*callback=*/NULL, /*user_data=*/NULL);
	free(publish_topic);
	free(publish_message);
	
//	UBaseType_t water_mark = uxTaskGetStackHighWaterMark(NULL);
//	ESP_LOGI(TAG, "wm: %d", water_mark);
//
//	heap_caps_print_heap_info(MALLOC_CAP_8BIT);
	
}

void iotc_mqtt_subscription_event_handler(iotc_context_handle_t in_context_handle, iotc_sub_call_type_t call_type, const iotc_sub_call_params_t *const params, iotc_state_t state, void *user_data) {
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
//		char *sub_message = (char *) malloc(params->message.temporary_payload_data_length + 1);
//		if (sub_message == NULL) {
//			ESP_LOGE(TAG, "failed to allocate memory to receive message from topic `%s`", params->message.topic);
//			return;
//		}
//		memcpy(sub_message, params->message.temporary_payload_data, params->message.temporary_payload_data_length);
//		sub_message[params->message.temporary_payload_data_length] = '\0';
		
		ota_url = (char *) malloc(params->message.temporary_payload_data_length + 1);
		memcpy(ota_url, params->message.temporary_payload_data, params->message.temporary_payload_data_length);
		ota_url[params->message.temporary_payload_data_length] = '\0';
		
		ESP_LOGI(TAG, "command acknowledged: will update now with file %s", ota_url);
		xTaskCreate(&ota_update_task, "simple_ota_task", 1024 * 8, NULL, 5, NULL);
		
//		free(sub_message);
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
		ESP_LOGI(TAG, "iotc failed to initialize, error: %d", error_init);
		vTaskDelete(NULL);
	}
	
	/*  Create a connection context. A context represents a Connection
		on a single socket, and can be used to publish and subscribe
		to numerous topics. */
	iotc_context = iotc_create_context();
	if (IOTC_INVALID_CONTEXT_HANDLE >= iotc_context) {
		ESP_LOGI(TAG, "iotc failed to create context, error: %d", -iotc_context);
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
	ESP_LOGI(TAG, "%s", device_path);
	
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

static void print_sha256 (const uint8_t *image_hash, const char *label) {
	char hash_print[HASH_LEN * 2 + 1];
	hash_print[HASH_LEN * 2] = 0;
	for (int i = 0; i < HASH_LEN; ++i) {
		sprintf(&hash_print[i * 2], "%02x", image_hash[i]);
	}
	ESP_LOGI(TAG, "%s: %s", label, hash_print);
}

static bool diagnostic(void) {
	gpio_config_t io_conf;
	io_conf.intr_type    = (gpio_int_type_t) GPIO_PIN_INTR_DISABLE;
	io_conf.mode         = GPIO_MODE_INPUT;
	io_conf.pin_bit_mask = (1ULL << DIAGNOSTIC_PIN);
	io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
	io_conf.pull_up_en   = GPIO_PULLUP_ENABLE;
	gpio_config(&io_conf);
	
	ESP_LOGI(TAG, "Diagnostics (5 sec)...");
	vTaskDelay(5000 / portTICK_PERIOD_MS);
	
	bool diagnostic_is_ok = gpio_get_level(DIAGNOSTIC_PIN);
	
	gpio_reset_pin(DIAGNOSTIC_PIN);
	return diagnostic_is_ok;
}

static void check_partition(void) {
	uint8_t sha_256[HASH_LEN] = { 0 };
	esp_partition_t partition;
	
	// get sha256 digest for the partition table
	partition.address   = ESP_PARTITION_TABLE_OFFSET;
	partition.size      = ESP_PARTITION_TABLE_MAX_LEN;
	partition.type      = ESP_PARTITION_TYPE_DATA;
	esp_partition_get_sha256(&partition, sha_256);
	print_sha256(sha_256, "SHA-256 for the partition table: ");
	
	// get sha256 digest for bootloader
	partition.address   = ESP_BOOTLOADER_OFFSET;
	partition.size      = ESP_PARTITION_TABLE_OFFSET;
	partition.type      = ESP_PARTITION_TYPE_APP;
	esp_partition_get_sha256(&partition, sha_256);
	print_sha256(sha_256, "SHA-256 for bootloader: ");
	
	// get sha256 digest for running partition
	esp_partition_get_sha256(esp_ota_get_running_partition(), sha_256);
	print_sha256(sha_256, "SHA-256 for current firmware: ");
	
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_img_states_t ota_state;
	if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
		if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
			// run diagnostic function ...
			bool diagnostic_is_ok = diagnostic();
			if (diagnostic_is_ok) {
				ESP_LOGI(TAG, "Diagnostics completed successfully! Continuing execution ...");
				esp_ota_mark_app_valid_cancel_rollback();
			} else {
				ESP_LOGE(TAG, "Diagnostics failed! Start rollback to the previous version ...");
				esp_ota_mark_app_invalid_rollback_and_reboot();
			}
		}
	}
}

static void enforcements(void) {
	// possible corruptions in boot
	const esp_partition_t *running = esp_ota_get_running_partition();
	esp_ota_set_boot_partition(running);
}

static void download_test_task(void *pvParameters){
	esp_http_client_config_t http_config = {
			.url = "https://storage.googleapis.com/heartflow-bin/espressif-devkitc-v4/file.txt",
			.cert_pem = (char *) google_root_ca_pem_start,
//			.timeout_ms = 30000,
//			.method = HTTP_METHOD_GET,
//			.disable_auto_redirect = false,
//			.buffer_size = 1024,
//			.is_async = true,
//			.event_handler = http_event_handler,
//			.max_redirection_count = 30,
	};

	esp_http_client_handle_t http_client = esp_http_client_init(&http_config);
	if (http_client == NULL) {
		ESP_LOGE(TAG, "failed to initialise http connection");
		vTaskDelete(NULL);
	}

	esp_err_t err;

	while (1) {
		err = esp_http_client_open(http_client, 0);
		if (err != ESP_OK) {
			ESP_LOGE(TAG, "failed to open HTTP connection: %s %d", esp_err_to_name(err), err);
			vTaskDelete(NULL);
		}

		int content_length = esp_http_client_fetch_headers(http_client);
		ESP_LOGI(TAG, "headers content-length %d", content_length);

		int status_code = esp_http_client_get_status_code(http_client);
		ESP_LOGI(TAG, "status code %d", status_code);

		if (status_code >= 300 && status_code < 310){
			esp_http_client_set_redirection(http_client);
		} else {
			break;
		}
	}

	char buffer[1024];
	int total_len = 0;
	while (1) {
		int data_len = esp_http_client_read(http_client, buffer, 1024);
		ESP_LOGI(TAG, "len %d", data_len);

		if (data_len <= 0) {
			break;
		}
		else if(data_len > 0){
			ESP_LOGI("XDATA", "%.*s", data_len, buffer);
			total_len += data_len;
		}
	}

	ESP_LOGI(TAG, "total %d", total_len);

	err = esp_http_client_is_complete_data_received(http_client);
	ESP_LOGI(TAG, "complete %d", err);

	err = esp_http_client_cleanup(http_client);
	ESP_LOGI(TAG, "cleaning %d", err);

	vTaskDelete(NULL);
}

void app_main() {
	enforcements();
	check_partition();
	
	app_device_init();
	nvs_init();
	wifi_station_init();
	time_init();
	
	xTaskCreate(&iotc_mqtt_connection_task, "iotc_mqtt_task", 1024*6, NULL, 5, NULL);
}
