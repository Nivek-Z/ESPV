#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_camera.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "index_html.h"

#define TAG "espv"
#define STREAM_BOUNDARY "espvframe"
#define DEFAULT_ADMIN_KEY "ESPVsetup123"

typedef struct {
    char ssid[33];
    char wifi_password[65];
    char admin_key[17];
    char resolution[8];
    uint8_t quality;
    bool mirror;
    bool flip;
} app_config_t;

static app_config_t s_config = {.resolution = "vga", .quality = 14};
static char s_ap_ssid[24];
static char s_ip[16] = "0.0.0.0";
static bool s_camera_ok;
static bool s_sta_connected;
static bool s_ap_running;
static bool s_manual_ap;
static bool s_stream_active;
static uint32_t s_stream_fps;
static uint32_t s_frames_sent;
static esp_timer_handle_t s_fallback_timer;
static httpd_handle_t s_web_server;
static httpd_handle_t s_stream_server;

static void load_string(nvs_handle_t nvs, const char *key, char *dst, size_t capacity)
{
    size_t length = capacity;
    if (nvs_get_str(nvs, key, dst, &length) != ESP_OK) {
        dst[0] = '\0';
    }
}

static void load_config(void)
{
    nvs_handle_t nvs;
    ESP_ERROR_CHECK(nvs_open("espv", NVS_READWRITE, &nvs));
    load_string(nvs, "ssid", s_config.ssid, sizeof(s_config.ssid));
    load_string(nvs, "wifi_pass", s_config.wifi_password, sizeof(s_config.wifi_password));
    load_string(nvs, "admin", s_config.admin_key, sizeof(s_config.admin_key));
    load_string(nvs, "res", s_config.resolution, sizeof(s_config.resolution));
    if (strcmp(s_config.resolution, "qvga") != 0 &&
        strcmp(s_config.resolution, "vga") != 0 &&
        strcmp(s_config.resolution, "svga") != 0) {
        strcpy(s_config.resolution, "vga");
    }
    uint8_t quality;
    if (nvs_get_u8(nvs, "quality", &quality) == ESP_OK && quality >= 8 && quality <= 30) {
        s_config.quality = quality;
    }
    uint8_t value;
    if (nvs_get_u8(nvs, "mirror", &value) == ESP_OK) s_config.mirror = value != 0;
    if (nvs_get_u8(nvs, "flip", &value) == ESP_OK) s_config.flip = value != 0;

    if (strlen(s_config.admin_key) < 8) {
        snprintf(s_config.admin_key, sizeof(s_config.admin_key), "%s", DEFAULT_ADMIN_KEY);
        ESP_ERROR_CHECK(nvs_set_str(nvs, "admin", s_config.admin_key));
        ESP_ERROR_CHECK(nvs_commit(nvs));
    }
    nvs_close(nvs);
}

static framesize_t frame_size_for(const char *resolution)
{
    if (strcmp(resolution, "qvga") == 0) return FRAMESIZE_QVGA;
    if (strcmp(resolution, "svga") == 0) return FRAMESIZE_SVGA;
    return FRAMESIZE_VGA;
}

static void init_camera(void)
{
    camera_config_t cam = {
        .pin_pwdn = -1,
        .pin_reset = -1,
        .pin_xclk = 15,
        .pin_sccb_sda = 4,
        .pin_sccb_scl = 5,
        .pin_d0 = 11,
        .pin_d1 = 9,
        .pin_d2 = 8,
        .pin_d3 = 10,
        .pin_d4 = 12,
        .pin_d5 = 18,
        .pin_d6 = 17,
        .pin_d7 = 16,
        .pin_vsync = 6,
        .pin_href = 7,
        .pin_pclk = 13,
        .xclk_freq_hz = 9411764,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = frame_size_for(s_config.resolution),
        .jpeg_quality = s_config.quality,
        .fb_count = 2,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };
    esp_err_t err = esp_camera_init(&cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: %s", esp_err_to_name(err));
        return;
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        // OV2640 sensor bank CLKRC bit 7 doubles the internal clock.
        // Keep the external XCLK below the measured STA failure range.
        if (sensor->id.PID == OV2640_PID && sensor->set_reg(sensor, 0x111, 0x80, 0x80) != 0) {
            ESP_LOGW(TAG, "OV2640 clock doubling failed; using base clock");
        }
        sensor->set_hmirror(sensor, s_config.mirror);
        sensor->set_vflip(sensor, s_config.flip);
    }
    s_camera_ok = true;
    ESP_LOGI(TAG, "Camera ready: %s, JPEG quality %u", s_config.resolution, s_config.quality);
}

