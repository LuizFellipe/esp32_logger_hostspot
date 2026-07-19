# ESP32 GPS Logger com SD Card e DHT22

Projeto de datalogger baseado em ESP32 que coleta dados de GPS, temperatura, umidade, aceleração/giroscópio (IMU) e realiza scan de redes WiFi, dispositivos BLE e dispositivos Bluetooth Clássico próximos, gravando todas as informações em cartão SD. Os três rádios (WiFi/BLE/BT) nunca operam ao mesmo tempo — alternam em fases via o **RF Phase Sequencer** (ver seção dedicada abaixo).

## 📋 Componentes Utilizados

| Componente | Modelo |
|------------|--------|
| Microcontrolador | ESP32 |
| Módulo GPS | NEO-6M |
| Sensor de Temperatura e Umidade | DHT22 |
| Sensor IMU (Acelerômetro/Giroscópio) | MPU6050 (opcional) |
| Armazenamento | Módulo Cartão SD |

## 🔌 Conexões (Pinout)

### Módulo GPS NEO-6M
| Pino GPS | Pino ESP32 |
|----------|------------|
| VCC | 5V |
| GND | GND |
| TX | GPIO 16 (GPS_RX) |
| RX | GPIO 17 (GPS_TX) |

### Sensor DHT22
| Pino DHT22 | Pino ESP32 |
|------------|------------|
| VCC | 3.3V |
| GND | GND |
| DATA | GPIO 32 |

### Módulo Cartão SD
| Pino SD | Pino ESP32 |
|---------|------------|
| VCC | 5V |
| GND | GND |
| CS | GPIO 5 (padrão SPI) |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| SCK | GPIO 18 |

### Sensor MPU6050 (opcional)
| Pino MPU6050 | Pino ESP32 |
|--------------|------------|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 (I2C padrão) |
| SCL | GPIO 22 (I2C padrão) |

> Se o MPU6050 não for detectado no boot, o projeto continua funcionando normalmente — as colunas de IMU no log ficam vazias.

## 📊 Dados Coletados

### 1. Sensor DHT22
- **Umidade** (%RH)
- **Temperatura** (°C)

### 2. Módulo GPS NEO-6M
- **Latitude** (milionésimos de grau)
- **Longitude** (milionésimos de grau)
- **Velocidade** (km/h)
- **Direção** (pontos cardeais: N, S, E, W, NE, SE, etc.)
- **Quantidade de Satélites**
- **Precisão (HDOP)**

### 3. Sensor MPU6050 (opcional)
- **Aceleração** X, Y, Z (m/s², faixa ±16G)
- **Giroscópio** X, Y, Z (rad/s, faixa ±1000°/s)
- Valores gravados são a **média** das amostras lidas (a cada 100ms) durante o ciclo de 1s de leitura do GPS

### 4. Scanner WiFi
- **SSID** (nome da rede)
- **RSSI** (intensidade do sinal)
- **Canal**
- **Tipo de Criptografia** (Open, WEP, WPA, WPA2, WPA3, etc.)

### 5. Scanner BLE (Bluetooth Low Energy)
- **Endereço MAC**
- **Nome** (se anunciado)
- **RSSI**
- **TX Power** (se anunciado)

### 6. Scanner Bluetooth Clássico
- **Endereço MAC**
- **Nome** (se anunciado)
- **RSSI**

## 📁 Arquivos de Saída

Os dados são gravados no cartão SD em quatro arquivos:

### log.txt
Contém os dados do GPS, DHT22 e MPU6050 (formato CSV, com cabeçalho gravado automaticamente na primeira execução):
```
data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
```

**Exemplo (com MPU6050 disponível):**
```
25/12/2024 18:30:45, -23550123, -46633456, 8, 1.20, 15.50, S, 65.0, 28.5, 0.12, -0.05, 9.79, 0.01, 0.00, -0.02
```

