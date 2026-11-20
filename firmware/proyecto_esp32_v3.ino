/*
 * =========================================================================================
 * PROYECTO V3: Sistema Completo ESP32 â€” WiFi Sniffer + BLE Scanner + AWS IoT Core
 * INSTITUCIÃ“N: Universidad Nacional de San Antonio Abad del Cusco (UNSAAC)
 * DEPARTAMENTO: InformÃ¡tica â€” Sistemas Embebidos e IoT
 *
 * NOVEDADES V3 (sobre V2):
 *  - Escaneo BLE: detecta audÃ­fonos (AirPods, Sony WH/WF, Galaxy Buds, Bose, Jabra, etc.)
 *  - ConexiÃ³n MQTT a AWS IoT Core con TLS/X.509 (certificados del directorio del proyecto)
 *  - Ciclo TDM: 25 s WiFi Sniffer â†’ 8 s BLE scan â†’ 7 s Upload MQTT
 *  - Contador de personas automÃ¡tico (Salice et al. 2024: ratio 1.5 disp/persona)
 *  - Ajuste manual de personas desde dashboard web (vÃ­a MQTT Subscribe)
 *  - PublicaciÃ³n JSON completa: WiFi devices + BLE devices + conteo personas + POI
 *
 * PAPERS INTEGRADOS:
 *  Salice et al. (2024, ACM GoodIT): ratio 1.5 disp/persona, ventana 30 s
 *  Chaaben et al. (2025, IEEE ICSC): desaleatorizaciÃ³n por IEs 45/127/221
 *  Al-Maktary et al. (2025) & Beigi & Shah-Mansouri (2023): Kalman + Path Loss
 *
 * LIBRERÃAS REQUERIDAS (Arduino Library Manager):
 *  - PubSubClient by Nick O'Leary (v2.8+)
 *  - ArduinoJson by Benoit Blanchon (v6.x)
 * =========================================================================================
 */

// --- Ajuste ANTES de los includes ---
#define MQTT_MAX_PACKET_SIZE 8192

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

// =========================================================================================
// 1. CONFIGURACIÃ“N â€” RELLENAR CON TUS DATOS
// =========================================================================================

// --- Credenciales WiFi ---
#define WIFI_SSID        "TU_WIFI_SSID"
#define WIFI_PASSWORD    "TU_WIFI_PASSWORD"

// --- AWS IoT Core ---
#define AWS_IOT_ENDPOINT   "TU_ENDPOINT-ats.iot.REGION.amazonaws.com"
#define AWS_IOT_PORT       8883
#define DEVICE_ID          "ESP32-UNSAAC-Sniffer"

// TÃ³picos MQTT
#define TOPIC_PUBLISH      "unsaac/esp32/scan"
#define TOPIC_ADJUST       "unsaac/esp32/adjust"   // SuscripciÃ³n: ajuste manual

// --- MAC propia del ESP32 (ignorar en captura) ---
const uint8_t OWN_ESP32_MAC[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // Reemplazar por la MAC local

// =========================================================================================
// 2. CERTIFICADOS AWS IoT â€” PEGAR CONTENIDO DE LOS ARCHIVOS .pem
// =========================================================================================

static const char AWS_CERT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
PEGA_AQUI_AMAZON_ROOT_CA
-----END CERTIFICATE-----
)EOF";

static const char AWS_CERT_CRT[] = R"EOF(
-----BEGIN CERTIFICATE-----
PEGA_AQUI_CERTIFICADO_DEL_DISPOSITIVO
-----END CERTIFICATE-----
)EOF";

static const char AWS_CERT_KEY[] = R"EOF(
-----BEGIN RSA PRIVATE KEY-----
PEGA_AQUI_CLAVE_PRIVADA_DEL_DISPOSITIVO
-----END RSA PRIVATE KEY-----
)EOF";

// =========================================================================================
// 3. PARÃMETROS DEL SISTEMA (calibrados segÃºn papers)
// =========================================================================================

// Path Loss â€” Al-Maktary et al. (2025) & Beigi & Shah-Mansouri (2023)
#define PATH_LOSS_A_1M        (-45.0f)   // RSSI a 1 metro (dBm) â€” calibrar en campo
#define PATH_LOSS_N           (2.7f)     // Exponente interiores (2.5â€“4.0 tÃ­pico)
#define RSSI_MIN_THRESHOLD    (-90)      // LÃ­mite de sensibilidad
#define MAX_RELIABLE_DIST     (15.0f)    // Cota mÃ¡xima fÃ­sica

// Kalman â€” Beigi & Shah-Mansouri (2023): Q=0.01, R=1.0
#define KALMAN_Q              (0.01f)
#define KALMAN_R              (1.20f)

// Ventana y timeouts â€” Salice et al. (2024)
#define DEVICE_TIMEOUT_MS     (60000)    // 60 s sin actividad â†’ retirar
#define MAX_WIFI_DEVICES      (40)       // Capacidad tabla WiFi
#define MAX_BLE_DEVICES       (20)       // Capacidad tabla BLE

// Channel Hopping WiFi
#define HOP_INTERVAL_MS       (250)
#define MIN_CHANNEL           (1)
#define MAX_CHANNEL           (13)

// Ciclo TDM (Time-Division Multiplexing WiFi/BLE/Upload)
#define PHASE_WIFI_DURATION   (25000)    // 25 s sniff WiFi
#define PHASE_BLE_DURATION    (8000)     // 8 s scan BLE
#define PHASE_UPLOAD_TIMEOUT  (7000)     // 7 s mÃ¡ximo para subir

// BotÃ³n POI
#define POI_BUTTON_PIN        (0)
#define DEBOUNCE_DELAY_MS     (250)

// Tabla y visualizaciÃ³n
#define TABLE_REFRESH_MS      (10000)    // Tabla en monitor serie cada 10 s

// =========================================================================================
// 4. ESTRUCTURAS DE DATOS
// =========================================================================================

struct KalmanFilter {
    float x, P, Q, R;
    bool initialized;

    void init(float v, float q = KALMAN_Q, float r = KALMAN_R) {
        x = v; P = 1.0f; Q = q; R = r; initialized = true;
    }
    float update(float z) {
        if (!initialized) { init(z); return x; }
        float Pm = P + Q;
        float K  = Pm / (Pm + R);
        x = x + K * (z - x);
        P = (1.0f - K) * Pm;
        return x;
    }
};

struct DeviceFingerprint {
    uint16_t frame_len;
    uint32_t hash_ie45;
    uint32_t hash_ie127;
    uint32_t hash_ie221;
    uint32_t vendor_oui;
    uint8_t  ie_flags;
};

struct WiFiTrackedDevice {
    uint8_t mac[6];
    bool    is_random;
    char    manufacturer[22];
    DeviceFingerprint fp;
    KalmanFilter kalman;
    int8_t  last_raw_rssi;
    float   filtered_rssi;
    float   estimated_distance;
    uint8_t zone_id;           // 0=Inmediato, 1=Cerca, 2=Medio, 3=Lejos
    uint32_t packet_count;
    uint32_t first_seen_ms;
    uint32_t last_seen_ms;
    uint8_t last_channel;
    bool    active;
};

