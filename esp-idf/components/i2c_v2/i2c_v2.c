#include "i2c_v2.h"
#include "esp_log.h"
#include "esp_check.h"

static const char *TAG="I2C_V2";
static i2c_master_bus_handle_t s_bus;
static uint32_t s_speed = 100000; // default

typedef struct { uint8_t addr; i2c_master_dev_handle_t h; } dev_slot_t;
static dev_slot_t s_devs[4];

esp_err_t i2c_v2_init(int sda,int scl,uint32_t hz){
    if(s_bus) return ESP_OK;
    if(hz>=10000 && hz <= 800000) s_speed = hz; // basic sanity range
    i2c_master_bus_config_t cfg={
        .i2c_port=I2C_NUM_0,
        .sda_io_num=sda,
        .scl_io_num=scl,
        .clk_source=I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt=7,
        .flags.enable_internal_pullup=true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&cfg,&s_bus),TAG,"bus create fail");
    return ESP_OK;
}

i2c_master_dev_handle_t i2c_v2_get_dev(uint8_t addr){
    for(int i=0;i<4;i++) if(s_devs[i].h && s_devs[i].addr==addr) return s_devs[i].h;
    for(int i=0;i<4;i++) if(!s_devs[i].h){
        i2c_device_config_t dc={
            .device_address=addr,
            .scl_speed_hz=s_speed,
        };
        if(i2c_master_bus_add_device(s_bus,&dc,&s_devs[i].h)==ESP_OK){ s_devs[i].addr=addr; return s_devs[i].h; }
        break;
    }
    return NULL;
}
