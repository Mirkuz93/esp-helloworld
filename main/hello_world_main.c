#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_event_loop.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "tcpip_adapter.h"
#include <arpa/inet.h>

#include <stdint.h>

// espmqtt library
#include "mqtt_client.h"
//#include "mqtt_config.h"

#include "md5.h"
#include "driver/gpio.h"
#include "sdkconfig.h"
#include "apps/sntp/sntp.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

//#define LED_GPIO_PIN	5
#define BLINK_GPIO	2
#define BLINK_TIME	250
#define BLINK_WAIT_TIME 500
uint8_t BLINK_LED_ON = 0;
uint8_t toBlink = 0;

#define WIFI_CHANNEL_SWITCH_INTERVAL  (500)
#define WIFI_CHANNEL_MAX               (13)

#define SSID_LEN (32)
#define HASH_LEN (32)
#define BUFFSIZE 1024 //size of buffer used to send data to the server
#define NROWS 11 //max rows that buffer can have inside send_data, it can be changed modifying BUFFSIZE

static const char *TAG = "ESPLOG";

uint8_t level = 0, channel = 1;
int i =0;
//unsigned int numProbesReceived = 0;

typedef struct{
	uint8_t	address[6];
	char	ssid[SSID_LEN];
	int		timestamp;
	char	hash[HASH_LEN];
	int8_t	rssi;
	int		sn;
	char	htci[5];
} json_obj_t;

void jsonToString(char *str, int size, json_obj_t *obj);

void jsonToString(char *str, int size, json_obj_t *obj){
	memset(str, '\0', size);
	sprintf(str, "{");
	sprintf(str, "addr: \"%02x:%02x:%02x:%02x:%02x:%02x\"",
		obj->address[0], obj->address[1], obj->address[2], obj->address[3], obj->address[4], obj->address[5]);
	if(obj->ssid[0]!='\0'){
		sprintf(str, ", ssid: \"%s\"", obj->ssid);
	}
	sprintf(str, ", tstp: %d", obj->timestamp);
	sprintf(str, ", hash: \"%s\"", obj->hash);
	sprintf(str, ", rssi: %d", obj->rssi);
	sprintf(str, ", sn: %d", obj->sn);
	sprintf(str, ", htci: \"%s\"", obj->htci);
	sprintf(str, "}");
};

//static wifi_country_t wifi_country = WIFI_COUNTRY_EU;

uint8_t mask = 1;

typedef struct {
 unsigned version:2;
 unsigned type:2;
 unsigned subtype:4;
 unsigned to_ds:1;
 unsigned from_ds:1;
 unsigned more_frag:1;
 unsigned retry:1;
 unsigned power_mgmt:1;
 unsigned more_data:1;
 unsigned wep:1;
 unsigned order:1;
} frame_control_t;

typedef struct {
  unsigned frame_ctrl:16;
  unsigned duration_id:16;
  uint8_t addr1[6]; /* receiver address */ //6 BYTES
  uint8_t addr2[6]; /* sender address */
  uint8_t addr3[6]; /* filtering address */
  unsigned sequence_ctrl:16;
} wifi_ieee80211_mac_hdr_t;

// typedef struct {
//   wifi_ieee80211_mac_hdr_t hdr; // 24 byte
//   uint8_t *payload;
//   //uint8_t payload[0]; // network data ended with 4 bytes csum (CRC32)
// } wifi_ieee80211_packet_t;
typedef struct {
  wifi_ieee80211_mac_hdr_t hdr; // 24 byte
  uint8_t payload[0]; // network data ended with 4 bytes csum (CRC32)
} wifi_ieee80211_packet_t;

static void blink_task(void *pvParameter);
static TaskHandle_t xHandle_led = NULL;
static void time_init(void);
static void initialize_sntp(void);
static void obtain_time(void);

static esp_err_t event_handler(void *ctx, system_event_t *event);
static void wifi_init(void);
static void sniffer_init(void);
static void wifi_sniffer_set_channel(uint8_t channel);
static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);

// Event group
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