struct BLETrackedDevice {
    char    mac_str[18];       // "AA:BB:CC:DD:EE:FF"
    char    name[32];          // Nombre del dispositivo BLE
    char    device_type[24];   // "AirPods Pro", "Sony WH-1000", etc.
    int     rssi;
    bool    is_random;
    bool    is_headphone;
    uint32_t last_seen_ms;
    bool    active;
};

// ClasificaciÃ³n de dispositivos y audÃ­fonos BLE
struct BLEClassification {
    char name[32];
    char type[24];
    bool is_headphone;
};

// MÃ©tricas de conteo de personas
struct PeopleCount {
    uint16_t wifi_active;
    uint16_t ble_active;
    uint16_t total_devices;
    uint16_t auto_estimate;    // = total / 1.5
    int      manual_offset;    // ajuste manual desde dashboard
    uint16_t total_persons;    // auto_estimate + manual_offset (mÃ­nimo 0)
};

// Prototipos explÃ­citos para el compilador de Arduino
BLEClassification classifyBLEDevice(BLEAdvertisedDevice &dev);
PeopleCount countPeople();

#define MAX_PAYLOAD_COPY 160
struct SnifferPacketMessage {
    int8_t  rssi;
    uint8_t channel;
    uint16_t length;
    uint8_t payload[MAX_PAYLOAD_COPY];
};

// Fases del ciclo TDM
enum SystemPhase {
    PHASE_WIFI_SNIFF,
    PHASE_BLE_SCAN,
    PHASE_UPLOAD
};

// =========================================================================================
// 5. VARIABLES GLOBALES
// =========================================================================================

static QueueHandle_t snifferQueue = NULL;
static WiFiTrackedDevice wifiTable[MAX_WIFI_DEVICES];
static BLETrackedDevice  bleTable[MAX_BLE_DEVICES];

static SystemPhase currentPhase = PHASE_WIFI_SNIFF;
static uint32_t phaseStartTime  = 0;
static uint8_t  currentChannel  = MIN_CHANNEL;
static uint32_t lastChannelHopTime = 0;
static uint32_t lastTablePrintTime = 0;

RTC_DATA_ATTR static uint8_t currentPoiId = 0;   // Persistente en RTC entre reinicios
RTC_DATA_ATTR static int     manualOffset = 0;   // Ajuste manual persistente en RTC entre reinicios
static uint32_t lastButtonPressTime = 0;

// MQTT / WiFiSecure
static WiFiClientSecure secureClient;
static PubSubClient     mqttClient(secureClient);

// BLE
static BLEScan* pBLEScan = nullptr;

// =========================================================================================
// 6. FUNCIONES AUXILIARES
// =========================================================================================

uint32_t hashFNV1a(const uint8_t *data, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= data[i]; h *= 16777619u; }
    return h;
}

bool isRandomMac(const uint8_t *mac)              { return (mac[0] & 0x02) != 0; }
bool areMacsEqual(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; i++) if (a[i] != b[i]) return false;
    return true;
}

float calculateDistance(float rssi) {
    if (rssi >= 0.0f) return 0.1f;
    float d = powf(10.0f, (PATH_LOSS_A_1M - rssi) / (10.0f * PATH_LOSS_N));
    return (d > MAX_RELIABLE_DIST) ? MAX_RELIABLE_DIST : d;
}

uint8_t getZoneId(float d) {
    if (d <= 1.5f) return 0;
    if (d <= 4.0f) return 1;
    if (d <= 8.0f) return 2;
    return 3;
}

const char* getZoneName(uint8_t z) {
    switch (z) {
        case 0: return "INMEDIATO";
        case 1: return "CERCA";
        case 2: return "MEDIO";
        default: return "LEJOS";
    }
}

bool matchFingerprint(const DeviceFingerprint &f1, const DeviceFingerprint &f2) {
    if (abs((int)f1.frame_len - (int)f2.frame_len) > 4) return false;
    int score = 0;
    if (f1.hash_ie127 && f1.hash_ie127 == f2.hash_ie127) score += 4;
    if (f1.hash_ie221 && f1.hash_ie221 == f2.hash_ie221) score += 3;
    if (f1.hash_ie45  && f1.hash_ie45  == f2.hash_ie45)  score += 2;
    if (f1.ie_flags == f2.ie_flags)                       score += 1;
    return score >= 4;
}

// =========================================================================================
// 7. CLASIFICADOR OUI WiFi (expandido)
// =========================================================================================

