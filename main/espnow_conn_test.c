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

#define STATUS_ESP_NOW              (1 << 0)
#define STATUS_ESP_NOW_CONN         (1 << 1)

#define BROADCAST_MAC   { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF }

EventGroupHandle_t STATUS_REG;  

TaskHandle_t vTask_register_mac_hdl;
TaskHandle_t vTask_pairing_hdl; 
TaskHandle_t vTask_send_data_hdl;

QueueHandle_t queue_esp_now_recv;

static const char *TAG_ESP_NOW = "ESP-NOW"; 

uint8_t global_peer_addr[ESP_NOW_ETH_ALEN] = {0};

typedef struct {
	uint8_t tag;
	uint8_t data;
} esp_now_data_packet_t; 





// takes the MAC Addr and sends it to task register. 
void esp_now_recv_cb(const esp_now_recv_info_t *esp_now_info, const uint8_t *data, int len) {
	
	if (!esp_now_info || !data || len <= 0) {
		return; 
	}
	
	// send mac to the mac register task. 
    uint8_t mac[ESP_NOW_ETH_ALEN] = {0};
    memcpy(mac, esp_now_info->src_addr, ESP_NOW_ETH_ALEN);

	xQueueOverwriteFromISR(queue_esp_now_recv, mac, pdFALSE);   
		
} 







void vTask_start_esp_now(void *pvParameters) {

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
        ESP_LOGE(TAG_ESP_NOW, "ESP-NOW callback could not be registered...");
        return; 
    }
    
    if (init != ESP_OK) {
         ESP_LOGE(TAG_ESP_NOW, "ESP-NOW could not be initialised...");
         return;
    }
    
    ESP_LOGW(TAG_ESP_NOW, "ESP-NOW INITIATED SUCCESSFULLY...");
    xEventGroupSetBits(STATUS_REG, STATUS_ESP_NOW);

	vTaskDelete(NULL); 
}






