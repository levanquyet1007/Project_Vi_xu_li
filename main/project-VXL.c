#include <stdio.h>
#include <stdlib.h>
#include <esp_system.h>
#include "esp_spi_flash.h"
#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <math.h>
#include <driver/adc.h>
#include <esp_http_server.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "driver/gpio.h"
#include <esp_http_client.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include "lwip/netdb.h"
#include "lwip/dns.h"

#include "esp_adc_cal.h"
#include "driver/adc.h"

#define WEB_SERVER "api.thingspeak.com"
#define WEB_PORT "80"

#define MQ135_PIN GPIO_NUM_32 
#define MQ135_BASE_VOLTAGE 200  // Điện áp cơ sở của MQ135 trong môi trường sạch (mV)
#define MQ135_BASE_RZERO 10000   // RZERO của MQ135 trong môi trường sạch (ohm)
#define MQ135_RZERO_COEFFICIENT 0.5 // Hệ số nhiệt độ RZERO (tính %/°C)
#define MQ135_VOLTAGE_DIVIDER 5   // Giá trị chia điện áp (tùy chỉnh tùy vào cấu hình mạch)

#define DEFAULT_VREF    1100    // Điện áp tham chiếu mặc định (1100 mV)
#define NO_OF_SAMPLES   64      // Số lượng mẫu ADC để lấy trung bình (64 mẫu)

#define WIFI_SSID "TP-Link_454C"
#define WIFI_PASS "63609875"

static const adc_channel_t channel = ADC_CHANNEL_5; // Kênh ADC được sử dụng (GPIO33)
static const adc_atten_t atten = ADC_ATTEN_DB_0;    // Độ rơi áp lớn nhất (0 dB)
static const adc_unit_t unit = ADC_UNIT_1;          // Đơn vị ADC (ADC1)

static esp_adc_cal_characteristics_t *adc_chars;
static const char *TAG = "MQ7";

char REQUEST[2000];
char SUBREQUEST[100];
char RESPONSE[2000];
char recv_buf[2000];

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        esp_wifi_connect();
    }
}

void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    esp_event_handler_instance_register(WIFI_EVENT, 
        ESP_EVENT_ANY_ID, 
        &wifi_event_handler, 
        NULL,
        &instance_any_id);

    esp_event_handler_instance_register(IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &wifi_event_handler,
        NULL,
        &instance_got_ip);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();
}
float estimate_CO2_ppm(float raw_value)
{
    // Chuyển đổi giá trị analog thành điện áp (mV)
    float voltage = (raw_value / 4095.0) * 3300 * MQ135_VOLTAGE_DIVIDER;

    // Chuyển đổi điện áp thành giá trị RZERO (ohm)
    float rzero = (MQ135_BASE_RZERO / (voltage / MQ135_BASE_VOLTAGE - 1));

        // Ước tính ppm CO2 dựa trên công thức tương quan 
    float ppm = 5000 * pow((rzero / MQ135_BASE_RZERO), MQ135_RZERO_COEFFICIENT);

    return ppm;
}

void initialize_adc()
{
    // Khởi tạo ADC
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(ADC1_CHANNEL_0, ADC_ATTEN_DB_0);
}
void connect_wifi()
{
    // Khởi tạo NVS Flash để lưu trữ cấu hình Wi-Fi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    // Kết nối Wi-Fi
    wifi_init_sta(); 
}
static void send_to_thingspeak(float ppm_CO2_0, float ppm_CO_0)
{
    const struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;
    struct in_addr *addr;
    int s, r;
    int err = getaddrinfo(WEB_SERVER, WEB_PORT, &hints, &res);

    if(err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup failed err=%d res=%p", err, res);
        return;
    }
    addr = &((struct sockaddr_in *)res->ai_addr)->sin_addr;
    ESP_LOGI(TAG, "DNS lookup succeeded. IP=%s", inet_ntoa(*addr));

    s = socket(res->ai_family, res->ai_socktype, 0);
        if(s < 0) {
            ESP_LOGE(TAG, "... Failed to allocate socket.");
            freeaddrinfo(res);
            return;
        }
        ESP_LOGI(TAG, "... allocated socket");

        if(connect(s, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "... socket connect failed errno=%d", errno);
            close(s);
            freeaddrinfo(res);
            return;
        }
        ESP_LOGI(TAG, "... connected");
        freeaddrinfo(res);
        sprintf(SUBREQUEST, "api_key=4XLRCO8NGQS2HPMX&field1=%f&field2=%f", ppm_CO2_0, ppm_CO_0);
        sprintf(REQUEST, "POST /update.json HTTP/1.1\nHost: api.thingspeak.com\nConnection: close\n Content-Type: application/x-www-form-urlencoded\nContent-Length:%d\n\n%s\n", strlen(SUBREQUEST), SUBREQUEST);
        
        if (write(s, REQUEST, strlen(REQUEST)) < 0) {
            ESP_LOGE(TAG, "... socket send failed");
            close(s);
            return;
        }
        ESP_LOGI(TAG, "Data send to Thingspeak");
        close(s);
}

void app_main()
{
    ESP_ERROR_CHECK( nvs_flash_init() );
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    // Khởi tạo Wi-Fi và kết nối 
    connect_wifi();
    initialize_adc();
    adc1_config_width(ADC_WIDTH_BIT_12);    // Độ phân giải ADC 12 bit (0-4095)
    adc1_config_channel_atten(channel, atten);

    // Hiệu chỉnh ADC
    adc_chars = calloc(1, sizeof(esp_adc_cal_characteristics_t));
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(unit, atten, ADC_WIDTH_BIT_12, DEFAULT_VREF, adc_chars);

    uint32_t adc_reading;
    float voltage;
    float ppm_CO;
    while (1)
    {
        // Đọc giá trị từ cảm biến MQ135
        float mq135_value = adc1_get_raw(ADC1_CHANNEL_4); // ADC1_CHANNEL_4 tương ứng với GPIO_NUM_32

        // Chuyển đổi giá trị đọc được thành ppm CO2
        float ppm_CO2 = estimate_CO2_ppm(mq135_value);
        // Đọc giá trị ADC từ cảm biến MQ7
        adc_reading = 0;
        for (int i = 0; i < NO_OF_SAMPLES; i++)
        {
            adc_reading += adc1_get_raw((adc1_channel_t)channel);
        }
        adc_reading /= NO_OF_SAMPLES;

        // Chuyển giá trị ADC sang đơn vị điện áp (V)
        voltage = esp_adc_cal_raw_to_voltage(adc_reading, adc_chars);

        // Tính giá trị ppm CO dựa trên đoạn cong tương quan từ datasheet của MQ7
        ppm_CO = (voltage - 0.22) / 0.1;
        send_to_thingspeak(ppm_CO2, ppm_CO);
        vTaskDelay(pdMS_TO_TICKS(5000)); // gửi data mỗi 5 giây
   }
}