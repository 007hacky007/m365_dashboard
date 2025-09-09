#include "aht10.h"
#if CFG_AHT10_ENABLE
#include "i2c_v2.h"
#include "esp_log.h"
#include "arduino_compat.h" // millis(), delay

float g_ahtTempC = NAN;
float g_ahtHum = NAN;
bool  g_ahtPresent = false;

static const char *TAG="AHT10";

static bool write_cmd(uint8_t c0,uint8_t c1,uint8_t c2){
    i2c_master_dev_handle_t dev = i2c_v2_get_dev(AHT10_I2C_ADDRESS);
    if(!dev) return false;
    uint8_t buf[3]={c0,c1,c2};
    i2c_master_transmit(dev, buf, 3, 100); // ignore err explicit for brevity
    return true;
}

bool aht10_init(void){
#ifdef SIM_MODE
    g_ahtPresent = true; g_ahtTempC=22.5f; g_ahtHum=45.0f; return true;
#else
    // ensure device handle created
    if(!i2c_v2_get_dev(AHT10_I2C_ADDRESS)) return false;
    // Soft reset 0xBA
    uint8_t rst=0xBA; i2c_master_transmit(i2c_v2_get_dev(AHT10_I2C_ADDRESS), &rst, 1, 100);
    delay(20);
    if(!write_cmd(0xE1,0x08,0x00)){ g_ahtPresent=false; ESP_LOGW(TAG,"init calibrate fail"); return false; }
    delay(10);
    g_ahtPresent=true; return true;
#endif
}

bool aht10_read(float *tempC,float *rh){
    if(!g_ahtPresent) return false;
#ifdef SIM_MODE
    uint32_t t=millis();
    g_ahtTempC = 22.0f + 3.0f * sinf((float)t/7000.0f);
    g_ahtHum   = 40.0f + 10.0f * sinf((float)t/11000.0f + 1.0f);
    if(tempC) *tempC=g_ahtTempC; if(rh) *rh=g_ahtHum; return true;
#else
    if(!write_cmd(0xAC,0x33,0x00)){ g_ahtPresent=false; return false; }
    delay(85);
    i2c_master_dev_handle_t dev=i2c_v2_get_dev(AHT10_I2C_ADDRESS); if(!dev) return false;
    uint8_t rx[6]={0};
    int ret=i2c_master_receive(dev, rx, 6, 100);
    if(ret!=ESP_OK) return false;
    if(rx[0] & 0x80) return false; // busy
    uint32_t rawHum = ((uint32_t)(rx[0] & 0x3F) << 16) | ((uint32_t)rx[1] << 8) | rx[2];
    uint32_t rawTemp = ((uint32_t)rx[3] << 16) | ((uint32_t)rx[4] << 8) | rx[5];
    rawTemp >>= 4;
    float RHum = (rawHum * 100.0f) / 1048576.0f;
    float TC   = ((rawTemp * 200.0f) / 1048576.0f) - 50.0f;
    g_ahtTempC = TC; g_ahtHum = RHum;
    if(tempC) *tempC=TC; if(rh) *rh=RHum; return true;
#endif
}
#endif // CFG_AHT10_ENABLE