// queue_esp_now_recv den gelen mac adreslerini peer olarak ekliyor. 
void vTask_register_mac (void *args) {
	uint8_t mac[ESP_NOW_ETH_ALEN] = {0}; 
	esp_now_peer_info_t newPeer = {0};
	newPeer.ifidx =WIFI_IF_STA;
    newPeer.channel = 0;
	newPeer.encrypt	= false;
	
	ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    
    ESP_LOGW(TAG_ESP_NOW, "vTask_register_mac IS TRIGGERRED...");

    for (;;) {

        if (xQueueReceive(queue_esp_now_recv, mac, portMAX_DELAY) == pdPASS) {
            
            ESP_LOGI(TAG_ESP_NOW, "queue_esp_now_recv TRIGGERED...");

            esp_err_t err = esp_now_add_peer(&newPeer);

            if (err == ESP_OK) {

                ESP_LOGW(TAG_ESP_NOW, "A new global address has been added: %02X %02X %02X %02X %02X %02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
                xEventGroupSetBits(STATUS_REG, STATUS_ESP_NOW_CONN);
                memcpy(mac, global_peer_addr, ESP_NOW_ETH_ALEN);
                xTaskNotifyGive(vTask_send_data_hdl);	
                return; // exit the task after registering the first mac address.
                
            } else if (err == ESP_ERR_ESPNOW_EXIST) {
                ESP_LOGE(TAG_ESP_NOW, "This peer already exists: %02X %02X %02X %02X %02X %02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            } else {
                ESP_LOGE(TAG_ESP_NOW, "An error occured while adding a new peer: %s", esp_err_to_name(err));
            }
	    }

        vTaskDelay(pdMS_TO_TICKS(100));
    }	 
    
    ESP_LOGW(TAG_ESP_NOW, "vTask_register_mac IS EXITING...");

    vTaskDelete(NULL); 
}






// esp_now aktiflestirildikten hemen sonra basliyor. amaci broadcast yapip herhangi bir mac adresi almak. ack sinyali geldigi an durur. yerini check conna birakir. 
void vTask_pairing(void *pvParameters) {
	uint8_t broadcast_addr[ESP_NOW_ETH_ALEN] = BROADCAST_MAC;
	esp_now_peer_info_t broadcast = {0};
	memcpy(broadcast.peer_addr, broadcast_addr, ESP_NOW_ETH_ALEN);
	broadcast.ifidx = WIFI_IF_STA;
	broadcast.channel = 0;
	broadcast.encrypt = false;

	ulTaskNotifyTake(pdTRUE, portMAX_DELAY); 

	esp_err_t err_peer = esp_now_add_peer(&broadcast);
    
    if (err_peer != ESP_OK) {
        ESP_LOGE(TAG_ESP_NOW, "Could not add the broadcast peer. Returning from the task.");
        return;
    }
    
	esp_now_data_packet_t broadcast_pkt;
    broadcast_pkt.tag = 31;
    broadcast_pkt.data = 0; 

	for (;;) {
        
		esp_err_t err = esp_now_send(broadcast_addr, (const uint8_t *)&broadcast_pkt, sizeof(broadcast_pkt));

        if (err == ESP_OK) { 
            ESP_LOGI(TAG_ESP_NOW, "Broadcast packet sent successfully.");
        } else {
            ESP_LOGE(TAG_ESP_NOW, "Send error: %s", esp_err_to_name(err));
        }
		
		if (xEventGroupGetBits(STATUS_REG) & STATUS_ESP_NOW_CONN) { 
            ESP_LOGW(TAG_ESP_NOW, "EXITING PAIRING PROCESS..."); 
            break;
        }  
			
		vTaskDelay(pdMS_TO_TICKS(1000));	
	}		

	vTaskDelete(NULL); 
}





// queue_esp_now dan paketleri alip global_peer_addr ye yollar. 
void vTask_esp_now_send_data(void *args) {
    uint8_t randint = 0; 
	esp_now_data_packet_t pkt; 
	esp_now_peer_info_t peer;
	peer.ifidx = WIFI_IF_STA;
    peer.channel = 0;
	peer.encrypt = false;	
	
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY); 
    ESP_LOGW(TAG_ESP_NOW, "vTask_esp_now_send_data is triggerred!"); 

    for (;;) {
        
        if (memcmp(global_peer_addr, "\0\0\0\0\0\0", ESP_NOW_ETH_ALEN) == 0) {
            ESP_LOGE(TAG_ESP_NOW, "Global_peer_addr is EMPTY"); 
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        memcpy(peer.peer_addr, global_peer_addr, ESP_NOW_ETH_ALEN);
        randint = esp_random() & 0xFF;
        pkt.tag = 1; 
        pkt.data = randint; 

        esp_err_t err = esp_now_send(peer.peer_addr, (uint8_t *)&pkt, sizeof(pkt));

        if (err == ESP_OK) {
            ESP_LOGW(TAG_ESP_NOW, "DATA HAS BEEN TRANSMITTED SUCCESSFULLY...");
        } else {
            ESP_LOGE(TAG_ESP_NOW, "ERROR SENDING DATA: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}


// Prints event group bits
void vTask_display_registers(void *args) {
	
	uint32_t statusBits = xEventGroupGetBits(STATUS_REG);

	for (;;) {
		
		statusBits = xEventGroupGetBits(STATUS_REG);
				
		for (int i = 0; i < 2; i++) {
			if (statusBits & (1 << i)) {
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


void app_main(void)
{

    STATUS_REG = xEventGroupCreate();
    queue_esp_now_recv = xQueueCreate(1, ESP_NOW_ETH_ALEN);

    printf("Start of main\n"); 
    xTaskCreate(vTask_start_esp_now, "ESP-NOW Start", 4096, NULL, 1, NULL);
    xTaskCreate(vTask_display_registers, "Display Registers", 2048, NULL, 1, NULL);
    
    while ((xEventGroupGetBits(STATUS_REG) & STATUS_ESP_NOW) == 0) {
        ESP_LOGW(TAG_ESP_NOW, "WAITING FOR ESP-NOW TO START");
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }

    ESP_LOGW(TAG_ESP_NOW, "STARTING PAIRING MODE AND REGISTER MAC TASK");
    xTaskCreate(vTask_pairing, "Pairing", 4096, NULL, 1, &vTask_pairing_hdl);
    xTaskCreate(vTask_register_mac, "Register MAC", 4096, NULL, 1, &vTask_register_mac_hdl);
    vTaskDelay(pdMS_TO_TICKS(500));
    xTaskNotifyGive(vTask_pairing_hdl);
    xTaskNotifyGive(vTask_register_mac_hdl);

    uint8_t counter = 0;
    while ((xEventGroupGetBits(STATUS_REG) & STATUS_ESP_NOW_CONN) == 0) {

        if (counter > 5) {
            ESP_LOGW(TAG_ESP_NOW, "WAITING FOR CONNECTION TO BE ESTABLISHED...");
            counter = 0;
        }

        counter++;
        vTaskDelay(pdMS_TO_TICKS(500)); 
    }

    xTaskCreate(vTask_esp_now_send_data, "Send Data", 4096, NULL, 1, &vTask_send_data_hdl);
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGW(TAG_ESP_NOW, "STARTING TO SEND DATA...");
    xTaskNotifyGive(vTask_send_data_hdl);

}
