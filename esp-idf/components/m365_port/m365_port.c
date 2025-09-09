#include "m365_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "arduino_compat.h"
#include "comms_port.h"
#include "protocol_state.h" // for ANSWER_HEADER and diagnostic globals

// Bus activity vars now declared in comms_port.h

static const char *TAG = "M365PORT";

void comms_port_init(void);
void start_comms_task(void);

static void diag_task(void *arg){
    const char *DTAG = "M365DIAG"; 
    while(1){
        ESP_LOGD(DTAG, "last addr=%02X cmd=%02X hz=%02X len=%u pay=%02X%02X%02X%02X%02X%02X%02X%02X", 
            g_lastDiagHeader.addr, g_lastDiagHeader.cmd, g_lastDiagHeader.hz, g_lastDiagHeader.len,
            g_lastDiagPayload[0],g_lastDiagPayload[1],g_lastDiagPayload[2],g_lastDiagPayload[3],
            g_lastDiagPayload[4],g_lastDiagPayload[5],g_lastDiagPayload[6],g_lastDiagPayload[7]);
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void m365_port_init(void) {
    ESP_LOGI(TAG, "Init port");
    comms_port_init();
}

void m365_port_start_tasks(void) {
    start_comms_task();
    xTaskCreate(diag_task, "diag_task", 3072, NULL, 3, NULL);
}
