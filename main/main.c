#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"
#include "secrets.h"

#define TAG                "CSI_FIREBASE"

#define ESP_UDP_PORT       5000
#define WIFI_TIMEOUT_MS    15000
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define NUM_SUBCARRIERS    64
#define NUM_PACKETS        50
#define TOTAL_SAMPLES      (NUM_SUBCARRIERS * NUM_PACKETS)  // 3200

#define CHUNK_SIZE         50
// FIX 1: Increased from 24576 to 40960 (40 KB) to prevent buffer overflow
// on chunks where float values are longer (e.g. angle_rad = -3.14159)
#define BODY_BUF_SIZE      40960

// ── AP list ──────────────────────────────────────────────────────────────────
typedef struct { char ssid[32]; char pass[64]; } ap_t;
static ap_t pi_aps[] = {
    { "PI5_AP_1", "password123" },
    { "PI5_AP_2", "password456" },
};
#define NUM_APS 2
static int ap_index = 0;

// ── One CSI IQ sample per subcarrier per packet ───────────────────────────────
typedef struct {
    int8_t real;
    int8_t imag;
} iq_t;

// ── Per-packet metrics ────────────────────────────────────────────────────────
typedef struct {
    int8_t  rssi;
    float   amplitude[NUM_SUBCARRIERS];
    float   angle_rad[NUM_SUBCARRIERS];
} metrics_t;

static iq_t      csi_buf[NUM_PACKETS][NUM_SUBCARRIERS];
static metrics_t pkt_metrics[NUM_PACKETS];
static int       packets_collected = 0;

static EventGroupHandle_t wifi_ev;
static volatile bool switching_ap = false;

// ══ Wi-Fi ════════════════════════════════════════════════════════════════════

static void on_wifi(void *arg, esp_event_base_t base,
                    int32_t id, void *data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (switching_ap) {
            xEventGroupSetBits(wifi_ev, WIFI_FAIL_BIT);
        } else {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(wifi_ev, WIFI_CONNECTED_BIT);
    }
}

static bool wifi_connect(const char *ssid, const char *pass) {
    switching_ap = true;
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(1000));
    xEventGroupClearBits(wifi_ev, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    switching_ap = false;

    wifi_config_t cfg = {};
    strncpy((char *)cfg.sta.ssid,     ssid, 32);
    strncpy((char *)cfg.sta.password, pass, 64);
    cfg.sta.threshold.authmode = strlen(pass) ?
                                 WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    esp_wifi_connect();

    EventBits_t b = xEventGroupWaitBits(
        wifi_ev,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_TIMEOUT_MS)
    );
    return (b & WIFI_CONNECTED_BIT) != 0;
}

// ══ CSI callback ═════════════════════════════════════════════════════════════

static void csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) return;
    if (packets_collected >= NUM_PACKETS) return;

    int8_t *buf = info->buf;
    int     len = info->len;
    int     pkt = packets_collected;

    int sc = 0;
    for (int i = 0; i + 1 < len && sc < NUM_SUBCARRIERS; i += 2, sc++) {
        csi_buf[pkt][sc].imag = buf[i];
        csi_buf[pkt][sc].real = buf[i + 1];
    }

    pkt_metrics[pkt].rssi = (int8_t)info->rx_ctrl.rssi;

    for (int s = 0; s < NUM_SUBCARRIERS; s++) {
        float r  = (float)csi_buf[pkt][s].real;
        float im = (float)csi_buf[pkt][s].imag;
        pkt_metrics[pkt].amplitude[s] = sqrtf(r * r + im * im);
        pkt_metrics[pkt].angle_rad[s] = atan2f(im, r);
    }

    packets_collected++;

    if (packets_collected % 50 == 0)
        ESP_LOGI(TAG, "Packets: %d / %d  (last RSSI: %d dBm)",
                 packets_collected, NUM_PACKETS,
                 pkt_metrics[pkt].rssi);
}

static void csi_enable(bool on) {
    if (on) {
        wifi_csi_config_t cfg = {
            .lltf_en = true, .htltf_en = true,
            .stbc_htltf2_en = true, .ltf_merge_en = true,
            .channel_filter_en = false, .manu_scale = false,
        };
        esp_wifi_set_csi_config(&cfg);
        esp_wifi_set_csi_rx_cb(csi_cb, NULL);
        esp_wifi_set_csi(true);
    } else {
        esp_wifi_set_csi(false);
        esp_wifi_set_csi_rx_cb(NULL, NULL);
    }
}

// ══ UDP traffic generator ═════════════════════════════════════════════════════

#define UDP_PING_INTERVAL_MS  10
#define PI_UDP_PORT           5000

static volatile bool udp_running = false;
static int           udp_sock    = -1;

static bool get_gateway_ip(char *out, size_t len) {
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) return false;
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return false;
    snprintf(out, len, IPSTR, IP2STR(&ip_info.gw));
    return true;
}