static void configure_ap(void)
{
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "%s", s_ap_ssid);
    snprintf((char *)ap.ap.password, sizeof(ap.ap.password), "%s", s_config.admin_key);
    ap.ap.ssid_len = strlen(s_ap_ssid);
    ap.ap.channel = 1;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
}

static void fallback_to_ap(void *arg)
{
    if (__atomic_load_n(&s_sta_connected, __ATOMIC_RELAXED) ||
        __atomic_load_n(&s_manual_ap, __ATOMIC_RELAXED)) return;
    ESP_LOGW(TAG, "Wi-Fi unavailable; enabling setup AP");
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    configure_ap();
    __atomic_store_n(&s_ap_running, true, __ATOMIC_RELAXED);
    ESP_LOGW(TAG, "Setup AP %s, password %s, http://192.168.4.1/", s_ap_ssid, s_config.admin_key);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    if (base == WIFI_EVENT && event == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) {
        __atomic_store_n(&s_sta_connected, false, __ATOMIC_RELAXED);
        strcpy(s_ip, "0.0.0.0");
        if (!__atomic_load_n(&s_manual_ap, __ATOMIC_RELAXED)) esp_wifi_connect();
    } else if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        if (__atomic_load_n(&s_manual_ap, __ATOMIC_RELAXED)) return;
        const ip_event_got_ip_t *got = data;
        snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&got->ip_info.ip));
        __atomic_store_n(&s_sta_connected, true, __ATOMIC_RELAXED);
        if (s_fallback_timer && esp_timer_is_active(s_fallback_timer)) {
            esp_timer_stop(s_fallback_timer);
        }
        if (__atomic_load_n(&s_ap_running, __ATOMIC_RELAXED) &&
            !__atomic_load_n(&s_manual_ap, __ATOMIC_RELAXED)) {
            esp_wifi_set_mode(WIFI_MODE_STA);
            __atomic_store_n(&s_ap_running, false, __ATOMIC_RELAXED);
        }
        ESP_LOGI(TAG, "Wi-Fi connected: http://%s/  stream: http://%s:81/stream", s_ip, s_ip);
    }
}

static void boot_button_task(void *arg)
{
    const gpio_config_t button = {
        .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&button));
    unsigned held_ticks = 0;
    bool handled = false;
    while (true) {
        if (gpio_get_level(GPIO_NUM_0) == 0) {
            if (held_ticks < 30) ++held_ticks;
            if (held_ticks == 30 && !handled) {
                handled = true;
                __atomic_store_n(&s_manual_ap, true, __ATOMIC_RELAXED);
                if (s_fallback_timer && esp_timer_is_active(s_fallback_timer)) {
                    esp_timer_stop(s_fallback_timer);
                }
                esp_err_t err = esp_wifi_set_mode(WIFI_MODE_AP);
                if (err == ESP_OK) {
                    configure_ap();
                    __atomic_store_n(&s_sta_connected, false, __ATOMIC_RELAXED);
                    strcpy(s_ip, "0.0.0.0");
                    __atomic_store_n(&s_ap_running, true, __ATOMIC_RELAXED);
                    ESP_LOGW(TAG, "Manual setup AP %s, password %s", s_ap_ssid, s_config.admin_key);
                } else {
                    __atomic_store_n(&s_manual_ap, false, __ATOMIC_RELAXED);
                    ESP_LOGE(TAG, "Cannot enable manual AP: %s", esp_err_to_name(err));
                }
            }
        } else {
            held_ticks = 0;
            handled = false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void init_wifi(void)
{
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ESPV-%02X%02X%02X", mac[3], mac[4], mac[5]);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    if (s_config.ssid[0]) {
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, s_config.ssid, strlen(s_config.ssid));
        snprintf((char *)station.sta.password, sizeof(station.sta.password), "%s", s_config.wifi_password);
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
        ESP_ERROR_CHECK(esp_wifi_start());
        const esp_timer_create_args_t timer_args = {
            .callback = fallback_to_ap,
            .name = "ap_fallback",
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_fallback_timer));
        ESP_ERROR_CHECK(esp_timer_start_once(s_fallback_timer, 15000000));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
        configure_ap();
        ESP_ERROR_CHECK(esp_wifi_start());
        __atomic_store_n(&s_ap_running, true, __ATOMIC_RELAXED);
        ESP_LOGW(TAG, "Setup AP %s, password %s, http://192.168.4.1/", s_ap_ssid, s_config.admin_key);
    }
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, (const char *)index_html, sizeof(index_html) - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    if (!json) return httpd_resp_send_500(req);
    cJSON_AddStringToObject(json, "ssid", s_config.ssid);
    cJSON_AddStringToObject(json, "ap_ssid", s_ap_ssid);
    cJSON_AddStringToObject(json, "ip", __atomic_load_n(&s_sta_connected, __ATOMIC_RELAXED) ? s_ip : "192.168.4.1");
    cJSON_AddBoolToObject(json, "connected", __atomic_load_n(&s_sta_connected, __ATOMIC_RELAXED));
    cJSON_AddBoolToObject(json, "ap", __atomic_load_n(&s_ap_running, __ATOMIC_RELAXED));
    wifi_ap_record_t connected_ap;
    if (esp_wifi_sta_get_ap_info(&connected_ap) == ESP_OK) {
        cJSON_AddNumberToObject(json, "rssi", connected_ap.rssi);
        cJSON_AddNumberToObject(json, "channel", connected_ap.primary);
    }
    cJSON_AddBoolToObject(json, "camera", s_camera_ok);
    cJSON_AddBoolToObject(json, "stream_active", __atomic_load_n(&s_stream_active, __ATOMIC_RELAXED));
    cJSON_AddNumberToObject(json, "fps", __atomic_load_n(&s_stream_fps, __ATOMIC_RELAXED));
    cJSON_AddNumberToObject(json, "frames", __atomic_load_n(&s_frames_sent, __ATOMIC_RELAXED));
    cJSON_AddStringToObject(json, "resolution", s_config.resolution);
    cJSON_AddNumberToObject(json, "quality", s_config.quality);
    cJSON_AddBoolToObject(json, "mirror", s_config.mirror);
    cJSON_AddBoolToObject(json, "flip", s_config.flip);
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!body) return httpd_resp_send_500(req);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_sendstr(req, body);
    free(body);
    return result;
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(900));
    esp_restart();
}

