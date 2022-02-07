/* ESP HTTP Client Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "protocol_examples_common.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "lvgl.h"
#include "lvgl_helpers.h"

#include "esp_http_client.h"

#define MAX_HTTP_RECV_BUFFER 512
#define MAX_HTTP_OUTPUT_BUFFER 4096
static const char *TAG = "HTTP_CLIENT";

#define LV_TICK_PERIOD_MS 1
#define LV_METER_MAX_VALUE 400

/* Root cert for howsmyssl.com, taken from howsmyssl_com_root_cert.pem

   The PEM file was extracted from the output of this command:
   openssl s_client -showcerts -connect www.howsmyssl.com:443 </dev/null

   The CA root cert is the last cert given in the chain of certs.

   To embed it in the app binary, the PEM file is named
   in the component.mk COMPONENT_EMBED_TXTFILES variable.
*/
extern const char howsmyssl_com_root_cert_pem_start[] asm("_binary_howsmyssl_com_root_cert_pem_start");
extern const char howsmyssl_com_root_cert_pem_end[]   asm("_binary_howsmyssl_com_root_cert_pem_end");

extern const char postman_root_cert_pem_start[] asm("_binary_postman_root_cert_pem_start");
extern const char postman_root_cert_pem_end[]   asm("_binary_postman_root_cert_pem_end");

typedef struct {
    int aqi;
    char dominentpol[6];
    char measure_time[20];
} aqi_result_t;

typedef struct {
    uint32_t color;
    char* description;
} aqi_text_t;

SemaphoreHandle_t xGuiSemaphore;
lv_obj_t* lmeter1;
lv_obj_t *label1, *label2, *label3;

static const aqi_text_t aqi_level_desc[6] = {
    {0x009966, "Good"},
    {0xffde33, "Moderate"},
    {0xff9933, "Unhealthy for Sensitive Groups"},
    {0xcc0033, "Unhealthy"},
    {0x660099, "Very Unhealthy"},
    {0x7e0023, "Hazardous"}
};

const aqi_text_t* aqi_to_desc(int aqi)
{
    int index = 0;

    if(aqi <= 50)         index = 0;
    else if(aqi <= 100)   index = 1;
    else if(aqi <= 150)   index = 2;
    else if(aqi <= 250)   index = 3;
    else if(aqi <= 300)   index = 4;
    else    index = 5;

    return &aqi_level_desc[index];
}

esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;  // Buffer to store response of http request from event handler
    static int output_len;       // Stores number of bytes read
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
            /*
             *  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
             *  However, event handler can also be used in case chunked encoding is used.
             */
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // If user_data buffer is configured, copy the response into the buffer
                if (evt->user_data) {
                    memcpy(evt->user_data + output_len, evt->data, evt->data_len);
                } else {
                    if (output_buffer == NULL) {
                        output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
                        output_len = 0;
                        if (output_buffer == NULL) {
                            ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                            return ESP_FAIL;
                        }
                    }
                    memcpy(output_buffer + output_len, evt->data, evt->data_len);
                }
                output_len += evt->data_len;
            }

            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            if (output_buffer != NULL) {
                // Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
                ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
                free(output_buffer);
                output_buffer = NULL;
            }
            output_len = 0;
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
            int mbedtls_err = 0;
            esp_err_t err = esp_tls_get_and_clear_last_error(evt->data, &mbedtls_err, NULL);
            if (err != 0) {
                if (output_buffer != NULL) {
                    free(output_buffer);
                    output_buffer = NULL;
                }
                output_len = 0;
                ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
                ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
            }
            break;
    }
    return ESP_OK;
}

static esp_err_t JSON_Parse(const cJSON * const root, aqi_result_t* result) {
    char *status = cJSON_GetObjectItem(root, "status")->valuestring;
    
    ESP_LOGI(TAG, "status=%s", status);
    if(!strcmp(status, "ok")) {
        cJSON *data = cJSON_GetObjectItem(root, "data");
        int aqi = cJSON_GetObjectItem(data, "aqi")->valueint;

        char *dominentpol = cJSON_GetObjectItem(data, "dominentpol")->valuestring;
        ESP_LOGI(TAG, "aqi=%d",aqi);
        ESP_LOGI(TAG, "dominentpol=%s", dominentpol);

        cJSON *time = cJSON_GetObjectItem(data, "time");
        char *measure_time = cJSON_GetObjectItem(time, "s")->valuestring;
        ESP_LOGI(TAG, "measure time=%s", measure_time);

        result->aqi = aqi;
        strcpy(result->dominentpol, dominentpol);
        strcpy(result->measure_time, measure_time);

        return ESP_OK;
    }
    return ESP_FAIL;
}


