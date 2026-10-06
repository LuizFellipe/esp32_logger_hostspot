/*
 * RF Scanner PoC — BLE + Classic BT + WiFi via RF Time-Sharing
 *
 * ┌─────────────────────────────────────────────────────────────────────────┐
 * │ IMPORTANTE: Arduino IDE → Tools → Partition Scheme → "No OTA (Large    │
 * │ APP)" — necessário para compilar WiFi + BluetoothSerial + NimBLE        │
 * │ juntos sem estourar a Flash.                                             │
 * └─────────────────────────────────────────────────────────────────────────┘
 *
 * Arquitetura (RF Phase Sequencer):
 *   - Estado de fase em RTC_DATA_ATTR → sobrevive esp_restart(), apaga em
 *     power-off (retorna automaticamente para PHASE_BLE)
 *   - Ciclo: PHASE_BLE(0) → PHASE_BT(1) → PHASE_WIFI(2) → PHASE_BLE → ...
 *   - setup() roteia para a fase correta; loop() se auto-deleta
 *   - Cada fase: task orquestradora (Core 0) + consumer (Core 1)
 *   - Formato de saída boxed (┌/│/└) idêntico em todas as fases
 *   - Cada fase chama esp_restart() ao terminar — RAM e rádio reinicializados
 */

#include <Arduino.h>
#include <NimBLEDevice.h>       // PHASE_BLE  — NimBLE-Arduino v2.x
#include <BluetoothSerial.h>    // PHASE_BT   — Classic BT inquiry
#include <WiFi.h>               // PHASE_WIFI — scan assíncrono
#include <vector>
#include <string>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────────
// RF Phase Sequencer — Fases e Durações
// ─────────────────────────────────────────────────────────────────────────────
#define PHASE_BLE       0
#define PHASE_BT        1
#define PHASE_WIFI      2

#define PHASE_BLE_MS    30000UL   // Duração total da fase BLE
#define PHASE_BT_MS     15000UL   // Duração total da fase Classic BT
#define PHASE_WIFI_MS   10000UL   // Duração total da fase WiFi

// Inquiry auto-calibrado: PHASE_BT_MS menos 3s de margem para flush serial
#define BT_INQUIRY_MS   (PHASE_BT_MS - 3000UL)

// ─────────────────────────────────────────────────────────────────────────────
// Estado persistente em RTC Slow Memory (.rtc.bss)
// Omitir a inicialização (= 0) garante que o C-startup não apague no soft reset.
// ─────────────────────────────────────────────────────────────────────────────
RTC_DATA_ATTR static uint8_t  currentPhase;
RTC_DATA_ATTR static uint32_t phaseCount;

