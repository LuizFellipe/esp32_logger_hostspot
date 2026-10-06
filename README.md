# ESP32 GPS Logger com SD Card e DHT22

## Organização do projeto

| Caminho | Papel |
|---|---|
| [`esp32gpsd/`](esp32gpsd/README.md) | Firmware principal. |
| [`esp32gpsd_v2/`](esp32gpsd_v2/README.md) | Variante experimental ativa. |
| [`libraries/`](libraries/README.md) | Bibliotecas locais, versões e sincronização com Arduino IDE. |
| [`docs/`](docs/README.md) | Wiki, mapa e pinout. |
| [`archive/`](archive/README.md) | Experimentos e variantes históricas; GSM como submódulo. |
| `graphify-out/` | Grafo e relatório gerados, versionados. |
| `log/` | Logs locais, preservados e ignorados pelo Git. |

Antes de compilar uma nova versão, sincronize bibliotecas projeto → IDE conforme
[procedimento obrigatório](libraries/README.md). O contexto e as regras ficam em
[`CONTEXT.md`](CONTEXT.md). Para clonar também GSM, use
`git clone --recurse-submodules`; em um clone existente, execute
`git submodule update --init --recursive`.

Projeto de datalogger baseado em ESP32 que coleta dados de GPS, temperatura, umidade, aceleração/giroscópio (IMU), redes WiFi e dispositivos BLE próximos, gravando as informações em cartão SD. O logger atual coordena ciclos WiFi+BLE no modo Movimento/Parado. Experimentos com Bluetooth Clássico e RF Phase Sequencer permanecem no histórico em `archive/`.

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
| TX | GPIO 17 (GPS_RX) |
| RX | GPIO 16 (GPS_TX) |

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

### 6. Scanner Bluetooth Clássico — histórico

Presente nos experimentos de `archive/ble_scanner_poc/`; não integra v1 ou v2 atuais.
- **Endereço MAC**
- **Nome** (se anunciado)
- **RSSI**

## 📁 Arquivos de Saída

Os firmwares ativos gravam três arquivos no cartão SD: `log.txt`, `wifi.txt` e `ble.txt`.

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

