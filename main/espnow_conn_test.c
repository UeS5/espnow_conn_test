#include <stdio.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>

#include <esp_timer.h>
#include <esp_twai.h>
#include <esp_twai_onchip.h>
#include <esp_wifi.h>
#include <esp_now.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <nvs_flash.h>
#include <esp_log.h>
#include <esp_random.h>

#define DATA_SPEED                  0x41
#define DATA_ENGINE_LOAD            0x04

#define STATUS_ESP_NOW              (1 << 0)

#define TASK_ESP_NOW_RECEIVE        (1 << 0)
#define TASK_ESP_NOW_SEND_DATA      (1 << 1)
#define TASK_DISPLAY_REGISTERS      (1 << 2)

#define BROADCAST_MAC   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

EventGroupHandle_t STATUS_REG;  
EventGroupHandle_t TASK_REG;

TaskHandle_t vTask_start_esp_now_hdl;

QueueHandle_t queue_esp_now_recv;

static const char *TAG_ESP_NOW = "ESP-NOW"; 
static const char *TAG_MAIN = "MAIN";
static const char *TAG_RECEIVE = "RECEIVE"; 
static const char *TAG_SEND_DATA = "SEND DATA";  

uint8_t global_peer_addr[ESP_NOW_ETH_ALEN] = {0};

typedef struct {
	uint8_t tag;
	uint8_t data;
} esp_now_data_packet_t; 

typedef struct { 
	uint8_t source_addr[ESP_NOW_ETH_ALEN];
	uint8_t destination_addr[ESP_NOW_ETH_ALEN];
	uint8_t tag;
	uint8_t *pData; 
	int len;
} esp_now_data_packet_buff_t; 



// takes the MAC Addr and sends it to task register. 
void esp_now_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {

    esp_now_data_packet_buff_t pkt;
    esp_now_data_packet_buff_t *data_pkt = (esp_now_data_packet_buff_t *)data;
    
	if (!esp_now_info || !data || len <= 0) return; 

    memcpy(pkt.source_addr, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);
    memcpy(pkt.destination_addr, esp_now_info->des_addr, ESP_NOW_ETH_ALEN);
    pkt.tag = data_pkt->tag;
    pkt.len = len;

    pkt.pData = malloc(len);
    if (pkt.pData == NULL) return;
    memcpy(pkt.pData, data, len);
	
	xQueueSend(queue_esp_now_recv, &pkt, 0);   
		
} 


// ESP-NOW RECEIVE: 
void vTask_esp_now_receive(void *args) {

	uint8_t broadcast_addr[ESP_NOW_ETH_ALEN] = BROADCAST_MAC;
	esp_now_data_packet_buff_t pkt;
    uint8_t source_addr_buff[ESP_NOW_ETH_ALEN]; 
	
	for (;;) {
		xEventGroupWaitBits(TASK_REG, TASK_ESP_NOW_RECEIVE, pdFALSE, pdFALSE, portMAX_DELAY);
        
        while (xEventGroupGetBits(TASK_REG) & TASK_ESP_NOW_RECEIVE) {
            if (xQueueReceive(queue_esp_now_recv, &pkt, portMAX_DELAY) == pdTRUE) {

                free(pkt.pData); // free the heap 
                memcpy(source_addr_buff, pkt.source_addr, ESP_NOW_ETH_ALEN);

                if (memcmp(pkt.source_addr, broadcast_addr, ESP_NOW_ETH_ALEN) == 0) {
                   ESP_LOGI(TAG_RECEIVE, "Data Received from: %02X %02X %02X %02X %02X %02X", source_addr_buff[0] , source_addr_buff[1], source_addr_buff[2], source_addr_buff[3], source_addr_buff[4], source_addr_buff[5]); 
                }
            }
    	}
    }
}



void vTask_start_esp_now(void *pvParameters) {

    for (;;) {
        
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        ESP_LOGW(TAG_ESP_NOW, "STARTING ESP-NOW...");
    	esp_err_t ret = nvs_flash_init();  
    	ESP_ERROR_CHECK(ret); 								  
    	ESP_ERROR_CHECK(esp_netif_init());
    	ESP_ERROR_CHECK(esp_event_loop_create_default());
    	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT(); 				 
    	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    	ESP_ERROR_CHECK(esp_wifi_start());
        ESP_ERROR_CHECK(esp_wifi_set_channel(1, WIFI_SECOND_CHAN_NONE));
    	esp_err_t init = esp_now_init();
        esp_err_t err = esp_now_register_recv_cb(esp_now_recv_cb); 
    
       vTaskDelay(pdMS_TO_TICKS(50));

       if (err != ESP_OK) {
           ESP_LOGE(TAG_ESP_NOW, ">> Error: ESP-NOW callback could not be registered!");
           return; 
       }
    
       if (init != ESP_OK) {
             ESP_LOGE(TAG_ESP_NOW, ">> Error: ESP-NOW could not be initialised!");
            return;
        }
    
       ESP_LOGW(TAG_ESP_NOW, ">> Info: ESP-NOW initiated successfully!");
       xEventGroupSetBits(STATUS_REG, STATUS_ESP_NOW);
    }
	
}