static void blink_task(void *pvParameter)
{
    gpio_pad_select_gpio(BLINK_GPIO);

    /* Set the GPIO as a push/pull output */
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    while(true){
    	toBlink = BLINK_LED_ON;
    	while(toBlink){
		    gpio_set_level(BLINK_GPIO, 1);
		    vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);
		    BLINK_LED_ON = 0;
		    gpio_set_level(BLINK_GPIO, 0);
		    vTaskDelay(BLINK_TIME / portTICK_PERIOD_MS);
		    toBlink = BLINK_LED_ON;
		}
	    gpio_set_level(BLINK_GPIO, 0);
	    vTaskDelay(BLINK_WAIT_TIME / portTICK_PERIOD_MS);
    }
}

esp_err_t event_handler(void *ctx, system_event_t *event)
{
	switch(event->event_id) {
		
	    case SYSTEM_EVENT_STA_START:
		esp_wifi_connect();
		break;
	    
		case SYSTEM_EVENT_STA_GOT_IP:
			xEventGroupSetBits(wifi_event_group, CONNECTED_BIT);
		break;
	    
		case SYSTEM_EVENT_STA_DISCONNECTED:
			xEventGroupClearBits(wifi_event_group, CONNECTED_BIT);
		break;
	    
		default:
		break;
	}
  return ESP_OK;
}

static void time_init()
{
	time_t now;
    struct tm timeinfo;
    char strftime_buf[64];

	ESP_LOGI(TAG, "Connecting to WiFi and getting time over NTP.");
	obtain_time();
	time(&now);  //update 'now' variable with current time

    //setting timezone to Greenwich
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();
    localtime_r(&now, &timeinfo);
    strftime(strftime_buf, sizeof(strftime_buf), "%c", &timeinfo);
    ESP_LOGI(TAG, "TIME INFO: The Greenwich date/time is: %s", strftime_buf);
}

static void obtain_time()
{
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 15;

    initialize_sntp();

    //wait for time to be set
    while(timeinfo.tm_year < (2016 - 1900) && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(2000 / portTICK_PERIOD_MS);
        time(&now);
        localtime_r(&now, &timeinfo);
    }

    if(retry >= retry_count){ //can't set time
    		ESP_LOGI(TAG, "No response from server after several time. Impossible to set current time");
    		esp_restart();
    }
}

static void initialize_sntp()
{
    sntp_setoperatingmode(SNTP_OPMODE_POLL); //automatically request time after 1h
    sntp_setservername(0, "pool.ntp.org");
    sntp_init();
}

static esp_err_t mqtt_event_handler(esp_mqtt_event_handle_t event)
{
    esp_mqtt_client_handle_t client = event->client;
    int msg_id = 0;
    // your_context_t *context = event->context;
    switch (event->event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
            msg_id = esp_mqtt_client_subscribe(client, "/topic/", 0);
            ESP_LOGI(TAG, "sent subscribe successful, msg_id=%d", msg_id);

            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
            break;

        case MQTT_EVENT_SUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
            msg_id = esp_mqtt_client_publish(client, "/topic/", "data", 0, 0, 0);
            ESP_LOGI(TAG, "sent publish successful, msg_id=%d", msg_id);
            break;
        case MQTT_EVENT_UNSUBSCRIBED:
            ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
            break;
        case MQTT_EVENT_DATA:
            ESP_LOGI(TAG, "MQTT_EVENT_DATA");
            printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
            printf("DATA=%.*s\r\n", event->data_len, event->data);
            break;
        case MQTT_EVENT_ERROR:
            ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
            break;
	default:
		break;
    }
    return ESP_OK;
}


static void mqtt_app_start(void)
{
    const esp_mqtt_client_config_t mqtt_cfg = {
        // .uri = "mqtts://api.emitter.io:443",    // for mqtt over ssl
        .uri = "mqtt://192.168.1.136:8080", //for mqtt over tcp
        // .uri = "ws://api.emitter.io:8080", //for mqtt over websocket
        // .uri = "wss://api.emitter.io:443", //for mqtt over websocket secure
        .event_handle = mqtt_event_handler,
    };

    ESP_LOGI(TAG, "[APP] Free memory: %d bytes", esp_get_free_heap_size());
    esp_mqtt_client_handle_t client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_start(client);
}

void sniffer_init(void){

	printf("initializing sniffer...\n");
	
	wifi_promiscuous_filter_t prom_filter;
  prom_filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&prom_filter);
  esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
  esp_wifi_set_promiscuous(true);
  
  printf("sniffer initialized.\n");
}

