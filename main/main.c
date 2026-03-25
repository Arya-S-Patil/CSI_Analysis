#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "lwip/sockets.h"
#include "esp_netif.h"
#include "esp_crt_bundle.h"

#define TAG                "CSI_SHEETS"
#define HOTSPOT_SSID       "AryaSPatil"
#define HOTSPOT_PASS       "aryaspatil"
#define APPS_SCRIPT_URL    "https://script.google.com/macros/s/AKfycbzULsiUTr7lry-QDhloZYMpe77cCV0wSpiIwxLflNALydamXwcIq7sUb1SwT-vVYWo6kg/exec"
#define ESP_UDP_PORT       5000
#define WIFI_TIMEOUT_MS    15000
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

#define NUM_SUBCARRIERS    64
#define NUM_PACKETS        50
#define TOTAL_SAMPLES      (NUM_SUBCARRIERS * NUM_PACKETS)  // 3200

// ── AP list ──────────────────────────────────────────
typedef struct { char ssid[32]; char pass[64]; } ap_t;
static ap_t pi_aps[] = {
    { "PI5_AP_1", "password123" },
    { "PI5_AP_2", "password456" },
};
#define NUM_APS 2
static int ap_index = 0;

// ── One CSI IQ sample per subcarrier per packet ───────
typedef struct {
    int8_t real;
    int8_t imag;
} iq_t;

// ── Per-packet metrics: RSSI + derived per-subcarrier values ─────────────────
// amplitude = sqrt(real^2 + imag^2)  — proportional to received signal strength
// angle_rad  = atan2(imag, real)      — phase of the subcarrier in radians [-π, π]
typedef struct {
    int8_t  rssi;                        // RSSI in dBm for this packet
    float   amplitude[NUM_SUBCARRIERS];  // magnitude per subcarrier
    float   angle_rad[NUM_SUBCARRIERS];  // phase angle per subcarrier
} metrics_t;

static iq_t      csi_buf[NUM_PACKETS][NUM_SUBCARRIERS];
static metrics_t pkt_metrics[NUM_PACKETS];   // indexed by packet number
static int       packets_collected = 0;

static EventGroupHandle_t wifi_ev;
static volatile bool switching_ap = false;

// ══ Wi-Fi ═════════════════════════════════════════════

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

// ══ CSI callback ══════════════════════════════════════

static void csi_cb(void *ctx, wifi_csi_info_t *info) {
    if (!info || !info->buf) return;
    if (packets_collected >= NUM_PACKETS) return;

    int8_t *buf = info->buf;
    int     len = info->len;
    int     pkt = packets_collected;

    // ── Store raw IQ ──────────────────────────────────
    int sc = 0;
    for (int i = 0; i + 1 < len && sc < NUM_SUBCARRIERS; i += 2, sc++) {
        csi_buf[pkt][sc].imag = buf[i];
        csi_buf[pkt][sc].real = buf[i + 1];
    }

    // ── Capture RSSI ─────────────────────────────────
    // rx_ctrl.rssi is the per-frame RSSI in dBm reported by the radio.
    pkt_metrics[pkt].rssi = (int8_t)info->rx_ctrl.rssi;

    // ── Derive amplitude and phase per subcarrier ────
    for (int s = 0; s < NUM_SUBCARRIERS; s++) {
        float r = (float)csi_buf[pkt][s].real;
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

// ── UDP traffic generator ──────────────────────────────────────────────────
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

// ══ Build JSON and push to Sheets ════════════════════
//
// Each row now encodes:
//   subcarrier, packet, real, imag,
//   rssi        (int, dBm),
//   amplitude   (float, sqrt(r²+i²)),
//   angle_rad   (float, atan2(i,r))
//
// Worst-case per-row JSON:
//   {"subcarrier":63,"packet":49,"real":-128,"imag":-128,
//    "rssi":-99,"amplitude":181.02,"angle_rad":-3.14159}
//   ≈ 100 chars per row.  CHUNK_SIZE=50 → 50×100 + 30 header = 5030 → 6144 buf.

#define CHUNK_SIZE     50
#define BODY_BUF_SIZE  6144   // increased from 4096 to fit new fields

static void push_chunk(int ap_idx, int start_row, int end_row) {
    char *body = malloc(BODY_BUF_SIZE);
    if (!body) { ESP_LOGE(TAG, "OOM: chunk malloc failed"); return; }

    int pos = 0;
    pos += snprintf(body + pos, BODY_BUF_SIZE - pos,
        "{\"ap_index\":%d,\"samples\":[", ap_idx);

    for (int row = start_row; row < end_row; row++) {
        int sc  = row / NUM_PACKETS;
        int pkt = row % NUM_PACKETS;

        if ((BODY_BUF_SIZE - pos) < 128) {
            ESP_LOGE(TAG, "Buffer full at row %d — truncating", row);
            break;
        }

        pos += snprintf(body + pos, BODY_BUF_SIZE - pos,
            "%s{"
            "\"subcarrier\":%d,"
            "\"packet\":%d,"
            "\"real\":%d,"
            "\"imag\":%d,"
            "\"rssi\":%d,"
            "\"amplitude\":%.4f,"
            "\"angle_rad\":%.5f"
            "}",
            row == start_row ? "" : ",",
            sc, pkt,
            csi_buf[pkt][sc].real,
            csi_buf[pkt][sc].imag,
            (int)pkt_metrics[pkt].rssi,
            pkt_metrics[pkt].amplitude[sc],
            pkt_metrics[pkt].angle_rad[sc]
        );
    }
    pos += snprintf(body + pos, BODY_BUF_SIZE - pos, "]}");

    esp_http_client_config_t cfg = {
        .url    = APPS_SCRIPT_URL,
        .method = HTTP_METHOD_POST,
        .crt_bundle_attach  = esp_crt_bundle_attach,
        .skip_cert_common_name_check = true,
        .timeout_ms         = 30000,
        .buffer_size        = 2048,
        .buffer_size_tx     = 2048,
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, pos);

    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK && err != ESP_ERR_HTTP_INCOMPLETE_DATA) {
        ESP_LOGE(TAG, "HTTP POST failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGD(TAG, "HTTP POST OK (status=%d)",
                 esp_http_client_get_status_code(client));
    }

    esp_http_client_cleanup(client);
    free(body);
}

static void push_all_chunks(int ap_idx) {
    int total  = NUM_SUBCARRIERS * NUM_PACKETS;
    int chunks = (total + CHUNK_SIZE - 1) / CHUNK_SIZE;

    // ── Log summary stats before upload ───────────────
    // Compute and log average RSSI and amplitude across all packets
    // so you can see a quick sanity-check in the monitor before data leaves.
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

    ESP_LOGI(TAG, "Pushing %d rows in %d chunks...", total, chunks);

    for (int c = 0; c < chunks; c++) {
        int start = c * CHUNK_SIZE;
        int end   = start + CHUNK_SIZE;
        if (end > total) end = total;

        push_chunk(ap_idx, start, end);
        ESP_LOGI(TAG, "Chunk %d/%d done", c + 1, chunks);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

// ══ Main loop ═════════════════════════════════════════

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
        memset(pkt_metrics, 0, sizeof(pkt_metrics));   // clear metrics too
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
            push_all_chunks(ap_index);
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