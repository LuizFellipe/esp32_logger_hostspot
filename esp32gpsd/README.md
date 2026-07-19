# ESP32 GPS Logger — Documentação Completa

Rastreador baseado em ESP32 que coleta dados de **GPS**, **temperatura/umidade (DHT22)**, **aceleração/giroscópio (MPU6050)**, **redes WiFi**, **dispositivos BLE** e **dispositivos Bluetooth Clássico** próximos, gravando tudo em arquivos CSV no cartão SD (via SdFat) para análise posterior.

---

## Visão Geral

```
┌───────────────────────────────────────────────────────────────────────────┐
│                                  ESP32                                    │
│                                                                            │
│  GPS (NEO-6M) ─► TinyGPS ─► processarDadosGPS()  ◄── roda em TODAS as     │
│  DHT22 (GPIO32) ─────────────────────┘               fases (WiFi/BLE/BT) │
│  MPU6050 (I2C) ─► média ~10 amostras/ciclo                                │
│                                                                            │
│  ┌──────────────────────────────────────────────────────────────────┐    │
│  │            RF Phase Sequencer (nunca 2 rádios ligados juntos)    │    │
│  │                                                                    │    │
│  │   PHASE_WIFI (~5 min)  ──►  PHASE_BLE (30 s)  ──►  PHASE_BT (15s) │    │
│  │        ▲                                                    │     │    │
│  │        └────────────────────────────────────────────────────┘     │    │
│  │        cada transição: flushBuffers() confirmado + esp_deep_sleep  │    │
│  │        (10 ms) preservando estado em RTC_DATA_ATTR                 │    │
│  └──────────────────────────────────────────────────────────────────┘    │
│                                     │                                     │
│                         flushBuffers() (4 buffers circulares em RAM)      │
│                                     │                                     │
│         ┌───────────┬──────────────┼──────────────┬───────────┐          │
│     log.txt      wifi.txt       ble.txt         bt.txt         │          │
│                              (SD Card, via SdFat)                         │
└───────────────────────────────────────────────────────────────────────────┘
```

O `loop()` roda em ciclos de **~1 segundo**: lê o GPS via UART e amostra o MPU6050 a cada 100 ms, em todas as fases. Quando o GPS produz um fix válido, os dados são formatados, exibidos no Serial Monitor e acumulados no buffer de log. Apenas o rádio (WiFi/BLE/BT) muda de acordo com a fase atual — a amostragem de sensores e a gravação no SD nunca param.

---

## Hardware

| Componente | Interface | Pinos ESP32 |
|-----------|-----------|-------------|
| Módulo GPS (ex: NEO-6M) | UART2 | RX=GPIO17, TX=GPIO16 |
| DHT22 | 1-Wire digital | GPIO32 |
| MPU6050 (IMU 6 eixos) | I2C | SDA=GPIO21, SCL=GPIO22 (padrão ESP32) |
| Cartão SD (via SPI, SdFat) | SPI | CS=GPIO5, clock configurável (`SPI_CLOCK`) |

> **Alimentação:** todos os sensores a 3.3 V. O módulo GPS pode exigir 5 V dependendo do modelo — verificar datasheet.

> **Build:** Arduino IDE → Tools → Partition Scheme → **"No OTA (Large APP)"**. Necessário porque WiFi + BluetoothSerial + NimBLE juntos estouram a flash da partição padrão de 4 MB.

---

## Bibliotecas Utilizadas

| Biblioteca | Função | Instalação |
|-----------|--------|------------|
| `TinyGPS` | Parser de sentenças NMEA do GPS | Arduino Library Manager |
| `SdFat` (Bill Greiman) | Sistema de arquivos e acesso ao cartão SD (substitui `FS`/`SD` do core) | Arduino Library Manager → "SdFat" |
| `DHT` (Adafruit) | Leitura de temperatura e umidade do DHT22 | Arduino Library Manager → "DHT sensor library" |
| `WiFi` | Scan de redes WiFi (modo estação) | Embutida no ESP32 Arduino Core |
| `Adafruit_MPU6050` | Leitura de aceleração e giroscópio | Arduino Library Manager → "Adafruit MPU6050" |
| `Adafruit_Sensor` | Interface unificada Adafruit (dependência) | Instalada automaticamente com MPU6050 |
| `Wire` | Comunicação I2C com o MPU6050 | Embutida no Arduino Core |
| `esp_task_wdt` | Watchdog de tarefa (reset automático em travamento) | Embutida no ESP-IDF/core |
| `NimBLEDevice` (NimBLE-Arduino) | Scan ativo de dispositivos BLE | Arduino Library Manager → "NimBLE-Arduino" |
| `BluetoothSerial` | Inquiry de dispositivos Bluetooth Clássico | Embutida no ESP32 Arduino Core |