void getManufacturerName(const uint8_t *mac, const DeviceFingerprint &fp, char *out, size_t maxlen) {
    if (isRandomMac(mac)) {
        // IdentificaciÃ³n por Vendor OUI en IE 221 (Chaaben et al., 2025)
        if      (fp.vendor_oui == 0x0017F2 || fp.vendor_oui == 0x8C8590) snprintf(out, maxlen, "Apple (Privado)");
        else if (fp.vendor_oui == 0x00A0C6)                               snprintf(out, maxlen, "Android Qualcomm");
        else if (fp.vendor_oui == 0x001302)                               snprintf(out, maxlen, "Laptop Intel (Priv)");
        else if (fp.vendor_oui == 0x00904C || fp.vendor_oui == 0x001018) snprintf(out, maxlen, "Movil Broadcom");
        else if (fp.vendor_oui == 0x0050F2)                               snprintf(out, maxlen, "Movil/PC (WPS)");
        else if (fp.frame_len >= 160)                                     snprintf(out, maxlen, "Laptop/PC (Priv)");
        else if (fp.frame_len >= 100)                                     snprintf(out, maxlen, "Smartphone (Priv)");
        else                                                              snprintf(out, maxlen, "IoT (Privado)");
        return;
    }
    uint32_t p = ((uint32_t)mac[0] << 16) | ((uint32_t)mac[1] << 8) | mac[2];
    switch (p) {
        // Samsung
        case 0x508569: case 0x244B03: case 0x34C9F0: case 0x5C0A5B: case 0xE8508B:
        case 0x0012FB: case 0x205531: case 0x380146: case 0x40163B: case 0x60A10A:
        case 0x842519: case 0x88329B: case 0x90F1AA: case 0xA0821F: case 0xD059E4:
            snprintf(out, maxlen, "Samsung Cel/Tab"); break;
        // Xiaomi / Redmi / POCO
        case 0xA82BD5: case 0x14F65A: case 0x640980: case 0x7802F8: case 0x04CF4B:
        case 0x286C07: case 0x34CE00: case 0x40313C: case 0x68DFDD: case 0x742344:
        case 0x94870 + 3: case 0xB0E235: case 0xDC729B: case 0xFC64BA:
            snprintf(out, maxlen, "Xiaomi Celular"); break;
        // Apple
        case 0xB014D1: case 0x904C81: case 0x44A3C7: case 0x0017F2: case 0x3C0754:
        case 0x7CD1C3: case 0xACBC32: case 0xF01898: case 0x000393: case 0x040C56:
        case 0x1093E9: case 0x149877: case 0x20A2E4: case 0x280B5C: case 0x38F9D3:
        case 0x406C8F: case 0x4860BC: case 0x64A3CB: case 0x701124: case 0x787E61:
        case 0x80B03D: case 0x88C663: case 0xBC926B: case 0xDC2B61: case 0xF45C89:
            snprintf(out, maxlen, "Apple iPhone/Mac"); break;
        // TP-Link
        case 0x503EAA: case 0x4C445B: case 0x14CC20: case 0x18A6F7: case 0x50C7BF:
        case 0x647002: case 0x704F57: case 0x909A4A: case 0xDC396F: case 0xE4C32A:
            snprintf(out, maxlen, "TP-Link Router"); break;
        // Intel
        case 0x7C70DB: case 0x001302: case 0x4851B7: case 0x58946B: case 0x8086F2:
        case 0x3C6AA7: case 0x48F17F: case 0x5076AF: case 0x843497: case 0x984FEE:
            snprintf(out, maxlen, "Intel Laptop/PC"); break;
        // HP
        case 0x54F0B1: case 0x001E0B: case 0x3C4A92: case 0x705A0F: case 0x9C8E99:
        case 0x10604B: case 0x28924A: case 0x645106: case 0x78E3B5:
            snprintf(out, maxlen, "HP Laptop/PC"); break;
        // Dell
        case 0x001422: case 0x14FEB5: case 0x180373: case 0x24B6FD: case 0x44A842:
        case 0x5CF9DD: case 0x74867A: case 0x847BEB: case 0xB8AC6F: case 0xC81F66:
            snprintf(out, maxlen, "Dell Laptop/PC"); break;
        // Lenovo / Motorola
        case 0x000B2B: case 0x001A6B: case 0x044B80: case 0x107B44: case 0x207693:
        case 0x40C729: case 0x600292: case 0x70723C: case 0x985FD3: case 0xEC8EB5:
            snprintf(out, maxlen, "Lenovo/Moto"); break;
        // MediaTek / Ralink
        case 0x38D57A: case 0x000C43: case 0x001B9E: case 0x00E04C:
            snprintf(out, maxlen, "MediaTek Laptop/PC"); break;
        // ASUS
        case 0xBC6EE2: case 0x0015F2: case 0x001A92: case 0x08606E: case 0x14DDA9:
        case 0x244BFE: case 0x3085A9: case 0x708BCD: case 0x74D02B:
            snprintf(out, maxlen, "ASUS Laptop/PC"); break;
        // Huawei / Honor
        case 0x00E0FC: case 0x4846FB: case 0x707990: case 0x001E10: case 0x104780:
        case 0x14A51A: case 0x24DBAC: case 0x60E327: case 0x68A03E:
            snprintf(out, maxlen, "Huawei Celular"); break;
        // Sony
        case 0x00014A: case 0x280DFA: case 0x307512: case 0x40B837: case 0x709E29:
        case 0x981C0A: case 0xAC9B0A: case 0xFC0F4B:
            snprintf(out, maxlen, "Sony TV/Audif"); break;
        // Espressif ESP32/ESP8266
        case 0x240AC4: case 0x30AEA4: case 0x28562F: case 0x246F28: case 0x24B2DE:
        case 0x3C6105: case 0x68C63A: case 0x840D8E: case 0x84CCA8: case 0xA020A6:
            snprintf(out, maxlen, "Espressif IoT"); break;
        // LG Smart TV
        case 0x001C62: case 0x10F9EE: case 0x203D66: case 0x388C50: case 0x7440BE:
        case 0x88366C: case 0xCC2DE0:
            snprintf(out, maxlen, "LG Smart TV"); break;
        // Amazon Echo / Fire
        case 0x00FC8B: case 0x18742C: case 0x40B4CD: case 0x68545A: case 0x747548:
        case 0x84D6D0: case 0xAC63BE:
            snprintf(out, maxlen, "Amazon Echo/IoT"); break;
        default:
            if      (fp.frame_len >= 160) snprintf(out, maxlen, "Laptop/PC (OUI)");
            else if (fp.frame_len <= 80)  snprintf(out, maxlen, "Disp IoT (OUI)");
            else                          snprintf(out, maxlen, "OUI %02X:%02X:%02X", mac[0], mac[1], mac[2]);
            break;
    }
}

// =========================================================================================
// 8. PARSER DE IEs (Probe Request)
// =========================================================================================

void parseProbeRequest(const uint8_t *payload, uint16_t len, DeviceFingerprint &fp) {
    fp.frame_len = len;
    fp.hash_ie45 = fp.hash_ie127 = fp.hash_ie221 = fp.ie_flags = fp.vendor_oui = 0;
    if (len <= 24) return;
    size_t offset = 24;
    while (offset + 2 <= len) {
        uint8_t ie_id  = payload[offset];
        uint8_t ie_len = payload[offset + 1];
        if (offset + 2 + ie_len > len) break;
        const uint8_t *ie_data = &payload[offset + 2];
        switch (ie_id) {
            case 0:   fp.ie_flags |= (1 << 0); break;
            case 1:   fp.ie_flags |= (1 << 1); break;
            case 45:  fp.ie_flags |= (1 << 2); fp.hash_ie45  = hashFNV1a(ie_data, ie_len); break;
            case 127: fp.ie_flags |= (1 << 3); fp.hash_ie127 = hashFNV1a(ie_data, ie_len); break;
            case 221:
                fp.ie_flags |= (1 << 4);
                fp.hash_ie221 ^= hashFNV1a(ie_data, ie_len);
                if (ie_len >= 3) {
                    uint32_t oui = ((uint32_t)ie_data[0] << 16) | ((uint32_t)ie_data[1] << 8) | ie_data[2];
                    if (fp.vendor_oui == 0 || oui == 0x0017F2 || oui == 0x8C8590)
                        fp.vendor_oui = oui;
                }
                break;
        }
        offset += 2 + ie_len;
    }
}

// =========================================================================================
// 9. GESTIÃ“N DE TABLA WiFi
// =========================================================================================