> As linhas são acumuladas em um buffer em RAM (`LOG_BUFFER_MAX`) e gravadas no SD em lote, para reduzir desgaste do cartão. Com o veículo parado (ver [Gerenciamento de energia e vida útil do SD](#-gerenciamento-de-energia-e-vida-útil-do-sd)), o log entra no buffer a cada 30 s (10 s em movimento) e o flush é solicitado quando o buffer enche. Na v2, também há flush ao entrar no sono parado.

### wifi.txt
Contém os dados do scan de redes WiFi no formato:
```
DD/MM/AAAA HH:MM:SS, latitude, longitude, SSID, RSSI, canal, criptografia
```

**Exemplo:**
```
25/12/2024 18:30:45, -23550123, -46633456, MinhaRede, -65, 6, WPA2
```

> O scan de WiFi é assíncrono (não bloqueia o loop) e faz deduplicação de SSIDs por um cache único que **nunca reseta** (nem no flush, nem nas trocas de modo Movimento/Parado) — cada rede só é gravada 1 vez em wifi.txt até `SSID_CACHE_MAX` (500) encher ou o dispositivo perder energia. Com o veículo parado, o scan só roda 1 vez a cada 5 min (ver [Gerenciamento de energia e vida útil do SD](#-gerenciamento-de-energia-e-vida-útil-do-sd)).

### ble.txt
Contém os dispositivos BLE encontrados, no formato:
```
DD/MM/AAAA HH:MM:SS, latitude, longitude, MAC, nome, RSSI, tx_power
```

**Exemplo:**
```
25/12/2024 18:31:10, -23550145, -46633478, AA:BB:CC:DD:EE:FF, MeuFone, -70, 4
```

> `ble.txt` é gravado durante os ciclos de scan WiFi+BLE (ver seção abaixo). A posição (`latitude`/`longitude`/timestamp) é a **última posição conhecida do GPS**, não a posição exata do instante da detecção. Deduplicação por endereço MAC funciona igual à de SSID (cache de hash em `RTC_DATA_ATTR`, `BLE_CACHE_MAX` = 500, nunca reseta).

> **Nota:** O fuso horário está configurado para UTC-3 (Brasília).

## 📚 Bibliotecas Necessárias

As versões locais são a fonte principal. Sincronize-as com Arduino IDE seguindo
[`libraries/README.md`](libraries/README.md). As bibliotecas utilizadas são:

- **TinyGPS** - Para processamento dos dados do GPS
- **DHT sensor library** (Adafruit) - Para leitura do sensor DHT22
- **Adafruit MPU6050** - Para leitura do sensor IMU (opcional)
- **Adafruit Unified Sensor** - Dependência da lib acima
- **SdFat** (Bill Greiman) - Para acesso ao cartão SD
- **NimBLE-Arduino** - Para o scan ativo de dispositivos BLE

As bibliotecas `SPI`, `WiFi`, `Wire`, `esp_task_wdt` (watchdog) já fazem parte do framework ESP32.

> **Partition Scheme:** selecione **"No OTA (Large APP)"** em Tools → Partition Scheme na Arduino IDE. WiFi + NimBLE juntos estouram a flash da partição padrão de 4 MB.

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

#define SSID_CACHE_MAX    500    // Máx. SSIDs rastreadas para deduplicação (guarda hash, não string)
#define BLE_CACHE_MAX     500    // Máx. MACs BLE rastreados para deduplicação

#define WDT_TIMEOUT_S     60     // Timeout (s) do watchdog: reseta o ESP32 se o loop travar
#define SD_REMOUNT_MAX_FALHAS 10 // Falhas consecutivas de remount do SD antes de reiniciar o ESP32

#define PARKED_KMH_THRESHOLD 2.0                 // Abaixo/igual a esse km/h o veículo é considerado "parado"
#define MOVING_KMH_THRESHOLD 5.0                 // Exclusivo da v2: limiar de retorno a movimento
#define MOVING_DEBOUNCE_FIXES 5                   // Exclusivo da v2: fixes consecutivos
#define PARKED_SLEEP_MS (5UL*60UL*1000UL)        // Sono parado (sem scan) entre checks (5 min)
#define RADIO_SCAN_INTERVAL_MS 30000UL           // Cadência do ciclo WiFi+BLE em movimento (30s)
#define LOG_ADD_MOVING_MS 10000UL                // Cadência de log.txt no buffer em movimento
#define LOG_ADD_PARKED_MS 30000UL                // Cadência de log.txt no buffer parado
#define BLE_SCAN_DURATION_MS 5000                // Duração de cada scan BLE

const uint8_t SD_CS_PIN = 5;              // Pino CS do cartão SD
#define SPI_CLOCK SD_SCK_MHZ(16)          // Clock SPI do SD (reduza p/ SD_SCK_MHZ(10) ou (4) se houver falhas de leitura/escrita)
```

O MPU6050 usa o barramento I2C padrão do ESP32 (GPIO 21/22) e é inicializado automaticamente — se não for detectado, o programa continua normalmente sem os dados de IMU.

### Acesso ao cartão SD (SdFat)

O projeto usa a biblioteca **SdFat** (não a `SD.h` do core ESP32) para acesso ao cartão, via volume `SdFs` (auto-detecta FAT16/FAT32/exFAT). A instância global `sd` (tipo `SdFs`) substitui o antigo objeto `SD`. Configuração SPI em `SD_CONFIG` (CS + clock, acima).

Cada gravação (`appendLinhasCirculares()`, usada por log.txt/wifi.txt/ble.txt) abre o arquivo **1 única vez**, grava todas as linhas do buffer circular em sequência, e fecha — sem montar uma cópia intermediária do bloco inteiro em RAM (economiza `rows × lineLen` bytes por buffer). Os 3 buffers circulares são alocados no **heap** (`malloc`, em `setup()`), não como arrays estáticos. No boot, a verificação dos arquivos existentes usa só `sd.exists()` (sem abrir/ler o conteúdo nem contar linhas), pra minimizar ciclos de acesso ao cartão.

Na v1, `flushBuffers()` adquire `sdMutex`. Na v2, o SD e os buffers são manipulados pela `loopTask`; não há `sdMutex` nem tarefa consumidora BLE separada.

## 📡 Modo Movimento/Parado (WiFi + BLE)

WiFi e BLE permanecem inicializados no mesmo boot, sem restart periódico,
Bluetooth Clássico ou deep sleep. Os modos controlam a cadência dos scans.

| Comportamento | v1 — principal | v2 — experimental |
|---|---|---|
| Parada | ≤ 2 km/h; entra direto no sono | ≤ 2 km/h; conclui um ciclo final antes do sono |
| Retorno ao movimento | > 2 km/h, imediato | > 5 km/h por 5 atualizações consecutivas |
| Intervalo parado | 5 min entre checks | 5 min entre checks |
| Flush na entrada do sono | Não há gatilho específico | Fecha ciclo pendente e tenta gravar os três buffers |
| Consumo BLE | `bleConsumerTask`, Core 1 | `drenarFilaBLE()` dentro da `loopTask` |

Na v1, entrar no sono não fecha automaticamente um ciclo já aberto. A v2
inclui esse fechamento antes do flush. O avanço dos modos e ciclos é chamado
por `processarDadosGPS()`; o projeto de hotspot descreve uma evolução futura.
Sensores e dashboard mantêm ciclo aproximado de 1 s; o log entra no buffer
a cada 10 s em movimento e 30 s parado.

Detalhes em [v1](esp32gpsd/README.md) e [v2](esp32gpsd_v2/README.md).
A arquitetura antiga com RF Phase Sequencer fica em `archive/ble_scanner_poc/`.

## 🔋 Gerenciamento de energia e vida útil do SD

A combinação de scan WiFi + escrita no SD com o GPS ativo já causou brownout/travamento em campo (picos de corrente coincidindo). Mitigações via software:

### 1. Cadência reduzida quando parado
Com velocidade ≤ `PARKED_KMH_THRESHOLD` (2.0 km/h) o logger não escaneia por `PARKED_SLEEP_MS` (5 min) e faz um ciclo único ao acordar. Os rádios continuam ligados (a economia é de desgaste do SD e picos de corrente, não de energia).

### 2. Gate de escrita no SD durante scan
`flushBuffers()` nunca grava no SD enquanto há scan WiFi (`scanEmAndamento`) ou BLE (`bleScanAtivo`) em andamento — adia a escrita (o buffer circular tolera o atraso). Evita o pico de corrente do rádio coincidir com a escrita física. Na v2, entrar em `PARADO_SONO` fecha eventual ciclo pendente e tenta o flush; falhas mantêm dados no buffer para nova tentativa. A v1 não executa esse fechamento/flush na entrada do sono.

### Status no Monitor Serial
O dashboard ASCII (a cada ciclo, com ou sem fix de GPS) mostra o modo, estado do ciclo de rádio, buffers e caches de deduplicação.

## 🧠 Versão Dual-Core (esp32gpsd_dualcore)

Existe uma versão alternativa no diretório [esp32gpsd_dualcore](archive/esp32gpsd_dualcore/esp32gpsd_dualcore.ino), desenvolvida especificamente para tirar proveito da arquitetura de dois núcleos (Xtensa LX6) do ESP32 através do FreeRTOS. **Não inclui** BLE/BT nem o RF Phase Sequencer — só GPS/DHT22/MPU6050/WiFi/SD, divididos entre os dois núcleos:

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
- Existência dos arquivos de log (log.txt/wifi.txt/ble.txt), verificada só por `sd.exists()` no boot
- Dados lidos do GPS em tempo real
- Modo Movimento/Parado e estado do ciclo WiFi+BLE
- Tempo até o próximo scan, cadência do log, buffers, caches e contador de boot

## 🔧 Funcionalidades

- ✅ Leitura contínua de dados do GPS
- ✅ Ajuste automático de fuso horário (UTC-3)
- ✅ Leitura de temperatura e umidade
- ✅ Leitura de aceleração e giroscópio (MPU6050, opcional, com média por ciclo)
- ✅ Scan assíncrono e periódico de redes WiFi próximas, com deduplicação de SSID
- ✅ Scan ativo de dispositivos BLE (NimBLE) com deduplicação por MAC
- ✅ Modo Movimento/Parado: cadência de scan WiFi+BLE reduzida quando parado (`PARKED_KMH_THRESHOLD`/`PARKED_SLEEP_MS`); ciclo final, histerese e flush ao entrar no sono na v2
- ✅ Log no buffer a cada 10 s em movimento e 30 s parado; flush por capacidade, além do gatilho de sono na v2
- ✅ Gate: adia o flush durante scans WiFi ou BLE
- ✅ Três buffers circulares em RAM (heap-alocados) para log GPS/WiFi/BLE, com flush em lote no SD
- ✅ Gravação em arquivos separados (log.txt, wifi.txt, ble.txt), 1 open/close por lote (sem cópia intermediária em RAM)
- ✅ Consumo BLE por tarefa separada na v1; consumo BLE, buffers e escrita SD concentrados na `loopTask` na v2
- ✅ Cabeçalho CSV automático em log.txt
- ✅ Tratamento de erros para cartão SD e sensores ausentes
- ✅ Dashboard ASCII com modo, ciclo de rádio, cadência do log, buffers e caches
- ✅ Watchdog (esp_task_wdt): reseta o ESP32 automaticamente se o loop travar
- ✅ Reinício automático do ESP32 após falhas consecutivas de remount do SD (`SD_REMOUNT_MAX_FALHAS`)

## 📌 Observações

- O scan de WiFi é realizado a cada atualização válida dos dados do GPS, apenas enquanto há um ciclo WiFi+BLE ativo (ver [Modo Movimento/Parado](#-modo-movimentoparado-wifi--ble))
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