---

## Configuração (Defines e Constantes)

```cpp
// GPS
#define GPS_RX          17      // Pino RX da UART2 (recebe TX do GPS)
#define GPS_TX          16      // Pino TX da UART2 (envia RX do GPS)
#define GPS_Serial_Baud 9600    // Baud rate padrão dos módulos GPS NMEA

// DHT22
#define DHTPIN  32
#define DHTTYPE DHT22

// SD (SdFat)
const uint8_t SD_CS_PIN = 5;
#define SPI_CLOCK SD_SCK_MHZ(16)   // reduzir p/ SD_SCK_MHZ(10)/(4) se houver falha de leitura/escrita

// Buffers circulares (por linha): se o SD ficar indisponível e o buffer
// encher, as linhas mais antigas são descartadas para abrir espaço às novas
#define LOG_BUFFER_MAX    150    // linhas de log GPS
#define WIFI_BUFFER_MAX   100    // linhas de log WiFi
#define BLE_BUFFER_MAX    100    // linhas de log BLE
#define BT_BUFFER_MAX     100    // linhas de log BT Clássico

// Caches de deduplicação (guardam hash FNV-1a de 32 bits, não a string/MAC)
#define SSID_CACHE_MAX    500
#define BLE_CACHE_MAX     200
#define BT_CACHE_MAX      200

// Watchdog: se loop() não "alimentar" o watchdog nesse tempo, o ESP32 reseta
#define WDT_TIMEOUT_S 15

// Falhas seguidas de remount do SD antes de reiniciar o ESP32 inteiro
#define SD_REMOUNT_MAX_FALHAS 10

// Sono do WiFi: parado e sem SSID novo -> desliga o rádio por WIFI_SLEEP_MS.
// Também é o intervalo da fase WiFi antes de ceder o rádio pro BLE.
#define WIFI_SLEEP_KMH_THRESHOLD 2.0
#define WIFI_SLEEP_MS            (5UL * 60UL * 1000UL)   // ~5 min

// RF Phase Sequencer
#define PHASE_WIFI  0
#define PHASE_BLE   1
#define PHASE_BT    2
#define PHASE_BLE_MS   30000UL             // 30 s
#define PHASE_BT_MS    15000UL             // 15 s
#define BT_INQUIRY_MS  (PHASE_BT_MS - 3000UL)  // margem de 3s pra flush/encerrar

// Arquivos de log no SD
const char* logFileName  = "/log.txt";   // GPS + DHT22 + MPU6050
const char* wifiFileName = "/wifi.txt";  // Redes WiFi
const char* bleFileName  = "/ble.txt";   // Dispositivos BLE
const char* btFileName   = "/bt.txt";    // Dispositivos BT Clássico
```

---

## Objetos e Estado Globais

| Objeto/Variável | Tipo | Descrição |
|--------|------|-----------|
| `gps` | `TinyGPS` | Parser dos dados NMEA recebidos via Serial2 |
| `dht` | `DHT` | Sensor de temperatura e umidade |
| `mpu` | `Adafruit_MPU6050` | Sensor IMU de aceleração e giroscópio |
| `sd` | `SdFs` | Volume SdFat (auto-detecta FAT16/FAT32/exFAT), substitui o `SD` do core |
| `SerialBT` | `BluetoothSerial` | Usado só na fase BT, pra inquiry clássico |
| `mpuDisponivel` | `bool` | `true` se o MPU6050 foi inicializado com sucesso |
| `sdMutex` | `SemaphoreHandle_t` | Protege `sd` contra escrita concorrente entre `loop()` (Core 1) e as orchestrator tasks de BLE/BT (Core 0) |
| `currentPhase` / `phaseCount` | `RTC_DATA_ATTR uint8_t/uint32_t` | Fase atual do RF Phase Sequencer e contador de ciclos — sobrevivem a `esp_restart()`/deep sleep, só zeram em power-off real |
| `ssidCacheHash` / `bleCacheHash` / `btCacheHash` | `RTC_DATA_ATTR uint32_t[]` | Caches de hash FNV-1a das SSIDs/MACs já gravadas — nunca resetam enquanto a placa fica ligada |