void wifi_init(void)
{
	wifi_event_group = xEventGroupCreate();
  tcpip_adapter_init();
  ESP_ERROR_CHECK( esp_event_loop_init(event_handler, NULL) );
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
  //ESP_ERROR_CHECK( esp_wifi_set_country(*wifi_country) ); /* set country for channel range [1, 13] */
  ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
  ESP_ERROR_CHECK( esp_wifi_set_mode(WIFI_MODE_APSTA) );
  wifi_sniffer_set_channel(10);


	wifi_config_t wifi_config = {
		.sta = {
		    .ssid = CONFIG_WIFI_SSID,
			.password = CONFIG_WIFI_PSW,
		},
	    };
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));


  esp_wifi_set_event_mask(WIFI_EVENT_MASK_ALL);

	ESP_ERROR_CHECK( esp_wifi_start() );


	printf("waiting for wifi network...");
	xEventGroupWaitBits(wifi_event_group, CONNECTED_BIT, false, true, portMAX_DELAY);
	printf(" connected (wifi)!\n");
	
	//tcpip_adapter_if_t tcp_arg_0 = TCPIP_ADAPTER_IF_STA;
	tcpip_adapter_ip_info_t tcp_arg_1;
	tcpip_adapter_get_ip_info(TCPIP_ADAPTER_IF_STA, &tcp_arg_1);
	printf("ip address: %d.%d.%d.%d\n", IP2STR(&tcp_arg_1.ip));

}

void wifi_sniffer_set_channel(uint8_t channel)
{
  esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
}

void wifi_sniffer_packet_handler(void* buff, wifi_promiscuous_pkt_type_t type)
{
  int i=0;
  const wifi_promiscuous_pkt_t *ppkt = (wifi_promiscuous_pkt_t *)buff;
  const wifi_ieee80211_packet_t *ipkt = (wifi_ieee80211_packet_t *)ppkt->payload;
  const wifi_ieee80211_mac_hdr_t *hdr = &ipkt->hdr;
  uint16_t frame_ctr = (uint16_t)(hdr->frame_ctrl);
  frame_control_t *frame_ctr_str = (frame_control_t *)&frame_ctr;

  if(frame_ctr_str->type!=0 || frame_ctr_str->subtype!=4){
	return;
  }
  
  BLINK_LED_ON = 1;
  
  printf("\033[22;31mPROBE REQUEST!\033[0m\n");
  printf("TIME=%02d, CH=%02d, RSSI=%02d,"
    " A1=%02x:%02x:%02x:%02x:%02x:%02x,"
    " A2=%02x:%02x:%02x:%02x:%02x:%02x,"
    " A3=%02x:%02x:%02x:%02x:%02x:%02x",
	ppkt->rx_ctrl.timestamp,
    ppkt->rx_ctrl.channel,
    ppkt->rx_ctrl.rssi,
    /* ADDR1 */
    hdr->addr1[0],hdr->addr1[1],hdr->addr1[2],
    hdr->addr1[3],hdr->addr1[4],hdr->addr1[5],
    /* ADDR2 */
    hdr->addr2[0],hdr->addr2[1],hdr->addr2[2],
    hdr->addr2[3],hdr->addr2[4],hdr->addr2[5],
    /* ADDR3 */
    hdr->addr3[0],hdr->addr3[1],hdr->addr3[2],
    hdr->addr3[3],hdr->addr3[4],hdr->addr3[5]
  );

  int l = (int)ipkt->payload[1];
  //printf(" Typ:%d-Len:%d SSID:", (int)ipkt->payload[0],(int)ipkt->payload[1]);
  if ((int)ipkt->payload[0]==0) {
	printf(", SSID=\"");
    for(i=2;i<l+2;i++){
      printf("%c",(char)ipkt->payload[i]);
    }
	printf("\"");
  }
  
  //printf(" #%u", ++numProbesReceived);
  
  printf("\n");
  
  return;
}

void app_main(){
		
	ESP_ERROR_CHECK(nvs_flash_init()); //initializing NVS (Non-Volatile Storage)
	
	xTaskCreate(&blink_task, "blink_task", configMINIMAL_STACK_SIZE, NULL, 5, &xHandle_led);
	
	wifi_init();
	
	time_init(); //initializing time (current data time)
	
	sniffer_init();

	mqtt_app_start();

  //vTaskDelay(WIFI_CHANNEL_SWITCH_INTERVAL / portTICK_PERIOD_MS);
}