void registerOrUpdateWiFiDevice(const uint8_t *mac, int8_t rssi, uint8_t channel, const DeviceFingerprint &fp) {
    uint32_t now = millis();
    int found = -1, oldest = 0;
    uint32_t oldest_time = 0xFFFFFFFF;
    bool is_rand = isRandomMac(mac);

    for (int i = 0; i < MAX_WIFI_DEVICES; i++) {
        if (wifiTable[i].active) {
            if (areMacsEqual(wifiTable[i].mac, mac)) { found = i; break; }
            if (wifiTable[i].last_seen_ms < oldest_time) { oldest_time = wifiTable[i].last_seen_ms; oldest = i; }
        } else if (found == -1) { oldest = i; oldest_time = 0; }
    }

    // DesaleatorizaciÃ³n (Chaaben et al., 2025)
    if (found == -1 && is_rand) {
        for (int i = 0; i < MAX_WIFI_DEVICES; i++) {
            if (wifiTable[i].active && wifiTable[i].is_random && matchFingerprint(wifiTable[i].fp, fp)) {
                found = i;
                memcpy(wifiTable[i].mac, mac, 6);
                break;
            }
        }
    }

    if (found != -1) {
        WiFiTrackedDevice &d = wifiTable[found];
        d.last_raw_rssi      = rssi;
        d.filtered_rssi      = d.kalman.update((float)rssi);
        d.estimated_distance = calculateDistance(d.filtered_rssi);
        d.packet_count++;
        d.last_seen_ms  = now;
        d.last_channel  = channel;
        d.fp            = fp;
        uint8_t newZone = getZoneId(d.estimated_distance);
        if (newZone != d.zone_id) {
            d.zone_id = newZone;
            Serial.printf("  [ZONA] %02X:%02X:%02X:%02X:%02X:%02X (%s) â†’ %s (%.1fm)\n",
                          mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
                          d.manufacturer, getZoneName(newZone), d.estimated_distance);
        }
        return;
    }

    // Nuevo dispositivo
    WiFiTrackedDevice &d = wifiTable[oldest];
    memcpy(d.mac, mac, 6);
    d.is_random      = is_rand;
    getManufacturerName(mac, fp, d.manufacturer, sizeof(d.manufacturer));
    d.fp             = fp;
    d.kalman.init((float)rssi);
    d.last_raw_rssi      = rssi;
    d.filtered_rssi      = (float)rssi;
    d.estimated_distance = calculateDistance(d.filtered_rssi);
    d.zone_id        = getZoneId(d.estimated_distance);
    d.packet_count   = 1;
    d.first_seen_ms  = now;
    d.last_seen_ms   = now;
    d.last_channel   = channel;
    d.active         = true;

    Serial.printf("  [+WiFi] %02X:%02X:%02X:%02X:%02X:%02X | %-18s | %4d dBm | %.1fm | %s\n",
                  mac[0],mac[1],mac[2],mac[3],mac[4],mac[5],
                  d.manufacturer, rssi, d.estimated_distance, getZoneName(d.zone_id));
}

// =========================================================================================
// 10. CALLBACK PROMISCUO (ISR â€” rÃ¡pido)
// =========================================================================================

void wifiPromiscuousCallback(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    uint16_t len = pkt->rx_ctrl.sig_len;
    if (len < 24 || pkt->rx_ctrl.rssi < RSSI_MIN_THRESHOLD) return;
    if ((payload[0] & 0xFC) != 0x40) return; // Solo Probe Requests (evita saturaciÃ³n)
    const uint8_t *src = &payload[10];
    if (areMacsEqual(src, OWN_ESP32_MAC)) return;
    if (src[0] == 0xFF || (src[0] & 0x01)) return;

    if (snifferQueue) {
        SnifferPacketMessage msg;
        msg.rssi    = pkt->rx_ctrl.rssi;
        msg.channel = pkt->rx_ctrl.channel;
        msg.length  = (len > MAX_PAYLOAD_COPY) ? MAX_PAYLOAD_COPY : len;
        memcpy(msg.payload, payload, msg.length);
        BaseType_t hp = pdFALSE;
        xQueueSendToBackFromISR(snifferQueue, &msg, &hp);
        if (hp) portYIELD_FROM_ISR();
    }
}

// =========================================================================================
// 11. TAREA FREERTOS â€” PROCESADOR DE PAQUETES WiFi (Core 1)
// =========================================================================================

void snifferTask(void *param) {
    SnifferPacketMessage msg;
    DeviceFingerprint fp;
    for (;;) {
        if (xQueueReceive(snifferQueue, &msg, portMAX_DELAY)) {
            parseProbeRequest(msg.payload, msg.length, fp);
            registerOrUpdateWiFiDevice(&msg.payload[10], msg.rssi, msg.channel, fp);
        }
    }
}

// =========================================================================================
// 12. CLASIFICADOR BLE â€” AUDÃFONOS Y DISPOSITIVOS
// =========================================================================================

BLEClassification classifyBLEDevice(BLEAdvertisedDevice &dev) {
    BLEClassification r;
    r.is_headphone = false;
    strncpy(r.name, "BLE Desconocido", sizeof(r.name));
    strncpy(r.type, "Dispositivo BLE", sizeof(r.type));

    // 1. Por datos del fabricante (Manufacturer Specific Data)
    if (dev.haveManufacturerData()) {
        String mf = dev.getManufacturerData().c_str();
        if (mf.length() >= 2) {
            uint16_t mfId = (uint8_t)mf[0] | ((uint8_t)mf[1] << 8);
            switch (mfId) {
                case 0x004C: { // Apple Inc.
                    if (mf.length() >= 3) {
                        uint8_t appleType = (uint8_t)mf[2];
                        r.is_headphone = true;
                        switch (appleType) {
                            case 0x07: strncpy(r.name,"AirPods",sizeof(r.name));         strncpy(r.type,"Audifono Apple",sizeof(r.type)); break;
                            case 0x0E: strncpy(r.name,"AirPods Pro",sizeof(r.name));     strncpy(r.type,"Audifono Apple",sizeof(r.type)); break;
                            case 0x0A: strncpy(r.name,"AirPods Max",sizeof(r.name));     strncpy(r.type,"Audifono Apple",sizeof(r.type)); break;
                            case 0x11: strncpy(r.name,"AirPods (3ra)",sizeof(r.name));   strncpy(r.type,"Audifono Apple",sizeof(r.type)); break;
                            case 0x14: strncpy(r.name,"AirPods Pro 2",sizeof(r.name));   strncpy(r.type,"Audifono Apple",sizeof(r.type)); break;
                            case 0x01: r.is_headphone = false; strncpy(r.name,"iPhone/iPad",sizeof(r.name)); strncpy(r.type,"Apple iOS",sizeof(r.type)); break;
                            case 0x05: r.is_headphone = false; strncpy(r.name,"Apple Watch",sizeof(r.name)); strncpy(r.type,"Wearable Apple",sizeof(r.type)); break;
                            case 0x06: r.is_headphone = false; strncpy(r.name,"AirPlay",sizeof(r.name)); strncpy(r.type,"Audio Apple",sizeof(r.type)); break;
                            default:   r.is_headphone = false; strncpy(r.name,"Apple Device",sizeof(r.name)); strncpy(r.type,"Apple BLE",sizeof(r.type)); break;
                        }
                    }
                    break;
                }
                case 0x012D: // Sony Corporation
                    r.is_headphone = true;
                    strncpy(r.name, "Sony WH/WF", sizeof(r.name));
                    strncpy(r.type, "Audifono Sony", sizeof(r.type));
                    break;
                case 0x0075: // Samsung Electronics
                    if (mf.length() >= 4 && (uint8_t)mf[2] == 0x42) {
                        r.is_headphone = true;
                        strncpy(r.name, "Galaxy Buds", sizeof(r.name));
                        strncpy(r.type, "Audifono Samsung", sizeof(r.type));
                    } else {
                        strncpy(r.name, "Samsung BLE", sizeof(r.name));
                        strncpy(r.type, "Samsung Device", sizeof(r.type));
                    }
                    break;
                case 0x009E: // Bose Corporation
                    r.is_headphone = true;
                    strncpy(r.name, "Bose Headphone", sizeof(r.name));
                    strncpy(r.type, "Audifono Bose", sizeof(r.type));
                    break;
                case 0x0122: // GN Audio (Jabra)
                    r.is_headphone = true;
                    strncpy(r.name, "Jabra Headset", sizeof(r.name));
                    strncpy(r.type, "Audifono Jabra", sizeof(r.type));
                    break;
                case 0x0310: // Xiaomi / MIUI
                    strncpy(r.name, "Xiaomi BLE", sizeof(r.name));
                    strncpy(r.type, "Xiaomi Device", sizeof(r.type));
                    break;
                default:
                    break;
            }
        }
    }

    // 2. Por nombre del dispositivo (si existe)
    if (dev.haveName()) {
        String devName = dev.getName().c_str();
        // Copiar nombre al resultado si no fue identificado por fabricante
        if (strncmp(r.name, "BLE Desconocido", 15) == 0 && devName.length() > 0) {
            strncpy(r.name, devName.c_str(), sizeof(r.name) - 1);
        }
        // Detectar audÃ­fonos por nombre
        const char* keywords[] = {"bud","ear","pod","head","audio","airpod","wh-","wf-","pixel bud","jabra","bose","sennheiser","anker","soundcore","beats","studio","powerbeats"};
        String lower = devName;
        lower.toLowerCase();
        for (auto kw : keywords) {
            if (lower.indexOf(kw) >= 0) {
                r.is_headphone = true;
                if (strncmp(r.type, "Dispositivo BLE", 15) == 0)
                    strncpy(r.type, "Audifono BLE", sizeof(r.type));
                break;
            }
        }
    }

    // 3. Por UUID de servicio de audio
    if (dev.haveServiceUUID()) {
        // A2DP Sink = 0x110B, HFP = 0x111E
        BLEUUID audioSink = BLEUUID((uint16_t)0x110B);
        BLEUUID hfp       = BLEUUID((uint16_t)0x111E);
        if (dev.isAdvertisingService(audioSink) || dev.isAdvertisingService(hfp)) {
            r.is_headphone = true;
            if (strncmp(r.type, "Dispositivo BLE", 15) == 0)
                strncpy(r.type, "Audifono/Auricular", sizeof(r.type));
        }
    }

    return r;
}