---

## RF Phase Sequencer

O ESP32 nunca opera WiFi, Bluetooth Clássico e BLE ao mesmo tempo (compartilhamento de rádio + evitar picos de corrente simultâneos). Em vez disso, alterna entre 3 fases via `esp_restart()`-equivalente:

```
PHASE_WIFI (~5 min, = WIFI_SLEEP_MS)  ──►  PHASE_BLE (30 s)  ──►  PHASE_BT (15 s)  ──►  PHASE_WIFI  ──► ...
```

- **WiFi é a fase "padrão" de longa duração** — o log de GPS/DHT/MPU roda continuamente, `varrerWiFi()` faz scan normalmente.
- **BLE e BT são excursões curtas** disparadas ao fim de cada janela WiFi. O log de sensores continua rodando durante as excursões (tasks de scan pinadas no Core 0; sensores seguem no Core 1).
- **Transição de fase:** antes de trocar (`currentPhase = PHASE_X`) e chamar `triggerRestart()`, o código chama `flushBuffers()` em loop até confirmar sucesso — se o SD estiver indisponível, a fase atual é mantida e o flush é tentado de novo no próximo ciclo (**Flush-Antes-de-Restart com retry**), evitando perder linhas de log por causa do timing do restart.
- **`triggerRestart()`** não usa `esp_restart()` puro — usa `esp_deep_sleep_start()` com wakeup por timer de 10 ms. Mesmo efeito de limpar RAM/rádio, mas o bootloader garante a preservação do `RTC_DATA_ATTR` no wakeup (com `esp_restart()` puro isso não é garantido em todas as revisões de silício).
- `currentPhase`/`phaseCount` e os 3 caches de hash (`ssidCacheHash`/`bleCacheHash`/`btCacheHash`) vivem em `RTC_DATA_ATTR` justamente por causa desses restarts periódicos — em RAM comum eles zerariam a cada troca de fase e duplicariam SSIDs/MACs.

---

## Sistema de Buffers em RAM (Circulares)

Para reduzir o desgaste do cartão SD, os dados são acumulados em 4 buffers circulares (por linha, não por byte) alocados no **heap** (`malloc`, em `setup()`) antes de serem gravados em disco.

| Buffer | Linhas × tamanho | Arquivo destino |
|--------|-------------------|------------------|
| `logBuffer` | 150 × 160 bytes (~24 KB) | `log.txt` |
| `wifiBuffer` | 100 × 256 bytes (~25,6 KB) | `wifi.txt` |
| `bleBuffer` | 100 × 256 bytes (~25,6 KB) | `ble.txt` |
| `btBuffer` | 100 × 256 bytes (~25,6 KB) | `bt.txt` |

Cada buffer é **circular**: se encher (SD indisponível por tempo suficiente), a linha mais antiga é sobrescrita para abrir espaço à mais recente — nunca trava a gravação em RAM esperando o SD.

`flushBuffers()` grava os 4 buffers no SD numa única passada (1 open/close por arquivo, via `appendLinhasCirculares()`, sem montar cópia intermediária em RAM), só quando:
- nenhum rádio está em scan (`scanEmAndamento == false` — evita coincidir pico de corrente do rádio com o pico da escrita física, causa já confirmada de brownout em campo);
- consegue tomar `sdMutex` (evita corrida com as orchestrator tasks de BLE/BT).

Em caso de falha parcial de escrita, os dados não gravados permanecem no buffer circular, o SD é remontado (`remontarSD()`) e a tentativa é repetida no próximo ciclo — nada é descartado só por causa de uma falha transitória de escrita.

> **Nota:** em caso de perda abrupta de energia, os dados ainda no buffer (não gravados) serão perdidos.

---

## Deduplicação por Hash (SSID / BLE MAC / BT MAC)

Generalização das 3 caches de deduplicação: `hashString()` calcula FNV-1a de 32 bits de qualquer string; `hashJaVisto()`/`adicionarHashCache()` recebem array/contador/limite como parâmetros, reaproveitando a mesma lógica para SSID, MAC BLE e MAC BT (guardam 4 bytes/entrada em vez da string/MAC completa).

