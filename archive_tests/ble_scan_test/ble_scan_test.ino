/*
 * Teste de scan BLE contínuo (NimBLE-Arduino).
 *
 * Bluetooth Clássico ficou fora: NimBLEDevice::init() libera a RAM do
 * Classic BT (esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT)) e
 * sobe o controller só em modo BLE — não dá pra usar BluetoothSerial
 * (Bluedroid clássico) junto no mesmo boot.
 *
 * Nota: caracteres ilegíveis nos primeiros ~1-2s após o reset (antes de
 * "Iniciando scan BLE...") são o bootloader ROM do ESP32, que imprime a
 * 74880 baud — normal, não é bug deste sketch.
 */

#include <Arduino.h>
#include <NimBLEDevice.h>

static constexpr uint32_t scanTimeMs = 10 * 1000; // duração de cada ciclo de scan

// Nome de dispositivo BLE pode vir com bytes binários/não-ASCII (firmware
// mal comportado, campo corrompido). Sem isso, esses bytes chegam crus no
// terminal serial e aparecem como caracteres ilegíveis.
static String sanitize(const std::string& raw) {
    String out;
    out.reserve(raw.size());
    for (char c : raw) {
        out += isPrintable((uint8_t)c) ? c : '.';
    }
    return out;
}

class ScanCallbacks : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* advertisedDevice) override {
        if (!advertisedDevice->haveName()) {
            return; // objetivo é só nome de dispositivo, ignora anônimos
        }
        Serial.printf("[RESULTADO] Nome: %-30s Endereco: %s RSSI: %d\n",
                       sanitize(advertisedDevice->getName()).c_str(),
                       advertisedDevice->getAddress().toString().c_str(),
                       advertisedDevice->getRSSI());
    }

    void onScanEnd(const NimBLEScanResults& results, int reason) override {
        Serial.printf("[SCAN] Ciclo encerrado (motivo=%d, %d resultados). Reiniciando scan...\n",
                       reason, results.getCount());
        NimBLEDevice::getScan()->start(scanTimeMs, false, true);
        Serial.println("[SCAN] Novo ciclo iniciado, aguardando dispositivos...");
    }
} scanCallbacks;

void setup() {
    Serial.begin(115200);
    delay(500); // dá tempo do monitor serial conectar antes dos primeiros prints
    Serial.println();
    Serial.println("[SETUP] Iniciando sketch de teste de scan BLE...");

    Serial.println("[SETUP] Chamando NimBLEDevice::init()...");
    NimBLEDevice::init("");
    Serial.println("[SETUP] NimBLEDevice::init() concluido.");

    NimBLEScan* pBLEScan = NimBLEDevice::getScan();
    pBLEScan->setScanCallbacks(&scanCallbacks, false); // sem cache de duplicados
    pBLEScan->setActiveScan(true);                     // ativo: pega nome na scan response
    pBLEScan->setMaxResults(0);                        // não guarda em RAM, só callback
    Serial.println("[SETUP] Parametros de scan configurados (ativo, sem cache).");

    Serial.printf("[SETUP] Iniciando primeiro ciclo de scan (%lu ms)...\n", scanTimeMs);
    pBLEScan->start(scanTimeMs, false, true);
    Serial.println("[SETUP] Scan em andamento, aguardando dispositivos...");
}

void loop() {
    static uint32_t lastBeat = 0;
    if (millis() - lastBeat >= 5000) {
        lastBeat = millis();
        Serial.printf("[LOOP] Programa vivo, scan ativo=%d, uptime=%lus\n",
                       NimBLEDevice::getScan()->isScanning(), millis() / 1000);
    }
}