// Callback BLE
class BLECallback : public BLEAdvertisedDeviceCallbacks {
    void onResult(BLEAdvertisedDevice dev) override {
        uint32_t now = millis();
        String macStr = dev.getAddress().toString().c_str();
        char mac_c[18];
        strncpy(mac_c, macStr.c_str(), sizeof(mac_c) - 1);

        // Buscar si ya estÃ¡ en la tabla
        int found = -1, oldest = 0;
        uint32_t oldest_time = 0xFFFFFFFF;
        for (int i = 0; i < MAX_BLE_DEVICES; i++) {
            if (bleTable[i].active && strncmp(bleTable[i].mac_str, mac_c, 17) == 0) {
                found = i; break;
            }
            if (!bleTable[i].active || bleTable[i].last_seen_ms < oldest_time) {
                oldest_time = bleTable[i].active ? bleTable[i].last_seen_ms : 0;
                oldest = i;
            }
        }

        BLEClassification cls = classifyBLEDevice(dev);
        int slot = (found != -1) ? found : oldest;

        strncpy(bleTable[slot].mac_str,     mac_c,    sizeof(bleTable[slot].mac_str) - 1);
        strncpy(bleTable[slot].name,        cls.name, sizeof(bleTable[slot].name) - 1);
        strncpy(bleTable[slot].device_type, cls.type, sizeof(bleTable[slot].device_type) - 1);
        bleTable[slot].rssi         = dev.getRSSI();
        bleTable[slot].is_random    = (mac_c[1] == '2' || mac_c[1] == '6' || mac_c[1] == 'a' || mac_c[1] == 'e');
        bleTable[slot].is_headphone = cls.is_headphone;
        bleTable[slot].last_seen_ms = now;
        bleTable[slot].active       = true;

        if (found == -1) {
            Serial.printf("  [+BLE] %s | %-18s | %s | %d dBm\n",
                          mac_c, cls.name, cls.type, dev.getRSSI());
        }
    }
};

static BLECallback bleCB;

// =========================================================================================
// 13. CONTEO DE PERSONAS (Salice et al., 2024)
// =========================================================================================

PeopleCount countPeople() {
    uint32_t now = millis();
    PeopleCount pc;
    pc.wifi_active = 0; pc.ble_active = 0;

    // Depurar inactivos WiFi
    for (int i = 0; i < MAX_WIFI_DEVICES; i++) {
        if (wifiTable[i].active) {
            if (now - wifiTable[i].last_seen_ms > DEVICE_TIMEOUT_MS) wifiTable[i].active = false;
            else pc.wifi_active++;
        }
    }
    // Depurar inactivos BLE
    for (int i = 0; i < MAX_BLE_DEVICES; i++) {
        if (bleTable[i].active) {
            if (now - bleTable[i].last_seen_ms > (DEVICE_TIMEOUT_MS * 2)) bleTable[i].active = false;
            else pc.ble_active++;
        }
    }

    pc.total_devices = pc.wifi_active + pc.ble_active;
    // Ratio 1.5 disp/persona (Salice et al., 2024 â€” aulas universitarias)
    pc.auto_estimate = (uint16_t)roundf((float)pc.total_devices / 1.5f);
    if (pc.total_devices > 0 && pc.auto_estimate == 0) pc.auto_estimate = 1;
    pc.manual_offset = manualOffset;
    int total = (int)pc.auto_estimate + manualOffset;
    pc.total_persons = (total < 0) ? 0 : (uint16_t)total;
    return pc;
}

// =========================================================================================
// 14. MQTT â€” PUBLICACIÃ“N Y SUSCRIPCIÃ“N
// =========================================================================================

void mqttCallback(char *topic, byte *payload, unsigned int len) {
    // Recibe ajuste manual desde el dashboard: {"action":"adjust","delta":1}
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, len) == DeserializationError::Ok) {
        if (doc.containsKey("delta")) {
            int delta = doc["delta"].as<int>();
            manualOffset += delta;
            Serial.printf("\n[MQTT] Ajuste manual recibido: delta=%+d â†’ offset total=%+d\n", delta, manualOffset);
        } else if (doc.containsKey("action") && strcmp(doc["action"], "reset") == 0) {
            manualOffset = 0;
            Serial.println("\n[MQTT] Offset manual reiniciado a 0.");
        }
    }
}