**Comportamento:**
- Cada cache vive em `RTC_DATA_ATTR` e **nunca reseta** enquanto a placa fica ligada (nem no flush, nem na troca de fase, nem quando o WiFi acorda do sono) — só zera em power-off real.
- Comparação por hash (equivalente a nome/MAC exato).
- Cache cheio (`SSID_CACHE_MAX`=500 / `BLE_CACHE_MAX`=200 / `BT_CACHE_MAX`=200) → aviso único no Serial, novas entradas passam sem dedup a partir daí.
- Deduplicação BLE também tem uma camada **transiente** por ciclo de scan interno (`seenInCycleBLE`, limpa a cada `BLE_SCAN_DURATION_MS`), separada da cache persistente em RTC.

---

## Estrutura de Dados

```cpp
struct DadosMPU {
  float acX, acY, acZ;  // Aceleração nos eixos X, Y, Z em m/s²
  float gyX, gyY, gyZ;  // Velocidade angular nos eixos X, Y, Z em rad/s
};

struct WifiStats {
  int total;   // Redes encontradas no scan
  int novas;   // Redes novas (gravadas)
  int dup;     // Redes duplicatas (ignoradas)
};

// Alocado no heap (malloc), transportado por FreeRTOS Queue até a consumer task
struct BLEDeviceRecord {
  char     address[18];
  char     name[65];
  bool     hasName;
  int8_t   rssi;
  int8_t   txPower;
  bool     hasTxPower;
  uint32_t timestamp;
};

// Alocado no heap (malloc), transportado por FreeRTOS Queue até a consumer task
struct BTDeviceRecord {
  char     address[18];
  char     name[65];
  bool     hasName;
  int8_t   rssi;
  bool     hasRSSI;
  uint32_t timestamp;
};
```

---

## Funções

### `appendFile(const char *path, const char *message) → bool`
Abre um arquivo no SD em modo **append** e grava a string `message` ao final. Retorna `true` só se abertura e escrita tiverem sucesso.

### `remontarSD() → bool`
Tenta remontar o cartão (`sd.end()` + `sd.begin()`), usado quando uma escrita falha. Após `SD_REMOUNT_MAX_FALHAS` falhas seguidas, reinicia o ESP32 inteiro.

### `appendLinhasCirculares(...) → int`
Grava as linhas de um buffer circular direto no arquivo (1 open/close), em ordem cronológica, sem montar cópia do buffer inteiro em RAM. Retorna quantas linhas foram efetivamente gravadas.

### `adicionarLinhaCircular(...)`
Adiciona uma linha a um buffer circular; se cheio, descarta a mais antiga.