static esp_err_t config_handler(httpd_req_t *req)
{
    char key[24];
    if (httpd_req_get_hdr_value_str(req, "X-Config-Key", key, sizeof(key)) != ESP_OK ||
        strcmp(key, s_config.admin_key) != 0) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Wrong configuration password");
        return ESP_FAIL;
    }
    if (req->content_len < 2 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid body size");
        return ESP_FAIL;
    }
    char body[513];
    size_t received = 0;
    while (received < req->content_len) {
        int n = httpd_req_recv(req, body + received, req->content_len - received);
        if (n <= 0) {
            httpd_resp_send_err(req, HTTPD_408_REQ_TIMEOUT, "Request incomplete");
            return ESP_FAIL;
        }
        received += n;
    }
    body[received] = '\0';
    cJSON *json = cJSON_Parse(body);
    if (!json) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(json, "ssid");
    const cJSON *password = cJSON_GetObjectItemCaseSensitive(json, "wifi_password");
    const cJSON *new_admin_key = cJSON_GetObjectItemCaseSensitive(json, "new_admin_key");
    const cJSON *resolution = cJSON_GetObjectItemCaseSensitive(json, "resolution");
    const cJSON *quality = cJSON_GetObjectItemCaseSensitive(json, "quality");
    const cJSON *mirror = cJSON_GetObjectItemCaseSensitive(json, "mirror");
    const cJSON *flip = cJSON_GetObjectItemCaseSensitive(json, "flip");
    if (!cJSON_IsString(ssid) || !cJSON_IsString(password) ||
        !cJSON_IsString(resolution) || !cJSON_IsNumber(quality) ||
        !cJSON_IsBool(mirror) || !cJSON_IsBool(flip) ||
        (new_admin_key && (!cJSON_IsString(new_admin_key) ||
         (new_admin_key->valuestring[0] &&
          (strlen(new_admin_key->valuestring) < 8 || strlen(new_admin_key->valuestring) > 16)))) ||
        strlen(ssid->valuestring) > 32 || strlen(password->valuestring) > 63 ||
        (password->valuestring[0] && strlen(password->valuestring) < 8) ||
        (strcmp(resolution->valuestring, "qvga") != 0 &&
         strcmp(resolution->valuestring, "vga") != 0 &&
         strcmp(resolution->valuestring, "svga") != 0) ||
        quality->valuedouble < 8 || quality->valuedouble > 30 ||
        quality->valuedouble != quality->valueint) {
        cJSON_Delete(json);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid settings");
        return ESP_FAIL;
    }

    bool same_ssid = strcmp(ssid->valuestring, s_config.ssid) == 0;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("espv", NVS_READWRITE, &nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "ssid", ssid->valuestring);
        if (err == ESP_OK && (password->valuestring[0] || !same_ssid)) {
            err = nvs_set_str(nvs, "wifi_pass", password->valuestring);
        }
        if (err == ESP_OK && new_admin_key && new_admin_key->valuestring[0]) {
            err = nvs_set_str(nvs, "admin", new_admin_key->valuestring);
        }
        if (err == ESP_OK) err = nvs_set_str(nvs, "res", resolution->valuestring);
        if (err == ESP_OK) err = nvs_set_u8(nvs, "quality", quality->valueint);
        if (err == ESP_OK) err = nvs_set_u8(nvs, "mirror", cJSON_IsTrue(mirror));
        if (err == ESP_OK) err = nvs_set_u8(nvs, "flip", cJSON_IsTrue(flip));
        if (err == ESP_OK) err = nvs_commit(nvs);
        nvs_close(nvs);
    }
    cJSON_Delete(json);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Could not save settings");
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"restarting\":true}");
    xTaskCreate(restart_task, "restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!s_camera_ok) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Camera unavailable");
        return ESP_FAIL;
    }
    bool expected = false;
    if (!__atomic_compare_exchange_n(&s_stream_active, &expected, true, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "Stream already in use");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Stream client connected");
    int fd = httpd_req_to_sockfd(req);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=" STREAM_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    uint64_t window_start = esp_timer_get_time();
    uint32_t window_frames = 0;
    unsigned capture_failures = 0;
    esp_err_t result = ESP_OK;
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            if (++capture_failures >= 5) {
                ESP_LOGW(TAG, "Camera returned no frames five times");
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        capture_failures = 0;
        if (fb->format != PIXFORMAT_JPEG) {
            esp_camera_fb_return(fb);
            result = ESP_FAIL;
            break;
        }
        char header[112];
        int length = snprintf(header, sizeof(header),
                              "\r\n--" STREAM_BOUNDARY "\r\n"
                              "Content-Type: image/jpeg\r\n"
                              "Content-Length: %u\r\n\r\n", (unsigned)fb->len);
        result = httpd_resp_send_chunk(req, header, length);
        if (result == ESP_OK) result = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        esp_camera_fb_return(fb);
        if (result != ESP_OK) break;
        __atomic_add_fetch(&s_frames_sent, 1, __ATOMIC_RELAXED);
        ++window_frames;
        uint64_t now = esp_timer_get_time();
        if (now - window_start >= 1000000) {
            __atomic_store_n(&s_stream_fps,
                             (uint32_t)((window_frames * 1000000ULL) / (now - window_start)),
                             __ATOMIC_RELAXED);
            window_start = now;
            window_frames = 0;
        }
    }
    __atomic_store_n(&s_stream_fps, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_stream_active, false, __ATOMIC_SEQ_CST);
    ESP_LOGI(TAG, "Stream ended: %s", esp_err_to_name(result));
    return result;
}