static void https_with_hostname_path(void)
{
    char local_response_buffer[MAX_HTTP_OUTPUT_BUFFER] = {0};
    int content_length = 0;
    int status_code = 0;
    static lv_style_t style_lmeter1;
    lv_style_init(&style_lmeter1);
    static char aqi_s[4];

    esp_http_client_config_t config = {
        .host = "api.waqi.info",
        .path = "/feed/hangzhou/?token=b2ed35a8cbad26bdb1607fe2d47a2f2aa4358ea3", // modified for your token
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .event_handler = _http_event_handler,
        .cert_pem = howsmyssl_com_root_cert_pem_start,
        .method = HTTP_METHOD_GET,
        .user_data = local_response_buffer,        // Pass address of local buffer to get response
        .timeout_ms = 1500,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);


    if (err == ESP_OK) {
        esp_err_t parse_err;
        aqi_result_t result = {0, "PM25", "2022-02-01 10:00:00"};

        status_code = esp_http_client_get_status_code(client);
        content_length = esp_http_client_get_content_length(client);
        ESP_LOGI(TAG, "HTTPS Status = %d, content_length = %d", status_code, content_length);
        ESP_LOG_BUFFER_CHAR_LEVEL(TAG, local_response_buffer, content_length, ESP_LOG_DEBUG);

        ESP_LOGI(TAG, "Deserialize.....");
        cJSON *root = cJSON_Parse(local_response_buffer);
		parse_err = JSON_Parse(root, &result);
        cJSON_Delete(root);

        if(parse_err == ESP_OK)
        {
            const aqi_text_t* desc = aqi_to_desc(result.aqi);
            ESP_LOGI(TAG,"set lmeter1 line color = %x",desc->color);
            lv_style_set_line_color(&style_lmeter1, LV_STATE_DEFAULT, lv_color_hex(desc->color));
            lv_style_set_scale_grad_color(&style_lmeter1, LV_STATE_DEFAULT, lv_color_hex(desc->color));
            itoa(result.aqi, aqi_s, 10);
            int lv_meter_value = result.aqi > LV_METER_MAX_VALUE ? LV_METER_MAX_VALUE : result.aqi;

            /* Try to take the semaphore, call lvgl related function on success */
            if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
                ESP_LOGI(TAG, "Update UI");
                lv_linemeter_set_value(lmeter1, lv_meter_value);
                lv_obj_add_style(lmeter1, LV_LINEMETER_PART_MAIN, &style_lmeter1);
                lv_label_set_text(label1, aqi_s);
                lv_label_set_text(label2, result.dominentpol);
                lv_label_set_text(label3, desc->description);

                xSemaphoreGive(xGuiSemaphore);
            }
        }
        else
            ESP_LOGE(TAG, "Error Parse JSON response");

    } else {
        ESP_LOGE(TAG, "Error perform http request %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
}


static void http_test_task(void *pvParameters)
{
    TickType_t xLastWakeTime;
    const TickType_t xFrequency = 6500; // 1min delay
    BaseType_t xWasDelayed;

    // Initialise the xLastWakeTime variable with the current time.
    xLastWakeTime = xTaskGetTickCount ();

    while(1) {
        https_with_hostname_path();
        ESP_LOGI(TAG, "Finish http example");
        // Wait for the next cycle.
        xWasDelayed = xTaskDelayUntil( &xLastWakeTime, xFrequency );
    }
    vTaskDelete(NULL);
}

static void lv_tick_task(void *arg) {
    (void) arg;

    lv_tick_inc(LV_TICK_PERIOD_MS);
}


static void create_ui_application(void)
{
    lv_obj_t* scr = lv_disp_get_scr_act(NULL);
    lmeter1 = lv_linemeter_create(scr, NULL);
    lv_linemeter_set_range(lmeter1, 0, LV_METER_MAX_VALUE); 
    lv_linemeter_set_value(lmeter1, 0);
    lv_obj_set_size(lmeter1, 200, 200);
    lv_obj_set_pos(lmeter1, 60, 10);

    label1 = lv_label_create(scr, NULL);
    lv_label_set_text(label1, "0");
    lv_label_set_long_mode(label1, LV_LABEL_LONG_BREAK);
    lv_label_set_align(label1, LV_LABEL_ALIGN_CENTER);

    static lv_style_t label1_style;
    lv_style_init(&label1_style);
    lv_style_set_text_font(&label1_style, LV_STATE_DEFAULT, &lv_font_montserrat_40);
    lv_obj_add_style(label1, LV_LABEL_PART_MAIN, &label1_style);
    lv_obj_set_size(label1, 120, 0);
    lv_obj_set_pos(label1, 100, 85);

    label2 = lv_label_create(scr, NULL);
    lv_label_set_text(label2, "Unknown");
    lv_label_set_long_mode(label2, LV_LABEL_LONG_BREAK);
    lv_label_set_align(label2, LV_LABEL_ALIGN_CENTER);

    static lv_style_t label2_style;
    lv_style_init(&label2_style);
    lv_style_set_text_font(&label2_style, LV_STATE_DEFAULT, &lv_font_montserrat_20);
    lv_obj_add_style(label2, LV_LABEL_PART_MAIN, &label2_style);
    lv_obj_set_size(label2, 100, 0);
    lv_obj_set_pos(label2, 110, 142);

    label3 = lv_label_create(scr, NULL);
    lv_label_set_text(label3, "Connecting...");
    lv_label_set_long_mode(label3, LV_LABEL_LONG_BREAK);
    lv_label_set_align(label3, LV_LABEL_ALIGN_LEFT);
    lv_obj_set_size(label3, 300, 0);
    lv_obj_set_pos(label3, 5, 215);
    
}

#if 0
static void create_demo_application(void)
{
    /* use a pretty small demo for monochrome displays */
    /* Get the current screen  */
    lv_obj_t * scr = lv_disp_get_scr_act(NULL);

    /*Create a Label on the currently active screen*/
    lv_obj_t * label1 =  lv_label_create(scr, NULL);

    /*Modify the Label's text*/
    lv_label_set_text(label1, "Hello\nworld");

    /* Align the Label to the center
     * NULL means align on parent (which is the screen now)
     * 0, 0 at the end means an x, y offset after alignment*/
    lv_obj_align(label1, NULL, LV_ALIGN_CENTER, 0, 0);
}
#endif

static void guiTask(void *pvParameter) {

    (void) pvParameter;
    xGuiSemaphore = xSemaphoreCreateMutex();

    lv_init();

    /* Initialize SPI or I2C bus used by the drivers */
    lvgl_driver_init();

    lv_color_t* buf1 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf1 != NULL);


    lv_color_t* buf2 = heap_caps_malloc(DISP_BUF_SIZE * sizeof(lv_color_t), MALLOC_CAP_DMA);
    assert(buf2 != NULL);

    static lv_disp_buf_t disp_buf;

    uint32_t size_in_px = DISP_BUF_SIZE;

    /* Initialize the working buffer depending on the selected display.
     * NOTE: buf2 == NULL when using monochrome displays. */
    lv_disp_buf_init(&disp_buf, buf1, buf2, size_in_px);

    lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.flush_cb = disp_driver_flush;

    disp_drv.buffer = &disp_buf;
    lv_disp_drv_register(&disp_drv);

    /* Create and start a periodic timer interrupt to call lv_tick_inc */
    const esp_timer_create_args_t periodic_timer_args = {
        .callback = &lv_tick_task,
        .name = "periodic_gui"
    };
    esp_timer_handle_t periodic_timer;
    ESP_ERROR_CHECK(esp_timer_create(&periodic_timer_args, &periodic_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(periodic_timer, LV_TICK_PERIOD_MS * 1000));

    /* Create the demo application */
    //create_demo_application();
    //create_calendar_application();
    create_ui_application();

    while (1) {
        /* Delay 1 tick (assumes FreeRTOS tick is 10ms */
        vTaskDelay(pdMS_TO_TICKS(10));

        /* Try to take the semaphore, call lvgl related function on success */
        if (pdTRUE == xSemaphoreTake(xGuiSemaphore, portMAX_DELAY)) {
            lv_task_handler();
            xSemaphoreGive(xGuiSemaphore);
       }
    }

    /* A task should NEVER return */
    free(buf1);
    free(buf2);

    vTaskDelete(NULL);
}


void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* This helper function configures Wi-Fi or Ethernet, as selected in menuconfig.
     * Read "Establishing Wi-Fi or Ethernet Connection" section in
     * examples/protocols/README.md for more information about this function.
     */
    ESP_ERROR_CHECK(example_connect());
    ESP_LOGI(TAG, "Connected to AP, begin http example");

    xTaskCreatePinnedToCore(guiTask, "gui", 4096*2, NULL, 0, NULL, 1);
    xTaskCreate(&http_test_task, "http_test_task", 8192, NULL, 5, NULL);
}