bool connectAWS() {
    Serial.print("[AWS] Conectando a WiFi (");
    Serial.print(WIFI_SSID);
    Serial.print(")... ");

    // 1. Desactivar COMPLETAMENTE el modo promiscuo y limpiar filtros de hardware
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
    wifi_promiscuous_filter_t emptyFilter = {0};
    esp_wifi_set_promiscuous_filter(&emptyFilter);

    // 2. Desinicializar BLE COMPLETAMENTE para liberar ~100 KB de heap para TLS
    if (pBLEScan) {
        pBLEScan->stop();
        pBLEScan->clearResults();
    }
    // BLEDevice::deinit libera toda la memoria del stack BLE (NimBLE/Bluedroid)
    // Es necesario porque mbedTLS necesita >80 KB contiguos para el handshake TLS
    BLEDevice::deinit(true);
    pBLEScan = nullptr;
    delay(200);
    Serial.printf("[AWS] Heap tras deinit BLE: %u B  |  Max bloque: %u B\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // 3. Reinicio limpio del subsistema WiFi en modo EstaciÃ³n (STA)
    WiFi.persistent(false);
    WiFi.disconnect(true, true);
    WiFi.mode(WIFI_OFF);
    delay(200);
    WiFi.mode(WIFI_STA);
    delay(100);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
        delay(400); Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
        Serial.printf(" FALLO WiFi (status=%d).\n", WiFi.status());
        Serial.println("  [DIAGNÃ“STICO] Escaneando redes 2.4 GHz visibles por el ESP32...");
        int n = WiFi.scanNetworks();
        if (n == 0) {
            Serial.println("  --> Â¡No se detectÃ³ NINGUNA red Wi-Fi en 2.4 GHz!");
        } else {
            Serial.printf("  --> %d redes encontradas alrededor:\n", n);
            for (int i = 0; i < n && i < 10; ++i) {
                Serial.printf("      * \"%s\" (SeÃ±al: %d dBm, Ch: %d)\n",
                              WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i));
            }
        }
        return false;
    }
    Serial.printf(" OK (IP: %s)\n", WiFi.localIP().toString().c_str());

    // Sincronizar hora NTP obligatoria para validaciÃ³n TLS de certificados AWS
    Serial.print("[AWS] Sincronizando hora NTP... ");
    configTime(-5 * 3600, 0, "pool.ntp.org", "time.google.com"); // UTC-5 (PerÃº)
    time_t nowSec = time(nullptr);
    uint32_t tSync = millis();
    while (nowSec < 1672531200 && millis() - tSync < 6000) {
        delay(250);
        Serial.print(".");
        nowSec = time(nullptr);
    }
    if (nowSec >= 1672531200) {
        struct tm timeinfo;
        gmtime_r(&nowSec, &timeinfo);
        Serial.printf(" OK (%04d-%02d-%02d %02d:%02d:%02d)\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    } else {
        Serial.println(" AVISO (continuando...)");
    }

    // â”€â”€ DIAGNÃ“STICO DE MEMORIA ANTES DE TLS â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
    Serial.printf("[AWS] Heap libre: %u B  |  Max bloque: %u B\n",
                  ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    if (ESP.getFreeHeap() < 50000) {
        Serial.println("[AWS] *** ADVERTENCIA: heap insuficiente para TLS (<50 KB) ***");
    }

    secureClient.stop();   // limpiar sesiÃ³n anterior
    delay(100);

    secureClient.setTimeout(30);
    secureClient.setHandshakeTimeout(30);
    secureClient.setCACert(AWS_CERT_CA);
    secureClient.setCertificate(AWS_CERT_CRT);
    secureClient.setPrivateKey(AWS_CERT_KEY);
    mqttClient.setServer(AWS_IOT_ENDPOINT, AWS_IOT_PORT);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setKeepAlive(60);
    mqttClient.setSocketTimeout(30);
    mqttClient.setBufferSize(4096);   // â† CRÃTICO: evita fallo en publish >256 bytes

    Serial.print("[AWS] Conectando MQTT a AWS IoT Core... ");
    // Reintentar hasta 3 veces
    bool connected = false;
    for (int intento = 1; intento <= 3 && !connected; intento++) {
        if (intento > 1) {
            Serial.printf("      [Reintento %d/3]... ", intento);
            secureClient.stop();
            delay(1000);
        }
        connected = mqttClient.connect(DEVICE_ID);
        if (!connected) {
            int st = mqttClient.state();
            char errBuf[160] = "";
            int lastErr = secureClient.lastError(errBuf, sizeof(errBuf));
            Serial.printf(" FALLO (estado=%d", st);
            if (lastErr != 0) Serial.printf(", TLS=%d: %s", lastErr, errBuf);
            Serial.println(")");
        }
    }
    if (!connected) {
        return false;
    }
    Serial.println(" Â¡CONEXIÃ“N EXITOSA! OK");
    bool subOk = mqttClient.subscribe(TOPIC_ADJUST);
    Serial.printf("[AWS] Subscrito a %s: %s\n", TOPIC_ADJUST, subOk ? "OK" : "FALLO");
    return true;
}

bool publishScan() {
    PeopleCount pc = countPeople();
    uint32_t now = millis();

    // Documento JSON (ArduinoJson v6)
    DynamicJsonDocument doc(4096);
    doc["device_id"]  = DEVICE_ID;
    doc["poi"]        = currentPoiId;
    doc["timestamp"]  = now;
    doc["phase"]      = "UPLOAD";
    doc["channel"]    = currentChannel;
    doc["free_heap"]  = ESP.getFreeHeap();

    // Personas
    JsonObject people = doc.createNestedObject("people");
    people["auto_estimate"]  = pc.auto_estimate;
    people["manual_offset"]  = pc.manual_offset;
    people["total"]          = pc.total_persons;
    people["wifi_devices"]   = pc.wifi_active;
    people["ble_devices"]    = pc.ble_active;
    people["total_devices"]  = pc.total_devices;

    // WiFi devices
    JsonObject wifi = doc.createNestedObject("wifi");
    wifi["count"] = pc.wifi_active;
    JsonArray wifiArr = wifi.createNestedArray("devices");
    for (int i = 0; i < MAX_WIFI_DEVICES && wifiArr.size() < 20; i++) {
        if (!wifiTable[i].active) continue;
        WiFiTrackedDevice &d = wifiTable[i];
        JsonObject dj = wifiArr.createNestedObject();
        char mac_s[18];
        snprintf(mac_s, sizeof(mac_s), "%02X:%02X:%02X:%02X:%02X:%02X",
                 d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5]);
        dj["mac"]       = mac_s;
        dj["type"]      = d.manufacturer;
        dj["rssi"]      = (int)d.filtered_rssi;
        dj["dist"]      = serialized(String(d.estimated_distance, 1));
        dj["zone"]      = getZoneName(d.zone_id);
        dj["zone_id"]   = d.zone_id;
        dj["is_random"] = d.is_random;
        dj["pkts"]      = d.packet_count;
        dj["channel"]   = d.last_channel;
    }

    // BLE devices
    JsonObject ble = doc.createNestedObject("ble");
    ble["count"] = pc.ble_active;
    JsonArray bleArr = ble.createNestedArray("devices");
    for (int i = 0; i < MAX_BLE_DEVICES && bleArr.size() < 10; i++) {
        if (!bleTable[i].active) continue;
        BLETrackedDevice &b = bleTable[i];
        JsonObject bj = bleArr.createNestedObject();
        bj["mac"]          = b.mac_str;
        bj["name"]         = b.name;
        bj["type"]         = b.device_type;
        bj["rssi"]         = b.rssi;
        bj["is_random"]    = b.is_random;
        bj["is_headphone"] = b.is_headphone;
    }

    char buf[4096];
    size_t n = serializeJson(doc, buf, sizeof(buf));

    // Asegurar que el buffer MQTT sea suficiente para el JSON
    if (mqttClient.getBufferSize() < (n + 128)) {
        mqttClient.setBufferSize(n + 256);
    }

    // PublicaciÃ³n de escaneo completo a AWS IoT Core
    bool ok = false;
    for (int intento = 1; intento <= 3 && !ok; intento++) {
        if (!mqttClient.connected()) {
            Serial.printf("[MQTT] Reconectando antes de publish (intento %d)... ", intento);
            if (!mqttClient.connect(DEVICE_ID)) {
                Serial.printf("Fallo connect (st=%d)\n", mqttClient.state());
                delay(1000);
                continue;
            }
            Serial.println("Reconectado OK.");
            mqttClient.loop();
            delay(200);
        }
        ok = mqttClient.publish(TOPIC_PUBLISH, buf);
        if (!ok) delay(500);
    }
    Serial.printf("[MQTT] Publicado: %u bytes â€” %s\n", n, ok ? "OK âœ“" : "FALLO");
    return ok;
}