static void start_servers(void)
{
    httpd_config_t web_config = HTTPD_DEFAULT_CONFIG();
    web_config.server_port = 80;
    web_config.max_uri_handlers = 3;
    web_config.lru_purge_enable = true;
    ESP_ERROR_CHECK(httpd_start(&s_web_server, &web_config));
    const httpd_uri_t index_uri = {.uri = "/", .method = HTTP_GET, .handler = index_handler};
    const httpd_uri_t status_uri = {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler};
    const httpd_uri_t config_uri = {.uri = "/api/config", .method = HTTP_POST, .handler = config_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_web_server, &index_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_web_server, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_web_server, &config_uri));

    httpd_config_t stream_config = HTTPD_DEFAULT_CONFIG();
    stream_config.server_port = 81;
    stream_config.ctrl_port = 32769;
    stream_config.max_uri_handlers = 1;
    stream_config.max_open_sockets = 2;
    stream_config.send_wait_timeout = 2;
    stream_config.recv_wait_timeout = 2;
    stream_config.stack_size = 8192;
    ESP_ERROR_CHECK(httpd_start(&s_stream_server, &stream_config));
    const httpd_uri_t stream_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream_handler};
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_stream_server, &stream_uri));
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_LOGI(TAG, "PSRAM: %u bytes", (unsigned)esp_psram_get_size());
    load_config();
    ESP_LOGI(TAG, "Configuration/AP password: %s", s_config.admin_key);
    init_camera();
    init_wifi();
    start_servers();
    xTaskCreate(boot_button_task, "setup_button", 8192, NULL, 2, NULL);
}