// ─────────────────────────────────────────────────────────────────────────────
// Mutex Serial compartilhado entre todas as fases
// ─────────────────────────────────────────────────────────────────────────────
static SemaphoreHandle_t serialMutex = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: imprime bytes em hex separados por ':'
// ─────────────────────────────────────────────────────────────────────────────
static void printHex(const uint8_t* data, uint8_t len) {
    for (uint8_t i = 0; i < len; i++) {
        if (i > 0) Serial.print(":");
        if (data[i] < 0x10) Serial.print("0");
        Serial.print(data[i], HEX);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Helper: Restart garantido preservando RTC_DATA_ATTR
// ─────────────────────────────────────────────────────────────────────────────
static void triggerRestart() {
    Serial.flush();
    // Um Deep Sleep de 10ms limpa a RAM e o Heap (mesmo efeito de esp_restart),
    // mas o bootloader tem a garantia de preservar o RTC_DATA_ATTR no wakeup.
    esp_sleep_enable_timer_wakeup(10000ULL);
    esp_deep_sleep_start();
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 0: BLE — NimBLE-Arduino v2.x
// Scan ativo de dispositivos BLE com deduplicação por MAC por ciclo.
// ═════════════════════════════════════════════════════════════════════════════

// Configuração BLE
#define BLE_SCAN_DURATION_MS        5000
#define BLE_QUEUE_DEPTH             10
#define BLE_CONSUMER_STACK          4096
#define BLE_CONSUMER_PRIORITY       1
#define BLE_ORCHESTRATOR_STACK      4096
#define BLE_ORCHESTRATOR_PRIORITY   2   // Prioridade maior que o consumer

// Limites do BLEDeviceRecord
#define BLE_DEVICE_NAME_LEN             65
#define BLE_URI_LEN                     64
#define BLE_UUID_STR_LEN                37
#define BLE_MAX_MANUFACTURER_ENTRIES    2
#define BLE_MAX_MANUFACTURER_DATA_LEN   32
#define BLE_MAX_SERVICE_UUIDS           4
#define BLE_MAX_SERVICE_DATA_ENTRIES    2
#define BLE_MAX_SERVICE_DATA_LEN        24

// BLE Device Record — alocado no heap via malloc, transportado por fila
struct BLEDeviceRecord {
    char     address[18];
    uint8_t  addressType;
    char     name[BLE_DEVICE_NAME_LEN];
    bool     hasName;
    int8_t   rssi;
    int8_t   txPower;
    bool     hasTxPower;
    uint8_t  advType;
    uint8_t  advFlags;
    bool     isConnectable;
    bool     isScannable;
    bool     isLegacy;
    uint16_t appearance;
    bool     hasAppearance;
    uint16_t advInterval;
    bool     hasAdvInterval;
    uint8_t  manufacturerDataCount;
    uint8_t  manufacturerData[BLE_MAX_MANUFACTURER_ENTRIES][BLE_MAX_MANUFACTURER_DATA_LEN];
    uint8_t  manufacturerDataLen[BLE_MAX_MANUFACTURER_ENTRIES];
    uint8_t  serviceUUIDCount;
    char     serviceUUIDs[BLE_MAX_SERVICE_UUIDS][BLE_UUID_STR_LEN];
    uint8_t  serviceDataCount;
    char     serviceDataUUIDs[BLE_MAX_SERVICE_DATA_ENTRIES][BLE_UUID_STR_LEN];
    uint8_t  serviceData[BLE_MAX_SERVICE_DATA_ENTRIES][BLE_MAX_SERVICE_DATA_LEN];
    uint8_t  serviceDataLen[BLE_MAX_SERVICE_DATA_ENTRIES];
    char     uri[BLE_URI_LEN];
    bool     hasURI;
    uint32_t timestamp;
};

static QueueHandle_t             bleQueue      = nullptr;
static NimBLEScan*               pBLEScan      = nullptr;
static std::vector<std::string>  seenInCycle;
static std::atomic<bool>         blePhaseEnding{false}; // Guarda race entre onScanEnd e orchestrator

// Consumer Task (Core 1): recebe BLEDeviceRecord da fila, imprime e libera
static void bleConsumerTask(void* pvParameters) {
    (void)pvParameters;
    BLEDeviceRecord* rec = nullptr;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[BLE Consumer] Task iniciada no Core %d\n", xPortGetCoreID());
    xSemaphoreGive(serialMutex);

    for (;;) {
        if (xQueueReceive(bleQueue, &rec, portMAX_DELAY) == pdTRUE && rec != nullptr) {
            xSemaphoreTake(serialMutex, portMAX_DELAY);

            Serial.println(F("┌─────────────────────────────────────────────────"));
            Serial.printf( "│ [BLE] Timestamp    : %lu ms\n",    (unsigned long)rec->timestamp);
            Serial.printf( "│ Address            : %s  (type=%u)\n", rec->address, rec->addressType);
            Serial.printf( "│ Name               : %s\n",        rec->hasName ? rec->name : "[sem nome]");
            Serial.printf( "│ RSSI               : %d dBm\n",   (int)rec->rssi);

            if (rec->hasTxPower)
                Serial.printf("│ TX Power           : %d dBm\n", (int)rec->txPower);
            else
                Serial.println(F("│ TX Power           : [não anunciado]"));

            Serial.printf("│ Adv Type           : 0x%02X\n",  rec->advType);
            Serial.printf("│ Adv Flags          : 0x%02X\n",  rec->advFlags);
            Serial.printf("│ Connectable        : %s\n",      rec->isConnectable ? "sim" : "nao");
            Serial.printf("│ Scannable          : %s\n",      rec->isScannable   ? "sim" : "nao");
            Serial.printf("│ Legacy Adv         : %s\n",      rec->isLegacy      ? "sim" : "nao");

            if (rec->hasAppearance)
                Serial.printf("│ Appearance         : 0x%04X\n", rec->appearance);
            if (rec->hasAdvInterval)
                Serial.printf("│ Adv Interval       : %u ms\n",  rec->advInterval);

            if (rec->manufacturerDataCount > 0) {
                for (uint8_t i = 0; i < rec->manufacturerDataCount; i++) {
                    Serial.printf("│ Manuf[%u]           : ", i);
                    printHex(rec->manufacturerData[i], rec->manufacturerDataLen[i]);
                    Serial.println();
                }
            } else {
                Serial.println(F("│ Manuf Data         : [nenhum]"));
            }

            if (rec->serviceUUIDCount > 0) {
                for (uint8_t i = 0; i < rec->serviceUUIDCount; i++)
                    Serial.printf("│ SvcUUID[%u]         : %s\n", i, rec->serviceUUIDs[i]);
            } else {
                Serial.println(F("│ Svc UUIDs          : [nenhum]"));
            }

            if (rec->serviceDataCount > 0) {
                for (uint8_t i = 0; i < rec->serviceDataCount; i++) {
                    Serial.printf("│ SvcData[%u]         : UUID=%s  data=", i, rec->serviceDataUUIDs[i]);
                    printHex(rec->serviceData[i], rec->serviceDataLen[i]);
                    Serial.println();
                }
            } else {
                Serial.println(F("│ Svc Data           : [nenhum]"));
            }

            if (rec->hasURI)
                Serial.printf("│ URI                : %s\n", rec->uri);

            Serial.println(F("└─────────────────────────────────────────────────"));
            Serial.println();

            xSemaphoreGive(serialMutex);
            free(rec);
            rec = nullptr;
        }
    }
}

// Callbacks do Scanner BLE
class ScanCallbacks : public NimBLEScanCallbacks {

    void onResult(const NimBLEAdvertisedDevice* dev) override {
        std::string addrStr = dev->getAddress().toString();

        // Deduplicação por MAC dentro do ciclo
        for (const auto& seen : seenInCycle) {
            if (seen == addrStr) return;
        }
        seenInCycle.push_back(addrStr);

        BLEDeviceRecord* rec = (BLEDeviceRecord*)malloc(sizeof(BLEDeviceRecord));
        if (rec == nullptr) return;
        memset(rec, 0, sizeof(BLEDeviceRecord));

        rec->timestamp     = (uint32_t)millis();
        rec->rssi          = dev->getRSSI();
        rec->advType       = dev->getAdvType();
        rec->advFlags      = dev->getAdvFlags();
        rec->isConnectable = dev->isConnectable();
        rec->isScannable   = dev->isScannable();
        rec->isLegacy      = dev->isLegacyAdvertisement();
        rec->addressType   = dev->getAddressType();
        strncpy(rec->address, addrStr.c_str(), sizeof(rec->address) - 1);

        if (dev->haveName()) {
            rec->hasName = true;
            strncpy(rec->name, dev->getName().c_str(), sizeof(rec->name) - 1);
        }
        if (dev->haveTXPower()) {
            rec->hasTxPower = true;
            rec->txPower    = dev->getTXPower();
        }
        if (dev->haveAppearance()) {
            rec->hasAppearance = true;
            rec->appearance    = dev->getAppearance();
        }
        if (dev->haveAdvInterval()) {
            rec->hasAdvInterval = true;
            rec->advInterval    = dev->getAdvInterval();
        }

        uint8_t mfCount = dev->getManufacturerDataCount();
        rec->manufacturerDataCount = (mfCount < BLE_MAX_MANUFACTURER_ENTRIES)
                                     ? mfCount : BLE_MAX_MANUFACTURER_ENTRIES;
        for (uint8_t i = 0; i < rec->manufacturerDataCount; i++) {
            std::string mfData = dev->getManufacturerData(i);
            uint8_t copyLen = (mfData.size() < BLE_MAX_MANUFACTURER_DATA_LEN)
                              ? (uint8_t)mfData.size() : BLE_MAX_MANUFACTURER_DATA_LEN;
            memcpy(rec->manufacturerData[i], mfData.data(), copyLen);
            rec->manufacturerDataLen[i] = copyLen;
        }

        uint8_t uuidCount = dev->getServiceUUIDCount();
        rec->serviceUUIDCount = (uuidCount < BLE_MAX_SERVICE_UUIDS)
                                ? uuidCount : BLE_MAX_SERVICE_UUIDS;
        for (uint8_t i = 0; i < rec->serviceUUIDCount; i++)
            strncpy(rec->serviceUUIDs[i],
                    dev->getServiceUUID(i).toString().c_str(), BLE_UUID_STR_LEN - 1);

        uint8_t sdCount = dev->getServiceDataCount();
        rec->serviceDataCount = (sdCount < BLE_MAX_SERVICE_DATA_ENTRIES)
                                ? sdCount : BLE_MAX_SERVICE_DATA_ENTRIES;
        for (uint8_t i = 0; i < rec->serviceDataCount; i++) {
            strncpy(rec->serviceDataUUIDs[i],
                    dev->getServiceDataUUID(i).toString().c_str(), BLE_UUID_STR_LEN - 1);
            std::string sd = dev->getServiceData(i);
            uint8_t copyLen = (sd.size() < BLE_MAX_SERVICE_DATA_LEN)
                              ? (uint8_t)sd.size() : BLE_MAX_SERVICE_DATA_LEN;
            memcpy(rec->serviceData[i], sd.data(), copyLen);
            rec->serviceDataLen[i] = copyLen;
        }

        if (dev->haveURI()) {
            rec->hasURI = true;
            strncpy(rec->uri, dev->getURI().c_str(), sizeof(rec->uri) - 1);
        }

        if (xQueueSend(bleQueue, &rec, 0) != pdTRUE) {
            free(rec); // Queue cheia: descarta sem bloquear o BT stack
        }
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        // Se o orchestrator sinalizou fim de fase, NÃO reinicia o scan
        if (blePhaseEnding) return;

        xSemaphoreTake(serialMutex, portMAX_DELAY);
        Serial.printf("\n[BLE Scanner] Ciclo encerrado | Devices únicos: %d | Razão: %d\n\n",
                      (int)seenInCycle.size(), reason);
        xSemaphoreGive(serialMutex);

        seenInCycle.clear();
        pBLEScan->clearResults();
        pBLEScan->start(BLE_SCAN_DURATION_MS);
    }

} scanCallbacks;

// Orchestrator Task BLE (Core 0): aguarda PHASE_BLE_MS e dispara restart
static void bleOrchestratorTask(void* pvParameters) {
    (void)pvParameters;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[BLE Orch] Core %d | Fase encerra em %lu ms\n",
                  xPortGetCoreID(), PHASE_BLE_MS);
    xSemaphoreGive(serialMutex);

    vTaskDelay(pdMS_TO_TICKS(PHASE_BLE_MS));

    // Sinaliza fim de fase ANTES de parar o scan para evitar race com onScanEnd
    blePhaseEnding = true;
    vTaskDelay(pdMS_TO_TICKS(10)); // pequena janela para onScanEnd ver o flag

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║  PHASE_BLE concluída — próxima: PHASE_BT        ║"));
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    pBLEScan->stop();
    vTaskDelay(pdMS_TO_TICKS(300)); // aguarda stop propagar
    
    NimBLEDevice::deinit(true);
    vTaskDelay(pdMS_TO_TICKS(100));

    currentPhase = PHASE_BT;
    triggerRestart();
}

static void initBLEPhase() {
    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║         PHASE 0: BLE SCAN (NimBLE v2.x)        ║"));
    Serial.printf( "║  Duração: %5lu ms | Ciclo: %-8lu             ║\n",
                   PHASE_BLE_MS, (unsigned long)phaseCount);
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    bleQueue = xQueueCreate(BLE_QUEUE_DEPTH, sizeof(BLEDeviceRecord*));
    if (bleQueue == nullptr) {
        Serial.println(F("ERRO: falha ao criar bleQueue"));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    NimBLEDevice::init("");
    pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(&scanCallbacks, false);
    pBLEScan->setActiveScan(true);
    pBLEScan->setInterval(100);
    pBLEScan->setWindow(100);
    pBLEScan->setMaxResults(0);

    xTaskCreatePinnedToCore(bleConsumerTask,     "BLE_Consumer",
                            BLE_CONSUMER_STACK,     nullptr,
                            BLE_CONSUMER_PRIORITY,  nullptr, 1);

    xTaskCreatePinnedToCore(bleOrchestratorTask, "BLE_Orch",
                            BLE_ORCHESTRATOR_STACK,     nullptr,
                            BLE_ORCHESTRATOR_PRIORITY,  nullptr, 0);

    pBLEScan->start(BLE_SCAN_DURATION_MS);
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 1: CLASSIC BT — BluetoothSerial
// Inquiry ativo de dispositivos Bluetooth Clássico.
// ═════════════════════════════════════════════════════════════════════════════

// Configuração Classic BT
#define BT_QUEUE_DEPTH              10
#define BT_CONSUMER_STACK           3072
#define BT_CONSUMER_PRIORITY        1
#define BT_ORCHESTRATOR_STACK       4096
#define BT_ORCHESTRATOR_PRIORITY    1
#define BT_DEVICE_NAME_LEN          65

// BT Device Record — alocado no heap via malloc, transportado por fila
struct BTDeviceRecord {
    char     address[18];
    char     name[BT_DEVICE_NAME_LEN];
    bool     hasName;
    uint32_t cod;       // Class of Device
    bool     hasCOD;
    int8_t   rssi;
    bool     hasRSSI;
    uint32_t timestamp;
};

static BluetoothSerial SerialBT;
static QueueHandle_t   btQueue = nullptr;

// Callback do inquiry — chamado pelo BT stack para cada dispositivo encontrado
static void btDeviceFoundCB(BTAdvertisedDevice* pDevice) {
    BTDeviceRecord* rec = (BTDeviceRecord*)malloc(sizeof(BTDeviceRecord));
    if (rec == nullptr) return;
    memset(rec, 0, sizeof(BTDeviceRecord));

    rec->timestamp = (uint32_t)millis();

    strncpy(rec->address, pDevice->getAddress().toString().c_str(),
            sizeof(rec->address) - 1);

    if (pDevice->haveName()) {
        rec->hasName = true;
        strncpy(rec->name, pDevice->getName().c_str(), sizeof(rec->name) - 1);
    }

    if (pDevice->haveCOD()) {
        rec->hasCOD = true;
        rec->cod    = pDevice->getCOD();
    }

    if (pDevice->haveRSSI()) {
        rec->hasRSSI = true;
        rec->rssi    = (int8_t)pDevice->getRSSI();
    }

    if (xQueueSend(btQueue, &rec, 0) != pdTRUE) {
        free(rec); // Queue cheia: descarta
    }
}

// Consumer Task (Core 1): recebe BTDeviceRecord da fila, imprime e libera
static void btConsumerTask(void* pvParameters) {
    (void)pvParameters;
    BTDeviceRecord* rec = nullptr;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[BT Consumer] Task iniciada no Core %d\n", xPortGetCoreID());
    xSemaphoreGive(serialMutex);

    for (;;) {
        if (xQueueReceive(btQueue, &rec, portMAX_DELAY) == pdTRUE && rec != nullptr) {
            xSemaphoreTake(serialMutex, portMAX_DELAY);

            Serial.println(F("┌─────────────────────────────────────────────────"));
            Serial.printf( "│ [Classic BT] Timestamp : %lu ms\n", (unsigned long)rec->timestamp);
            Serial.printf( "│ Address                : %s\n",     rec->address);
            Serial.printf( "│ Name                   : %s\n",     rec->hasName ? rec->name : "[sem nome]");

            if (rec->hasCOD)
                Serial.printf("│ CoD                    : 0x%06X\n", (unsigned int)rec->cod);
            else
                Serial.println(F("│ CoD                    : [não disponível]"));

            if (rec->hasRSSI)
                Serial.printf("│ RSSI                   : %d dBm\n", (int)rec->rssi);
            else
                Serial.println(F("│ RSSI                   : [não disponível]"));

            Serial.println(F("└─────────────────────────────────────────────────"));
            Serial.println();

            xSemaphoreGive(serialMutex);
            free(rec);
            rec = nullptr;
        }
    }
}

// Orchestrator Task Classic BT (Core 0): conduz inquiry e dispara restart
static void btOrchestratorTask(void* pvParameters) {
    (void)pvParameters;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[BT Orch] Core %d | Inquiry: %lu ms | Fase: %lu ms\n",
                  xPortGetCoreID(), BT_INQUIRY_MS, PHASE_BT_MS);
    xSemaphoreGive(serialMutex);

    // Inicia inquiry assíncrono (callback btDeviceFoundCB dispara por device)
    SerialBT.discoverAsync(btDeviceFoundCB);

    // Aguarda o tempo de inquiry configurado
    vTaskDelay(pdMS_TO_TICKS(BT_INQUIRY_MS));

    // Encerra inquiry e aguarda 2s para consumer drenar a fila
    SerialBT.discoverClear();
    vTaskDelay(pdMS_TO_TICKS(2000));

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║  PHASE_BT concluída — próxima: PHASE_WIFI       ║"));
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    SerialBT.end();
    vTaskDelay(pdMS_TO_TICKS(100));

    currentPhase = PHASE_WIFI;
    triggerRestart();
}

static void initBTPhase() {
    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║   PHASE 1: CLASSIC BT SCAN (BluetoothSerial)   ║"));
    Serial.printf( "║  Inquiry: %5lu ms | Ciclo: %-8lu             ║\n",
                   BT_INQUIRY_MS, (unsigned long)phaseCount);
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    btQueue = xQueueCreate(BT_QUEUE_DEPTH, sizeof(BTDeviceRecord*));
    if (btQueue == nullptr) {
        Serial.println(F("ERRO: falha ao criar btQueue"));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    SerialBT.begin("ESP32_BT_Scanner");

    xTaskCreatePinnedToCore(btConsumerTask,     "BT_Consumer",
                            BT_CONSUMER_STACK,     nullptr,
                            BT_CONSUMER_PRIORITY,  nullptr, 1);

    xTaskCreatePinnedToCore(btOrchestratorTask, "BT_Orch",
                            BT_ORCHESTRATOR_STACK,     nullptr,
                            BT_ORCHESTRATOR_PRIORITY,  nullptr, 0);
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE 2: WIFI SCAN — assíncrono (modelo dualcore)
// ═════════════════════════════════════════════════════════════════════════════

// Configuração WiFi
#define WIFI_ORCHESTRATOR_STACK     4096
#define WIFI_ORCHESTRATOR_PRIORITY  1

// Converte tipo de criptografia para string legível (modelo dualcore)
static const char* obterTipoCriptografia(wifi_auth_mode_t enc) {
    switch (enc) {
        case WIFI_AUTH_OPEN:             return "open";
        case WIFI_AUTH_WEP:              return "WEP";
        case WIFI_AUTH_WPA_PSK:          return "WPA";
        case WIFI_AUTH_WPA2_PSK:         return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK:     return "WPA+WPA2";
        case WIFI_AUTH_WPA2_ENTERPRISE:  return "WPA2-EAP";
        case WIFI_AUTH_WPA3_PSK:         return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK:    return "WPA2+WPA3";
        case WIFI_AUTH_WAPI_PSK:         return "WAPI";
        default:                         return "unknown";
    }
}

// Orchestrator Task WiFi (Core 0): scan assíncrono e dispara restart
static void wifiOrchestratorTask(void* pvParameters) {
    (void)pvParameters;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.printf("[WiFi Orch] Core %d | Fase: %lu ms\n",
                  xPortGetCoreID(), PHASE_WIFI_MS);
    xSemaphoreGive(serialMutex);

    // Inicia scan assíncrono (inclui redes com SSID oculto)
    WiFi.scanNetworks(true, true);

    unsigned long phaseStart = millis();
    bool          scanDone   = false;

    while ((millis() - phaseStart) < PHASE_WIFI_MS) {
        int16_t n = WiFi.scanComplete();

        if (n == WIFI_SCAN_FAILED) {
            xSemaphoreTake(serialMutex, portMAX_DELAY);
            Serial.println(F("[WiFi] Scan falhou. Reiniciando scan..."));
            xSemaphoreGive(serialMutex);
            WiFi.scanNetworks(true, true);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (n == WIFI_SCAN_RUNNING) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Scan concluído — imprime resultados
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        Serial.printf("\n[WiFi] %d rede(s) encontrada(s):\n\n", n);
        xSemaphoreGive(serialMutex);

        for (int i = 0; i < n; i++) {
            xSemaphoreTake(serialMutex, portMAX_DELAY);
            Serial.println(F("┌─────────────────────────────────────────────────"));
            Serial.printf( "│ [WiFi] Timestamp   : %lu ms\n",  (unsigned long)millis());
            Serial.printf( "│ SSID               : %s\n",      WiFi.SSID(i).c_str());
            Serial.printf( "│ BSSID              : %s\n",      WiFi.BSSIDstr(i).c_str());
            Serial.printf( "│ RSSI               : %d dBm\n", (int)WiFi.RSSI(i));
            Serial.printf( "│ Canal              : %d\n",      (int)WiFi.channel(i));
            Serial.printf( "│ Criptografia       : %s\n",      obterTipoCriptografia(WiFi.encryptionType(i)));
            Serial.println(F("└─────────────────────────────────────────────────"));
            Serial.println();
            xSemaphoreGive(serialMutex);
        }

        WiFi.scanDelete();
        scanDone = true;
        break;
    }

    if (!scanDone) {
        xSemaphoreTake(serialMutex, portMAX_DELAY);
        Serial.println(F("[WiFi] Tempo esgotado sem resultado de scan."));
        xSemaphoreGive(serialMutex);
        WiFi.scanDelete();
    }

    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(100));

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║  PHASE_WIFI concluída — próxima: PHASE_BLE      ║"));
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    currentPhase = PHASE_BLE;
    triggerRestart();
}

static void initWiFiPhase() {
    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n╔══════════════════════════════════════════════════╗"));
    Serial.println(F("║         PHASE 2: WIFI SCAN (async)              ║"));
    Serial.printf( "║  Duração: %5lu ms | Ciclo: %-8lu             ║\n",
                   PHASE_WIFI_MS, (unsigned long)phaseCount);
    Serial.println(F("╚══════════════════════════════════════════════════╝\n"));
    xSemaphoreGive(serialMutex);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    xTaskCreatePinnedToCore(wifiOrchestratorTask, "WiFi_Orch",
                            WIFI_ORCHESTRATOR_STACK,     nullptr,
                            WIFI_ORCHESTRATOR_PRIORITY,  nullptr, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// setup() — RF Phase Sequencer: roteia para a fase correta após cada reset
// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);

    serialMutex = xSemaphoreCreateMutex();
    if (serialMutex == nullptr) {
        Serial.println(F("ERRO CRITICO: falha ao criar serialMutex. Travando."));
        while (1) vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // Fallback de segurança caso a memória corrompa
    if (currentPhase > PHASE_WIFI) currentPhase = PHASE_BLE;

    phaseCount++;

    xSemaphoreTake(serialMutex, portMAX_DELAY);
    Serial.println(F("\n=== RF Scanner PoC — RF Phase Sequencer ==="));
    Serial.printf("Reset Reason : %d\n", (int)esp_reset_reason());
    Serial.printf("Fase atual : %d  |  Ciclo total: %lu\n\n",
                  (int)currentPhase, (unsigned long)phaseCount);
    xSemaphoreGive(serialMutex);

    switch (currentPhase) {
        case PHASE_BLE:  initBLEPhase();  break;
        case PHASE_BT:   initBTPhase();   break;
        case PHASE_WIFI: initWiFiPhase(); break;
    }
}

// loop() se auto-deleta — toda lógica reside nas tasks FreeRTOS
void loop() {
    vTaskDelete(nullptr);
}
