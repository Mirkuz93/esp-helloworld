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
#include "mqtt_config.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#define LED_GPIO_PIN                     5

#define WIFI_CHANNEL_SWITCH_INTERVAL  (500)
#define WIFI_CHANNEL_MAX               (13)

#define WIFI_SSID "MSM-Wireless"
#define WIFI_PASS "CiaoComeStai80171"

static const char *TAG = "ESPLOG";

uint8_t level = 0, channel = 1;
int i =0;

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

static esp_err_t event_handler(void *ctx, system_event_t *event);
static void wifi_sniffer_init(void);
static void wifi_sniffer_set_channel(uint8_t channel);
static void wifi_sniffer_packet_handler(void *buff, wifi_promiscuous_pkt_type_t type);

// Event group
static EventGroupHandle_t wifi_event_group;
const int CONNECTED_BIT = BIT0;

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

void wifi_sniffer_init(void)
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
		    .ssid = WIFI_SSID,
		    .password = WIFI_PASS,
		},
	    };
	ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));


  esp_wifi_set_event_mask(WIFI_EVENT_MASK_ALL);

  wifi_promiscuous_filter_t prom_filter;
  prom_filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
  esp_wifi_set_promiscuous_filter(&prom_filter);
  esp_wifi_set_promiscuous_rx_cb(&wifi_sniffer_packet_handler);
  esp_wifi_set_promiscuous(true);

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
  printf("\n");
  
  return;
}

void app_main(){
		
	ESP_ERROR_CHECK(nvs_flash_init()); //initializing NVS (Non-Volatile Storage)
	
	wifi_sniffer_init();

	mqtt_app_start();

  //vTaskDelay(WIFI_CHANNEL_SWITCH_INTERVAL / portTICK_PERIOD_MS);
}