static void udp_task(void *arg) {
    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock < 0) {
        ESP_LOGE(TAG, "UDP socket creation failed");
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = { .tv_sec = 0, .tv_usec = 5000 };
    setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in bind_addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons(ESP_UDP_PORT),
    };
    bind(udp_sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));

    char gw_ip[16] = {0};
    if (!get_gateway_ip(gw_ip, sizeof(gw_ip))) {
        ESP_LOGE(TAG, "Could not resolve gateway IP");
        close(udp_sock);
        udp_sock = -1;
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Sending UDP pings to Pi at %s:%d", gw_ip, PI_UDP_PORT);

    struct sockaddr_in pi_addr = {
        .sin_family = AF_INET,
        .sin_port   = htons(PI_UDP_PORT),
    };
    inet_aton(gw_ip, &pi_addr.sin_addr);

    const char ping_msg[] = "ping";
    char rx_buf[16];
    struct sockaddr_in src;
    socklen_t sl = sizeof(src);

    udp_running = true;
    while (udp_running) {
        sendto(udp_sock, ping_msg, sizeof(ping_msg), 0,
               (struct sockaddr *)&pi_addr, sizeof(pi_addr));
        recvfrom(udp_sock, rx_buf, sizeof(rx_buf), 0,
                 (struct sockaddr *)&src, &sl);
        vTaskDelay(pdMS_TO_TICKS(UDP_PING_INTERVAL_MS));
    }

    close(udp_sock);
    udp_sock = -1;
    vTaskDelete(NULL);
}

static void udp_stop(TaskHandle_t task) {
    udp_running = false;
    vTaskDelay(pdMS_TO_TICKS(300));
    (void)task;
}

// ══ Firebase Firestore upload ═════════════════════════════════════════════════

// Returns HTTP status code, or -1 on transport error.
static int firebase_insert_chunk(int ap_idx, int chunk_idx,
                                  int start_row, int end_row)
{
    char *body = malloc(BODY_BUF_SIZE);
    if (!body) { ESP_LOGE(TAG, "OOM: chunk malloc failed"); return -1; }

    int pos = 0;

    // Open Firestore document with top-level scalar fields
    pos += snprintf(body + pos, BODY_BUF_SIZE - pos,
        "{"
          "\"fields\":{"
            "\"ap_index\":{\"integerValue\":\"%d\"},"
            "\"chunk_index\":{\"integerValue\":\"%d\"},"
            "\"samples\":{\"arrayValue\":{\"values\":[",
        ap_idx, chunk_idx);

    // One Firestore mapValue per row
    for (int row = start_row; row < end_row; row++) {
        int sc  = row / NUM_PACKETS;
        int pkt = row % NUM_PACKETS;

        // FIX 2: Guard threshold raised from 300 to 400 bytes.
        // A single row entry can be up to ~280 bytes in the worst case
        // (e.g. angle_rad = -3.14159, amplitude = 127.0000). The original
        // 300-byte guard was too close and could still allow snprintf to
        // write a partial/truncated entry before hitting the buffer end,
        // producing malformed JSON that Firestore rejects with HTTP 400.
        if ((BODY_BUF_SIZE - pos) < 400) {
            ESP_LOGE(TAG, "Buffer nearly full at row %d — aborting chunk", row);
            break;
        }

        pos += snprintf(body + pos, BODY_BUF_SIZE - pos,
            "%s"
            "{\"mapValue\":{\"fields\":{"
              "\"subcarrier\":{\"integerValue\":\"%d\"},"
              "\"packet\":{\"integerValue\":\"%d\"},"
              "\"real\":{\"integerValue\":\"%d\"},"
              "\"imag\":{\"integerValue\":\"%d\"},"
              "\"rssi\":{\"integerValue\":\"%d\"},"
              "\"amplitude\":{\"doubleValue\":%.4f},"
              "\"angle_rad\":{\"doubleValue\":%.5f}"
            "}}}",
            row == start_row ? "" : ",",
            sc, pkt,
            (int)csi_buf[pkt][sc].real,
            (int)csi_buf[pkt][sc].imag,
            (int)pkt_metrics[pkt].rssi,
            pkt_metrics[pkt].amplitude[sc],
            pkt_metrics[pkt].angle_rad[sc]
        );
    }

    // FIX 3: Closing delimiter corrected from "]}}}}}" (6 closers) to
    // "]}}}}}" (5 closers). Brace count:
    //   ]      — closes "values" array
    //   }      — closes "arrayValue" object
    //   }      — closes "samples" field value
    //   }      — closes outer "fields" map
    //   }      — closes the top-level document object
    // The original had one extra "}" which made every chunk's JSON invalid.
    pos += snprintf(body + pos, BODY_BUF_SIZE - pos, "]}}}}");

    // Build Firestore REST URL
    char url[256];
    snprintf(url, sizeof(url),
        "https://firestore.googleapis.com/v1/projects/%s"
        "/databases/(default)/documents/csi?key=%s",
        FIREBASE_PROJECT, FIREBASE_API_KEY);
    
    esp_http_client_config_t cfg = {
        .url                         = url,
        .method                      = HTTP_METHOD_POST,
        .crt_bundle_attach           = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .timeout_ms                  = 30000,
        .buffer_size                 = 2048,
        .buffer_size_tx              = 4096,
        .max_redirection_count       = 5,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, pos);

    esp_err_t err    = esp_http_client_perform(client);
    int       status = esp_http_client_get_status_code(client);

    if (err != ESP_OK && err != ESP_ERR_HTTP_INCOMPLETE_DATA) {
        ESP_LOGE(TAG, "HTTP transport error: %s", esp_err_to_name(err));
        status = -1;
    } else if (status != 200) {
        // Log Firestore's response body to see the exact error reason
        char resp_buf[512] = {0};
        esp_http_client_read_response(client, resp_buf, sizeof(resp_buf) - 1);
        ESP_LOGW(TAG, "Firestore returned HTTP %d (expected 200)", status);
        ESP_LOGE(TAG, "Firestore error body: %s", resp_buf);
    }

    esp_http_client_cleanup(client);
    free(body);
    return status;
}