// Broadcasts random data via ESP-NOW
void vTask_esp_now_send_data(void *args) {
    uint8_t randint = 0;
    bool data_type = 1;
    uint8_t broadcast_addr[ESP_NOW_ETH_ALEN] = BROADCAST_MAC;

	esp_now_data_packet_t pkt = {0}; 
	esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, broadcast_addr, ESP_NOW_ETH_ALEN);
	peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
	peer.encrypt = false;	 

    for (;;) {
        xEventGroupWaitBits(TASK_REG, TASK_ESP_NOW_SEND_DATA, pdFALSE, pdFALSE, portMAX_DELAY);
        
        while (xEventGroupGetBits(TASK_REG) & TASK_ESP_NOW_SEND_DATA) {

            if (!esp_now_is_peer_exist(peer.peer_addr)) {
                esp_now_add_peer(&peer);
            }

            randint = esp_random() & 0xFF;
            ESP_LOGI(TAG_SEND_DATA, "Random int: %u");
            pkt.data = randint; 
            
            if (data_type) {
                pkt.tag = DATA_SPEED;
                data_type = !data_type; 
            } else {
                pkt.tag = DATA_ENGINE_LOAD;
                data_type = !data_type;
            } 

            esp_err_t err = esp_now_send(peer.peer_addr, (uint8_t *)&pkt, sizeof(pkt));
 
            if (err == ESP_OK) {
               ESP_LOGW(TAG_SEND_DATA, ">> Data sent. Tag: %02X Data: %u", pkt.tag, pkt.data);
            } else {
               ESP_LOGE(TAG_SEND_DATA, ">> Error while sending data: %s", esp_err_to_name(err));
            }

            vTaskDelay(pdMS_TO_TICKS(330));

        }
    }
}


// Prints event group bits
void vTask_display_registers(void *args) {
	 
    uint32_t statusBits; 
    uint32_t taskBits;

	for (;;) {
        xEventGroupWaitBits(TASK_REG, TASK_DISPLAY_REGISTERS, pdFALSE, pdFALSE, portMAX_DELAY);

        while (xEventGroupGetBits(TASK_REG) & TASK_DISPLAY_REGISTERS) {
		    statusBits = xEventGroupGetBits(STATUS_REG);
            taskBits = xEventGroupGetBits(TASK_REG);
			
            ESP_LOGI(TAG_ESP_NOW, "Status & Task Registers: ");

		    for (int i = 0; i < 1; i++) {
    			if (statusBits & (1 << i)) {
				    printf("%i", 1);
				    fflush(stdout); 
			    } else {
				    printf("%i", 0);
				    fflush(stdout);
			    }
		    }

            printf("\n");

            for (int i = 0; i < 3; i++) {
                if (taskBits & (1 << i)) {
                    printf("%i", 1);
                    fflush(stdout); 
                } else {
                    printf("%i", 0);
                    fflush(stdout);
                }
            }

	    	printf("\n");
	    	fflush(stdout);	
    		vTaskDelay(pdMS_TO_TICKS(3000));	
    	}
    }
}


void app_main(void)
{

    STATUS_REG = xEventGroupCreate();
    TASK_REG = xEventGroupCreate();

    queue_esp_now_recv = xQueueCreate(1, sizeof(esp_now_data_packet_buff_t));
 
    xTaskCreate(vTask_start_esp_now, "ESP-NOW Start", 4096, NULL, 1, &vTask_start_esp_now_hdl);
    xTaskCreate(vTask_display_registers, "Display Registers", 2048, NULL, 1, NULL);
    xTaskCreate(vTask_esp_now_send_data, "Send Data", 4096, NULL, 1, NULL);
    xTaskCreate(vTask_esp_now_receive, "Receive", 4096, NULL, 2, NULL);

    // Start

    xTaskNotifyGive(vTask_start_esp_now_hdl);
    
    while ((xEventGroupGetBits(STATUS_REG) & STATUS_ESP_NOW) == 0) {
        ESP_LOGW(TAG_MAIN, ">> Info: Waiting for ESP-NOW to start!");
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }
    
    ESP_LOGI(TAG_MAIN, ">> Info: Starting to broadcast data"); 

    xEventGroupSetBits(TASK_REG, TASK_ESP_NOW_SEND_DATA);
    xEventGroupSetBits(TASK_REG, TASK_ESP_NOW_RECEIVE);
    xEventGroupSetBits(TASK_REG, TASK_DISPLAY_REGISTERS);

    ESP_LOGI(TAG_MAIN, ">> Info: End of Main..."); 
}