### `flushBuffers() → bool`
Grava os 4 buffers no SD. Ver [Sistema de Buffers em RAM](#sistema-de-buffers-em-ram-circulares).

### `inicializarArquivoLog()`
Se `log.txt` ainda não existir, grava a linha de cabeçalho CSV. `wifi.txt`/`ble.txt`/`bt.txt` não têm cabeçalho (formato documentado abaixo).

### `hashString`, `hashJaVisto`, `adicionarHashCache`
Ver [Deduplicação por Hash](#deduplicação-por-hash-ssid--ble-mac--bt-mac).

### `obterTipoCriptografia(wifi_auth_mode_t) → const char*`
Converte o enum de segurança WiFi para string legível (`"open"`, `"WPA2"`, `"WPA3"`, etc.).

### `faseNome(uint8_t) → const char*`
Nome legível da fase atual (`"WIFI"`/`"BLE"`/`"BT"`), usado no Serial/dashboard.

### `triggerRestart()`
Ver [RF Phase Sequencer](#rf-phase-sequencer).

### `varrerWiFi(timeStamp, lat, lon, kmh) → WifiStats`
Scan assíncrono (não-bloqueante) de redes WiFi, só chamado durante `PHASE_WIFI`. Filtra duplicadas pela cache de hash, acumula no `wifiBuffer`. Também controla o **sono do WiFi**: parado (`kmh < WIFI_SLEEP_KMH_THRESHOLD`) e sem rede nova → `WiFi.mode(WIFI_OFF)` por `WIFI_SLEEP_MS`, acordando ao voltar a se mover ou ao esgotar o tempo.

### `exibirDashboard(...)`
Painel ASCII no Serial Monitor: coordenadas, velocidade, direção, DHT22, MPU6050, fase RF atual, estatísticas de WiFi (só na fase WiFi), estado do sono do WiFi, estado da rajada de `log.txt`, barra de progresso do buffer e contagem de todos os 4 buffers + 3 caches.

### BLE — `initBLEPhase()`, `bleConsumerTask()`, `BLEScanCallbacks::onResult/onScanEnd`, `bleOrchestratorTask()`
Scan ativo via NimBLE. `onResult` roda no contexto do BT stack, dedup transiente por ciclo (`seenInCycleBLE`) e envia `BLEDeviceRecord*` por fila (`bleQueue`, profundidade 10 — descarta sem bloquear se cheia) para `bleConsumerTask` (Core 1), que faz a dedup persistente (cache de hash) e acumula no `bleBuffer`. `bleOrchestratorTask` (Core 0) aguarda `PHASE_BLE_MS`, para o scan, faz flush confirmado e avança pra `PHASE_BT`.

### BT Clássico — `initBTPhase()`, `btDeviceFoundCB()`, `btConsumerTask()`, `btOrchestratorTask()`
Inquiry via `BluetoothSerial::discoverAsync()`. Mesma arquitetura fila+consumer da BLE (`btQueue`, `btConsumerTask` no Core 1). `btOrchestratorTask` (Core 0) conduz o inquiry por `BT_INQUIRY_MS`, encerra, faz flush confirmado e avança pra `PHASE_WIFI`.

### `processarDadosGPS(DadosMPU mediaMPU)`
Função principal de processamento, executada quando o GPS produz um fix válido, **em qualquer fase**.

**Passos internos:**
1. Lê posição, data/hora, satélites, HDOP, velocidade e direção do objeto `gps`
2. Lê temperatura e umidade do DHT22
3. Converte data/hora UTC para UTC-3 (Brasília) via `mktime()`
4. Atualiza `lastTimeStamp`/`lastLat`/`lastLon` — consumidos pelas consumer tasks de BLE/BT pra rotular achados de rádio com a posição do veículo
5. Monta a linha CSV (15 colunas) e acumula no `logBuffer`
6. Se `currentPhase == PHASE_WIFI`: chama `varrerWiFi()`
7. Exibe `exibirDashboard()`
8. Decide flush de `log.txt`: por contagem (`LOG_BUFFER_MAX`) em movimento, ou em rajada a cada `WIFI_SLEEP_MS` quando parado (ver README raiz, seção de gerenciamento de energia)

### `setup()`
1. Inicia `Serial` (115200) e `Serial2` (GPS, 9600)
2. Cria `sdMutex`
3. Aloca os 4 buffers circulares no heap (`malloc`) — trava com erro crítico se falhar
4. Inicia DHT22
5. Inicia MPU6050 (±16G / ±1000 deg/s / passa-baixa 44 Hz)
6. Monta o SD (retry infinito até montar — sem SD o log não serve pra nada)
7. Cria cabeçalho CSV, reporta existência dos 4 arquivos de log
8. Ativa o watchdog (`WDT_TIMEOUT_S`)
9. Lê `currentPhase` da RTC (fallback pra `PHASE_WIFI` se corrompida), incrementa `phaseCount`
10. Roteia pra fase correspondente: `PHASE_WIFI` só configura o rádio; `PHASE_BLE`/`PHASE_BT` chamam `initBLEPhase()`/`initBTPhase()`

### `loop()`
- Alimenta o watchdog a cada volta
- Se `PHASE_WIFI` esgotou `WIFI_SLEEP_MS`: desliga o WiFi, faz flush confirmado e avança pra `PHASE_BLE` (`triggerRestart()`)
- Lê os bytes do GPS por até 1s, amostrando o MPU6050 a cada 100ms
- Se fix válido: `processarDadosGPS(mediaMPU)`
- Sem fix: lê DHT/MPU e renderiza o dashboard com `---` nos campos de posição

---

## Formato dos Arquivos de Saída

Todos sem cabeçalho, exceto `log.txt`.

### `log.txt`
```
data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
12/07/2026 19:30:00, -234567890, -467890123, 8, 1.20, 45.30, NE, 72.5, 28.3, 1.23, -0.45, 9.81, 0.01, -0.02, 0.00
```
> `lat`/`lon` em milionésimos de grau (`÷ 1.000.000` = graus decimais). Sem MPU6050: campos `ac_*`/`gy_*` ficam vazios.

### `wifi.txt`
```
DD/MM/AAAA HH:MM:SS, lat, lon, SSID, RSSI, canal, criptografia
12/07/2026 19:30:01, -234567912, -467890145, MinhaRede, -65, 6, WPA2
```

### `ble.txt`
```
DD/MM/AAAA HH:MM:SS, lat, lon, MAC, nome, RSSI, tx_power
12/07/2026 19:31:10, -234567912, -467890145, AA:BB:CC:DD:EE:FF, MeuFone, -70, 4
```
> `nome` vazio se o dispositivo não anunciar. `tx_power` vazio se não anunciado (campo `hasTxPower == false`).

### `bt.txt`
```
DD/MM/AAAA HH:MM:SS, lat, lon, MAC, nome, RSSI
12/07/2026 19:31:55, -234567912, -467890145, 11:22:33:44:55:66, FoneCarro, -60
```
> `lat`/`lon`/`data_hora` de `ble.txt`/`bt.txt` são a **última posição conhecida do GPS** (`lastTimeStamp`/`lastLat`/`lastLon`), não a posição exata no instante da detecção — as consumer tasks de BLE/BT rodam fora do fluxo de `processarDadosGPS()`.

---

## Fuso Horário

O GPS fornece hora em **UTC**. O código subtrai 3 horas para **UTC-3 (Brasília)** usando `struct tm` e `mktime()`, que trata automaticamente a virada de meia-noite, mudança de dia, mês e ano.

Para outros fusos, alterar a linha:
```cpp
t.tm_hour -= 3;  // UTC-3 → alterar para o offset desejado
```

---

## Dependências de Instalação (Arduino IDE)

1. **TinyGPS** — Library Manager → buscar `TinyGPS`
2. **SdFat** (Bill Greiman) — Library Manager → buscar `SdFat`
3. **DHT sensor library** (Adafruit) — Library Manager → buscar `DHT sensor library`
4. **Adafruit MPU6050** — Library Manager → buscar `Adafruit MPU6050` *(instala `Adafruit Unified Sensor` automaticamente)*
5. **NimBLE-Arduino** — Library Manager → buscar `NimBLE-Arduino`
6. **ESP32 Arduino Core** — Boards Manager → `esp32` by Espressif *(inclui `WiFi`, `Wire`, `BluetoothSerial`, `esp_task_wdt`)*
7. **Partition Scheme:** Tools → Partition Scheme → **"No OTA (Large APP)"**

---

## Estrutura de Arquivos do Projeto

```
esp32gpsd/
├── esp32gpsd.ino      ← Código principal (GPS + DHT22 + MPU6050 + WiFi + BLE + BT)
└── README.md          ← Esta documentação

ble_scanner_poc/
└── ble_scanner_poc.ino  ← PoC original do RF Phase Sequencer (BLE ↔ BT ↔ WiFi),
                            fundido ao esp32gpsd.ino — ver CONTEXT.md ("Fusão RF")

esp32gpsd_dualcore/
└── esp32gpsd_dualcore.ino  ← Versão dual-core sem RF Phase Sequencer (só GPS/DHT/MPU/SD/WiFi
                                divididos em 2 tasks por núcleo) — ver README raiz e DUALCORE.MD
```

---

## Notas para Desenvolvimento Futuro

- **Análise dos dados:** os CSVs são compatíveis com Python (pandas), Excel e QGIS (plotar trajetos com lat/lon)
- **Calibração do MPU6050:** sem calibração de offset no código atual; considerar coletar amostras em repouso e subtrair o bias
- **WiFi scan:** consome tempo por execução; em velocidades altas pode impactar registros consecutivos do GPS
- **Tamanho das linhas dos buffers:** `logBuffer[160]`, `wifiBuffer`/`bleBuffer`/`btBuffer[256]` — suficiente para o formato atual; aumentar se campos extras forem adicionados
- **BLE/BT:** `BLEDeviceRecord`/`BTDeviceRecord` descartam de propósito manufacturer data, service UUIDs, appearance e Class of Device (sem consumidor hoje) — reavaliar se o CSV precisar desses campos

---