// Retry wrapper — 3 attempts with exponential back-off (1s, 2s, 3s).
static void firebase_insert_chunk_with_retry(int ap_idx, int chunk_idx,
                                              int start, int end)
{
    for (int attempt = 1; attempt <= 3; attempt++) {
        int status = firebase_insert_chunk(ap_idx, chunk_idx, start, end);
        if (status == 200) return;

        ESP_LOGW(TAG, "Chunk %d failed (HTTP %d), retry %d/3",
                 chunk_idx, status, attempt);
        vTaskDelay(pdMS_TO_TICKS(1000 * attempt));
    }
    ESP_LOGE(TAG, "Chunk %d permanently failed after 3 attempts", chunk_idx);
}

static void push_to_firebase(int ap_idx)
{
    int total  = NUM_SUBCARRIERS * NUM_PACKETS;
    int chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;

    float avg_rssi = 0.0f, avg_amp = 0.0f;
    for (int p = 0; p < NUM_PACKETS; p++) {
        avg_rssi += pkt_metrics[p].rssi;
        for (int s = 0; s < NUM_SUBCARRIERS; s++)
            avg_amp += pkt_metrics[p].amplitude[s];
    }
    avg_rssi /= NUM_PACKETS;
    avg_amp  /= (float)(NUM_PACKETS * NUM_SUBCARRIERS);
    ESP_LOGI(TAG, "AP %d — avg RSSI: %.1f dBm  avg amplitude: %.2f",
             ap_idx, avg_rssi, avg_amp);

    ESP_LOGI(TAG, "Pushing %d rows in %d chunks to Firestore...", total, chunks);

    for (int c = 0; c < chunks; c++) {
        int start = c * CHUNK_SIZE;
        int end   = start + CHUNK_SIZE;
        if (end > total) end = total;

        firebase_insert_chunk_with_retry(ap_idx, c, start, end);
        ESP_LOGI(TAG, "Chunk %d / %d done", c + 1, chunks);
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    ESP_LOGI(TAG, "=== All data pushed to Firestore for AP %d ===", ap_idx);
}

// ══ Main loop ════════════════════════════════════════════════════════════════

void app_main() {
    nvs_flash_init();

    wifi_ev = xEventGroupCreate();
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&init);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();

    while (1) {
        ap_t *ap = &pi_aps[ap_index];

        ESP_LOGI(TAG, "=== Connecting to %s ===", ap->ssid);
        memset(csi_buf,     0, sizeof(csi_buf));
        memset(pkt_metrics, 0, sizeof(pkt_metrics));
        packets_collected = 0;

        if (wifi_connect(ap->ssid, ap->pass)) {

            csi_enable(true);
            TaskHandle_t udp_handle = NULL;
            xTaskCreate(udp_task, "udp", 4096, NULL, 5, &udp_handle);

            ESP_LOGI(TAG, "Collecting %d packets...", NUM_PACKETS);
            while (packets_collected < NUM_PACKETS) {
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            ESP_LOGI(TAG, "%d packets collected from %s",
                     NUM_PACKETS, ap->ssid);

            csi_enable(false);
            udp_stop(udp_handle);

        } else {
            ESP_LOGW(TAG, "Could not connect to %s", ap->ssid);
        }

        ESP_LOGI(TAG, "=== Switching to hotspot ===");
        switching_ap = true;

        if (wifi_connect(HOTSPOT_SSID, HOTSPOT_PASS)) {
            push_to_firebase(ap_index);
        } else {
            ESP_LOGE(TAG, "Could not connect to hotspot — data lost for AP %d",
                     ap_index);
        }

        ap_index = (ap_index + 1) % NUM_APS;

        if (ap_index == 0) {
            ESP_LOGI(TAG, "=== All APs done — data collection complete ===");
            ESP_LOGI(TAG, "=== Halting. Reset ESP32 to collect again. ===");
            esp_wifi_disconnect();
            esp_wifi_stop();
            while (1) vTaskDelay(pdMS_TO_TICKS(10000));
        }

        ESP_LOGI(TAG, "=== Next: %s ===", pi_aps[ap_index].ssid);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}