// =========================================================================================
// 15. VISUALIZACIÃ“N EN MONITOR SERIE (tabla cada 10 s)
// =========================================================================================

void printTable() {
    PeopleCount pc = countPeople();
    uint32_t now = millis();
    int imm=0, near=0, mid=0, far=0;
    for (int i=0;i<MAX_WIFI_DEVICES;i++) if(wifiTable[i].active){ auto z=wifiTable[i].zone_id; if(z==0)imm++; else if(z==1)near++; else if(z==2)mid++; else far++; }

    Serial.println();
    Serial.println("+----------------------------------------------------------------------------------------------------+");
    Serial.printf("| UNSAAC ESP32 V3 | Uptime: %02lu:%02lu | POI:#%-2d | Fase: %-12s | Canal: %-2d           |\n",
                  (now/1000)/60, (now/1000)%60, currentPoiId,
                  currentPhase==PHASE_WIFI_SNIFF?"WIFI_SNIFF":currentPhase==PHASE_BLE_SCAN?"BLE_SCAN":"UPLOAD",
                  currentChannel);
    Serial.println("+------ PERSONAS -------+------ DISPOSITIVOS WiFi ------+------ BLE ------+---- HEAP ----|");
    Serial.printf("| Auto:%-3u | Total:%-3u | WiFi:%-3u Rand:%-3u Real:%-3u  | BLE:%-3u Audf:%-2u | %6u B      |\n",
                  pc.auto_estimate, pc.total_persons, pc.wifi_active,
                  [&]{ int r=0; for(int i=0;i<MAX_WIFI_DEVICES;i++) if(wifiTable[i].active && wifiTable[i].is_random) r++; return r; }(),
                  [&]{ int r=0; for(int i=0;i<MAX_WIFI_DEVICES;i++) if(wifiTable[i].active && !wifiTable[i].is_random) r++; return r; }(),
                  pc.ble_active,
                  [&]{ int r=0; for(int i=0;i<MAX_BLE_DEVICES;i++) if(bleTable[i].active && bleTable[i].is_headphone) r++; return r; }(),
                  ESP.getFreeHeap());
    Serial.printf("| Ajuste manual: %+3d    | Inmediato:%-2d Cerca:%-2d Medio:%-2d Lejos:%-2d                       |\n",
                  manualOffset, imm, near, mid, far);
    Serial.println("+-------------------+----------------------+----------+----------+--------------------+------+----+");
    Serial.println("| MAC               | TIPO/FABRICANTE      |RSSI (dBm)| DIST (m) | ZONA               | PKTS | CH |");
    Serial.println("+-------------------+----------------------+----------+----------+--------------------+------+----+");
    for (int i=0;i<MAX_WIFI_DEVICES;i++) {
        if (!wifiTable[i].active) continue;
        WiFiTrackedDevice &d = wifiTable[i];
        Serial.printf("| %02X:%02X:%02X:%02X:%02X:%02X | %-20s | %5.1f    | %5.1f    | %-18s | %4u | %2d |\n",
                      d.mac[0],d.mac[1],d.mac[2],d.mac[3],d.mac[4],d.mac[5],
                      d.manufacturer, d.filtered_rssi, d.estimated_distance,
                      getZoneName(d.zone_id), d.packet_count, d.last_channel);
    }
    if (pc.ble_active > 0) {
        Serial.println("+-------------------+----------------------+----------------------+------------------------+");
        Serial.println("| MAC (BLE)         | NOMBRE               | TIPO                 | RSSI                   |");
        Serial.println("+-------------------+----------------------+----------------------+------------------------+");
        for (int i=0;i<MAX_BLE_DEVICES;i++) {
            if (!bleTable[i].active) continue;
            BLETrackedDevice &b = bleTable[i];
            Serial.printf("| %-17s | %-20s | %-20s | %4d dBm %s          |\n",
                          b.mac_str, b.name, b.device_type, b.rssi, b.is_headphone?"ðŸŽ§":"   ");
        }
    }
    Serial.println("+----------------------------------------------------------------------------------------------------+");
    Serial.println(">>> [t]=tabla [+]=POI [r]=reset [a/z]=ajuste+1/-1 <<<\n");
}

// =========================================================================================
// 16. FASES TDM (Time-Division Multiplexing WiFi/BLE/Upload)
// =========================================================================================

void startPhaseWiFiSniff() {
    currentPhase = PHASE_WIFI_SNIFF;
    phaseStartTime = millis();
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);
    esp_wifi_set_promiscuous_rx_cb(&wifiPromiscuousCallback);
    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    Serial.println("\n-------------------------------------------------------------");
    Serial.printf("[FASE 1/3] WiFi Sniffer (25 s) â€” Canal %d...\n", currentChannel);
    Serial.println("-------------------------------------------------------------");
}

void startPhaseBLEScan() {
    currentPhase = PHASE_BLE_SCAN;
    phaseStartTime = millis();
    esp_wifi_set_promiscuous(false); // Pausar WiFi para liberar radio RF
    Serial.println("\n-------------------------------------------------------------");
    Serial.println("[FASE 2/3] BLE Scan (8 s) â€” Escaneando audÃ­fonos/wearables...");
    Serial.println("-------------------------------------------------------------");
    if (pBLEScan) {
        pBLEScan->clearResults();
        pBLEScan->start(8, false); // Bloqueante 8s
        pBLEScan->clearResults();
        int bleCount = 0;
        for (int i = 0; i < MAX_BLE_DEVICES; i++) if (bleTable[i].active) bleCount++;
        Serial.printf("[BLE] Escaneo finalizado. Dispositivos BLE detectados: %d\n", bleCount);
    }
}