**Exemplo (sem MPU6050):**
```
25/12/2024 18:30:45, -23550123, -46633456, 8, 1.20, 15.50, S, 65.0, 28.5, , , , , ,
```

> As linhas são acumuladas em um buffer em RAM (`LOG_BUFFER_MAX`) e gravadas no SD em lote, para reduzir desgaste do cartão. Com o veículo parado (ver [Gerenciamento de energia e vida útil do SD](#-gerenciamento-de-energia-e-vida-útil-do-sd)), a gravação vira rajada a cada 5 minutos em vez de a cada ~150 linhas.

### wifi.txt
Contém os dados do scan de redes WiFi no formato:
```
DD/MM/AAAA HH:MM:SS, latitude, longitude, SSID, RSSI, canal, criptografia
```

**Exemplo:**
```
25/12/2024 18:30:45, -23550123, -46633456, MinhaRede, -65, 6, WPA2
```

> O scan de WiFi é assíncrono (não bloqueia o loop) e faz deduplicação de SSIDs por um cache único que **nunca reseta** (nem no flush, nem quando o WiFi acorda do sono, nem nas trocas de fase RF) — cada rede só é gravada 1 vez em wifi.txt até `SSID_CACHE_MAX` (500) encher ou o dispositivo perder energia. Com o veículo parado e sem redes novas, o rádio WiFi desliga sozinho (ver [Gerenciamento de energia e vida útil do SD](#-gerenciamento-de-energia-e-vida-útil-do-sd)) e o wifi.txt para de crescer até o WiFi acordar.

### ble.txt
Contém os dispositivos BLE encontrados, no formato:
```
DD/MM/AAAA HH:MM:SS, latitude, longitude, MAC, nome, RSSI, tx_power
```

**Exemplo:**
```
25/12/2024 18:31:10, -23550145, -46633478, AA:BB:CC:DD:EE:FF, MeuFone, -70, 4
```

### bt.txt
Contém os dispositivos Bluetooth Clássico encontrados, no formato:
```
DD/MM/AAAA HH:MM:SS, latitude, longitude, MAC, nome, RSSI
```

**Exemplo:**
```
25/12/2024 18:31:55, -23550145, -46633478, 11:22:33:44:55:66, FoneCarro, -60
```

> `ble.txt`/`bt.txt` só são gravados durante as respectivas fases do RF Phase Sequencer (ver seção abaixo). A posição (`latitude`/`longitude`/timestamp) é a **última posição conhecida do GPS**, não a posição exata do instante da detecção. Deduplicação por endereço MAC funciona igual à de SSID (cache de hash em `RTC_DATA_ATTR`, `BLE_CACHE_MAX`/`BT_CACHE_MAX` = 200, nunca reseta).

> **Nota:** O fuso horário está configurado para UTC-3 (Brasília).

## 📚 Bibliotecas Necessárias

Instale as seguintes bibliotecas no Arduino IDE:

- **TinyGPS** - Para processamento dos dados do GPS
- **DHT sensor library** (Adafruit) - Para leitura do sensor DHT22
- **Adafruit MPU6050** - Para leitura do sensor IMU (opcional)
- **Adafruit Unified Sensor** - Dependência da lib acima
- **SdFat** (Bill Greiman) - Para acesso ao cartão SD
- **NimBLE-Arduino** - Para o scan ativo de dispositivos BLE

As bibliotecas `SPI`, `WiFi`, `Wire`, `BluetoothSerial` (inquiry Bluetooth Clássico) e `esp_task_wdt` (watchdog) já fazem parte do framework ESP32.

> **Partition Scheme:** selecione **"No OTA (Large APP)"** em Tools → Partition Scheme na Arduino IDE. WiFi + BluetoothSerial + NimBLE juntos estouram a flash da partição padrão de 4 MB.

## ⚙️ Configuração

As principais configurações podem ser ajustadas no início do arquivo `esp32gpsd.ino`:

```cpp
#define GPS_RX 17          // Pino RX do GPS
#define GPS_TX 16          // Pino TX do GPS
#define GPS_Serial_Baud 9600

#define DHTPIN 32          // Pino do DHT22
#define DHTTYPE DHT22      // Tipo do sensor DHT

#define LOG_BUFFER_MAX    150    // Linhas de log GPS acumuladas em RAM antes de gravar no SD
#define WIFI_BUFFER_MAX   100    // Linhas de log WiFi acumuladas em RAM antes de gravar no SD
#define BLE_BUFFER_MAX    100    // Linhas de log BLE acumuladas em RAM antes de gravar no SD
#define BT_BUFFER_MAX     100    // Linhas de log BT Clássico acumuladas em RAM antes de gravar no SD

#define SSID_CACHE_MAX    500    // Máx. SSIDs rastreadas para deduplicação (guarda hash, não string)
#define BLE_CACHE_MAX     200    // Máx. MACs BLE rastreados para deduplicação
#define BT_CACHE_MAX      200    // Máx. MACs BT Clássico rastreados para deduplicação

#define WDT_TIMEOUT_S     15     // Timeout (s) do watchdog: reseta o ESP32 se o loop travar
#define SD_REMOUNT_MAX_FALHAS 10 // Falhas consecutivas de remount do SD antes de reiniciar o ESP32

#define WIFI_SLEEP_KMH_THRESHOLD 2.0   // Abaixo desse km/h o veículo é considerado "parado"
#define WIFI_SLEEP_MS   (5UL*60UL*1000UL) // Duração do sono do WiFi / intervalo de rajada do log parado / duração da fase WiFi (5 min)

#define PHASE_BLE_MS    30000UL           // Duração da fase BLE (30s)
#define PHASE_BT_MS     15000UL           // Duração da fase BT Clássico (15s)

const uint8_t SD_CS_PIN = 5;              // Pino CS do cartão SD
#define SPI_CLOCK SD_SCK_MHZ(16)          // Clock SPI do SD (reduza p/ SD_SCK_MHZ(10) ou (4) se houver falhas de leitura/escrita)
```

O MPU6050 usa o barramento I2C padrão do ESP32 (GPIO 21/22) e é inicializado automaticamente — se não for detectado, o programa continua normalmente sem os dados de IMU.

### Acesso ao cartão SD (SdFat)

O projeto usa a biblioteca **SdFat** (não a `SD.h` do core ESP32) para acesso ao cartão, via volume `SdFs` (auto-detecta FAT16/FAT32/exFAT). A instância global `sd` (tipo `SdFs`) substitui o antigo objeto `SD`. Configuração SPI em `SD_CONFIG` (CS + clock, acima).

Cada gravação (`appendLinhasCirculares()`, usada por log.txt/wifi.txt/ble.txt/bt.txt) abre o arquivo **1 única vez**, grava todas as linhas do buffer circular em sequência, e fecha — sem montar uma cópia intermediária do bloco inteiro em RAM (economiza `rows × lineLen` bytes por buffer). Os 4 buffers circulares são alocados no **heap** (`malloc`, em `setup()`), não como arrays estáticos. No boot, a verificação dos arquivos existentes usa só `sd.exists()` (sem abrir/ler o conteúdo nem contar linhas), pra minimizar ciclos de acesso ao cartão.

Como o objeto `sd` é compartilhado entre o `loop()` principal e as tasks orquestradoras de BLE/BT (que rodam no Core 0 durante suas respectivas fases), um `sdMutex` (FreeRTOS `SemaphoreHandle_t`) protege todo acesso a `sd` contra escrita concorrente.

## 📡 RF Phase Sequencer

WiFi, BLE e Bluetooth Clássico nunca ficam ativos ao mesmo tempo — dividir o rádio em fases evita picos de corrente simultâneos e conflitos de coexistência WiFi/BT no chip. O ESP32 alterna entre 3 fases, nessa ordem:

```
PHASE_WIFI (~5 min, = WIFI_SLEEP_MS)  ──►  PHASE_BLE (30s)  ──►  PHASE_BT (15s)  ──►  PHASE_WIFI  ──► ...
```

- **WiFi é a fase "padrão" de longa duração** — o scan de WiFi e a leitura de GPS/DHT22/MPU6050 seguem normalmente, sem tarefa dedicada.
- **BLE e BT são excursões curtas** disparadas ao fim de cada janela WiFi. O log de sensores/GPS **nunca para**, mesmo durante as excursões BLE/BT (tasks de scan de rádio pinadas no Core 0; leitura de sensores segue no Core 1).
- Cada troca de fase é feita via `triggerRestart()`: antes de avançar, o firmware confirma que `flushBuffers()` teve sucesso (retry no ciclo seguinte se o SD estiver indisponível), evitando perder linhas de log por causa do timing do restart.
- `triggerRestart()` usa deep sleep de 10ms (não `esp_restart()` puro) para garantir a preservação do estado salvo em `RTC_DATA_ATTR` — que inclui a fase atual (`currentPhase`), o contador de ciclos (`phaseCount`) e os 3 caches de deduplicação (SSID/BLE/BT). Esse estado sobrevive aos restarts periódicos entre fases, mas é perdido em power-off real.

## 🔋 Gerenciamento de energia e vida útil do SD

A combinação de scan WiFi + escrita no SD com o GPS ativo já causou brownout/travamento em campo (picos de corrente coincidindo). Duas mitigações via software, ambas usando os mesmos parâmetros `WIFI_SLEEP_KMH_THRESHOLD` (2.0 km/h) e `WIFI_SLEEP_MS` (5 min):

### 1. Sono do WiFi (rádio desligado quando parado)
- Veículo parado (`kmh < WIFI_SLEEP_KMH_THRESHOLD`) **e** nenhuma rede nova encontrada na última varredura → `WiFi.mode(WIFI_OFF)`, rádio desligado.
- Acorda (`WiFi.mode(WIFI_STA)`) se o veículo voltar a se mover (`kmh >= WIFI_SLEEP_KMH_THRESHOLD`) **ou** se `WIFI_SLEEP_MS` (5 min) se esgotarem — o que ocorrer primeiro.
- O cache de SSIDs (`ssidCache`, `SSID_CACHE_MAX`=500) **não reseta ao acordar**: o scan pós-sono é comparado contra tudo que já foi visto antes de dormir. Se não achar rede nova, nada é gravado em wifi.txt e ele volta a dormir imediatamente — evita duplicar nome de rede já registrado em ciclos anteriores de sono.

### 2. Gate de escrita no SD durante scan WiFi
- `flushBuffers()` nunca grava no SD enquanto um `WiFi.scanNetworks()` assíncrono está em andamento (`scanEmAndamento`) — adia a escrita pro próximo ciclo (o buffer circular tolera o atraso). Evita o pico de corrente do rádio coincidir com o pico de escrita física no SD, tanto parado quanto em movimento.

### 3. Rajada de log.txt quando parado
- Em movimento, log.txt grava normalmente por contagem (flush a cada `LOG_BUFFER_MAX` linhas, ~150 linhas ≈ 2,5 min a 1 leitura/s).
- **Ao parar** (`kmh < WIFI_SLEEP_KMH_THRESHOLD`), faz um flush imediato do buffer — grava os dados da viagem antes de entrar no modo esparso, para não perdê-los quando o buffer circular (~2,5 min) sobrescrever as linhas mais antigas enquanto espera a rajada.
- Já parado, a gravação por contagem é suspensa: o `logBuffer` continua acumulando 1 linha/ciclo (circular — preserva só o trecho mais recente), mas só é gravado no SD em rajada única a cada `WIFI_SLEEP_MS` (5 min), usando um timer próprio (`logParadoStart`), desacoplado do timer do sono do WiFi.
- **Ao voltar a mover**, faz flush do que restou em buffer da parada e retoma a cadência normal por contagem.
- Diferente do WiFi (que fica sem dado novo útil quando parado), o log sempre tem dados relevantes (temperatura/umidade/IMU), então continua sendo gravado — só com cadência mais espaçada.

### Status no Monitor Serial
O dashboard ASCII (a cada ciclo, com ou sem fix de GPS) mostra:
- **RF**: `WiFi:ON` ou `WiFi:OFF (dormindo)` com contagem regressiva até acordar.
- **LOG**: `Em movimento: gravacao normal (por buffer)` ou `Parado: gravacao em rajada em Xs` com contagem até a próxima rajada.

## 🧠 Versão Dual-Core (esp32gpsd_dualcore)

Existe uma versão avançada no diretório [esp32gpsd_dualcore](file:///home/luiz/Documents/esp32/esp32gpsd_dualcore/esp32gpsd_dualcore.ino) desenvolvida especificamente para tirar proveito da arquitetura de dois núcleos (Xtensa LX6) do ESP32 através do FreeRTOS:

*   **Divisão de Tarefas por Núcleo:**
    *   **Core 1 (APP_CPU):** Executa a tarefa de sensores (`tarefaSensores`). Responsável pela leitura de alta frequência do MPU6050, DHT22 e recepção serial contínua de sentenças do GPS. Livre de bloqueios físicos.
    *   **Core 0 (PRO_CPU):** Executa a tarefa de sistema (`tarefaSDWifi`). Responsável pela varredura WiFi e escrita física no cartão SD via SPI (barramentos lentos).
*   **Sincronização Segura e Otimizada:**
    *   Uso de Mutexes para proteção de dados compartilhados em RAM (`logMutex`, `wifiMutex`, `sensorMutex`, `serialMutex`).
    *   **Double-Buffering & Heap Allocation:** Para evitar que o Core 1 sofra atrasos na leitura serial/sensor devido à lentidão de gravação do SD, os buffers são copiados rapidamente em memória sob Mutex para buffers locais e descarregados fora do lock. Todos os buffers grandes são alocados dinamicamente no **Heap** no momento da inicialização para evitar estouro de memória estática da DRAM (`dram0_0_seg`).
*   **Watchdog (TWDT) Multitarefa:**
    *   Ambas as tarefas registram-se individualmente ao Watchdog do sistema (`esp_task_wdt_add`) garantindo reinício automático caso ocorra travamento de hardware em qualquer barramento (I2C, SPI ou serial).

## 🚀 Como Usar

1. **Monte o hardware** conforme o diagrama de conexões acima
2. **Formate o cartão SD** em FAT32
3. **Insira o cartão SD** no módulo
4. **Abra o projeto** no Arduino IDE
5. **Selecione a placa** ESP32 e a porta COM correta
6. **Faça o upload** do código
7. **Abra o Monitor Serial** (115200 baud) para acompanhar os dados

## 📝 Saída no Monitor Serial

O projeto exibe informações como:
- Status de inicialização do cartão SD
- Tipo e tamanho do cartão SD
- Existência dos arquivos de log (log.txt/wifi.txt/ble.txt/bt.txt), verificada só por `sd.exists()` no boot
- Dados lidos do GPS em tempo real
- Fase RF atual (WiFi/BLE/BT) e status do scan/inquiry correspondente
- Estado do sono do WiFi (ON/OFF + contagem regressiva) e da rajada de log.txt quando parado

## 🔧 Funcionalidades

- ✅ Leitura contínua de dados do GPS
- ✅ Ajuste automático de fuso horário (UTC-3)
- ✅ Leitura de temperatura e umidade
- ✅ Leitura de aceleração e giroscópio (MPU6050, opcional, com média por ciclo)
- ✅ Scan assíncrono e periódico de redes WiFi próximas, com deduplicação de SSID
- ✅ Scan ativo de dispositivos BLE (NimBLE) e inquiry de Bluetooth Clássico (BluetoothSerial), com deduplicação por MAC
- ✅ RF Phase Sequencer: WiFi/BLE/BT alternam em fases exclusivas, sem coexistir (ver [RF Phase Sequencer](#-rf-phase-sequencer))
- ✅ Sono automático do WiFi quando parado e sem redes novas (`WIFI_SLEEP_KMH_THRESHOLD`/`WIFI_SLEEP_MS`)
- ✅ Rajada de gravação do log.txt a cada 5 min quando parado (em vez de por contagem de linhas)
- ✅ Gate: nunca grava no SD com scan WiFi em andamento (evita coincidir picos de corrente)
- ✅ Buffers circulares em RAM (heap-alocados) para log GPS/WiFi/BLE/BT, com flush em lote no SD (reduz desgaste do cartão)
- ✅ Gravação em arquivos separados (log.txt, wifi.txt, ble.txt, bt.txt), 1 open/close por lote (sem cópia intermediária em RAM)
- ✅ `sdMutex` protegendo o cartão SD contra acesso concorrente entre `loop()` e as tasks de BLE/BT
- ✅ Cabeçalho CSV automático em log.txt
- ✅ Tratamento de erros para cartão SD e sensores ausentes
- ✅ Dashboard em ASCII no Monitor Serial (inclui fase RF atual, estado do sono do WiFi e da rajada de log)
- ✅ Watchdog (esp_task_wdt): reseta o ESP32 automaticamente se o loop travar
- ✅ Reinício automático do ESP32 após falhas consecutivas de remount do SD (`SD_REMOUNT_MAX_FALHAS`)

## 📌 Observações

- O scan de WiFi é realizado a cada atualização válida dos dados do GPS, exceto quando o WiFi está dormindo (veículo parado, ver [Gerenciamento de energia e vida útil do SD](#-gerenciamento-de-energia-e-vida-útil-do-sd)) ou fora da fase `PHASE_WIFI` (ver [RF Phase Sequencer](#-rf-phase-sequencer))
- O cartão SD deve estar formatado em FAT32
- Para melhor precisão do GPS, utilize o módulo em área aberta
- O sensor DHT22 tem tempo de leitura de ~2 segundos entre medições
- O MPU6050 é opcional: se não detectado no boot, as colunas de IMU ficam vazias no log

## ⚡ Melhorando o tempo de fix do GPS (NEO-6M)

Se o fix demora muito (ou nunca acontece) toda vez que o dispositivo é ligado, a causa mais comum em módulos NEO-6M genéricos é a **falta de bateria de backup (VBAT)**. Esses módulos costumam vir de fábrica com apenas um capacitor no pino VBAT, que não segura o RTC nem o almanaque/efemérides quando o módulo é desligado — resultando em **cold start** (pode levar de ~30s a alguns minutos) a cada boot.

**Solução recomendada:** soldar uma bateria de backup (CR1220 ou CR2032, 3V) no pino **VBAT** do módulo. Isso mantém o RTC e os dados de almanaque/efemérides entre desligamentos, permitindo **warm/hot start** (fix em poucos segundos) nas próximas vezes que o módulo for ligado, desde que a última posição/hora não esteja muito desatualizada.

Outras dicas:
- Use o módulo em área aberta, sem obstruções (prédios, teto de metal, etc.)
- Se não for possível adicionar bateria, mantenha o módulo GPS sempre energizado (não corte o VCC dele mesmo que o ESP32 entre em suspensão), evitando perder o almanaque
- Evite alterar a configuração do receptor via comandos binários UBX sem necessidade — algumas configurações (ex.: desativar sentenças NMEA, mudar modelo dinâmico) podem, dependendo do firmware do módulo, atrapalhar ou impedir o fix. O projeto usa a configuração de fábrica do NEO-6M por esse motivo.

## 📄 Licença

Este projeto é de código aberto e pode ser utilizado e modificado livremente.

---

**Desenvolvido para ESP32** 🇧🇷