void startPhaseUpload() {
    currentPhase = PHASE_UPLOAD;
    phaseStartTime = millis();
    esp_wifi_set_promiscuous(false);
    Serial.println("\n-------------------------------------------------------------");
    Serial.println("[FASE 3/3] Upload AWS IoT Core (MQTT TLS)...");
    Serial.println("-------------------------------------------------------------");

    WiFi.mode(WIFI_STA);
    if (connectAWS()) {
        mqttClient.loop();
        delay(200);
        if (mqttClient.connected()) {
            publishScan();
            mqttClient.loop();
            delay(500);
        } else {
            Serial.println("[MQTT] Conexion caida antes de publish.");
        }
        mqttClient.disconnect();
    }

    // Cerrar TLS y WiFi
    secureClient.stop();
    WiFi.disconnect(true);
    delay(300);

    // IMPORTANTE: BLEDevice::deinit + reinit en el mismo ciclo causa
    // Guru Meditation StoreProhibited. Usar ESP.restart() para reiniciar
    // limpiamente â€” setup() inicializa BLE fresco en el proximo ciclo.
    Serial.println("[AWS] Upload completo. Reiniciando para proximo ciclo...");
    delay(300);
    ESP.restart();
}

void handleChannelHopping() {
    if (currentPhase != PHASE_WIFI_SNIFF) return;
    uint32_t now = millis();
    if (now - lastChannelHopTime >= HOP_INTERVAL_MS) {
        lastChannelHopTime = now;
        currentChannel++;
        if (currentChannel > MAX_CHANNEL) currentChannel = MIN_CHANNEL;
        esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    }
}

void handlePoiButton() {
    if (digitalRead(POI_BUTTON_PIN) == LOW) {
        uint32_t now = millis();
        if (now - lastButtonPressTime > DEBOUNCE_DELAY_MS) {
            lastButtonPressTime = now;
            currentPoiId++;
            Serial.printf("\n[BOOT] *** POI #%d marcado ***\n", currentPoiId);
            printTable();
        }
    }
}

void handleSerialCommands() {
    if (!Serial.available()) return;
    char cmd = Serial.read();
    switch (cmd) {
        case 't': case 'T': printTable(); break;
        case '+': case 'p': currentPoiId++; Serial.printf("[SERIAL] POI â†’ #%d\n", currentPoiId); break;
        case 'r': case 'R':
            memset(wifiTable, 0, sizeof(wifiTable));
            memset(bleTable,  0, sizeof(bleTable));
            manualOffset = 0;
            Serial.println("[SERIAL] Tablas WiFi+BLE reiniciadas.\n");
            break;
        case 'a': case 'A': manualOffset++; Serial.printf("[SERIAL] Offset manual â†’ %+d\n", manualOffset); break;
        case 'z': case 'Z': manualOffset--; Serial.printf("[SERIAL] Offset manual â†’ %+d\n", manualOffset); break;
    }
}

// =========================================================================================
// 17. SETUP & LOOP
// =========================================================================================

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n================================================================================");
    Serial.println("   UNSAAC â€” Sistema Embebido de DetecciÃ³n Pasiva de Equipos V3");
    Serial.println("   WiFi Sniffer + BLE Scanner + AWS IoT Core + Dashboard MÃ³vil");
    Serial.println("================================================================================");
    Serial.printf("MAC ESP32: %02X:%02X:%02X:%02X:%02X:%02X\n",
                  OWN_ESP32_MAC[0],OWN_ESP32_MAC[1],OWN_ESP32_MAC[2],
                  OWN_ESP32_MAC[3],OWN_ESP32_MAC[4],OWN_ESP32_MAC[5]);

    pinMode(POI_BUTTON_PIN, INPUT_PULLUP);
    memset(wifiTable, 0, sizeof(wifiTable));
    memset(bleTable,  0, sizeof(bleTable));
    mqttClient.setBufferSize(4096);

    // Cola FreeRTOS para paquetes WiFi
    snifferQueue = xQueueCreate(16, sizeof(SnifferPacketMessage));
    if (!snifferQueue) {
        Serial.println("[ERROR FATAL] No se pudo crear la cola FreeRTOS!");
        while (1) delay(1000);
    }

    // Tarea de procesamiento en Core 1
    xTaskCreatePinnedToCore(snifferTask, "SnifferTask", 4096, NULL, 1, NULL, 1);

    // WiFi en modo STA (sin conectar â€” para promiscuo)
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    // Configurar modo promiscuo WiFi
    esp_wifi_set_promiscuous_rx_cb(&wifiPromiscuousCallback);
    wifi_promiscuous_filter_t filter;
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous(true);
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
    Serial.println("[OK] WiFi Sniffer promiscuo activo (canales 1â€“13).");

    // Inicializar BLE
    BLEDevice::init("");
    pBLEScan = BLEDevice::getScan();
    pBLEScan->setAdvertisedDeviceCallbacks(&bleCB, true);
    pBLEScan->setActiveScan(true);   // Scan activo: pide mÃ¡s info al dispositivo
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(99);
    Serial.println("[OK] BLE Scanner inicializado (AirPods, Sony, Samsung Buds, etc.).");

    lastTablePrintTime  = millis();
    lastChannelHopTime  = millis();
    phaseStartTime      = millis();
    currentPhase        = PHASE_WIFI_SNIFF;

    Serial.printf("[OK] Ciclo TDM: %ds WiFi â†’ %ds BLE â†’ %ds Upload\n",
                  PHASE_WIFI_DURATION/1000, PHASE_BLE_DURATION/1000, PHASE_UPLOAD_TIMEOUT/1000);
    Serial.println("[OK] Comandos: [t]=tabla [+]=POI [r]=reset [a/z]=ajuste+1/-1\n");
}

void loop() {
    uint32_t now = millis();

    // ---- GestiÃ³n de fases TDM ----
    switch (currentPhase) {
        case PHASE_WIFI_SNIFF:
            handleChannelHopping();
            if (now - phaseStartTime >= PHASE_WIFI_DURATION)
                startPhaseBLEScan();  // BLE scan es bloqueante 8s
            break;
        case PHASE_BLE_SCAN:
            // El scan ya terminÃ³ (bloqueante en startPhaseBLEScan)
            // Pasar a upload inmediatamente
            startPhaseUpload();
            break;
        case PHASE_UPLOAD:
            // Upload ya terminÃ³ (bloqueante en startPhaseUpload)
            startPhaseWiFiSniff();
            break;
    }

    // ---- Eventos periÃ³dicos ----
    handlePoiButton();
    handleSerialCommands();

    if (now - lastTablePrintTime >= TABLE_REFRESH_MS) {
        lastTablePrintTime = now;
        printTable();
    }

    delay(10);
}
