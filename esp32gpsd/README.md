# ESP32 GPS Logger — Documentação Completa

Rastreador baseado em ESP32 que coleta dados de **GPS**, **temperatura/umidade (DHT22)**, **aceleração/giroscópio (MPU6050)** e **redes WiFi próximas**, gravando tudo em arquivos CSV no cartão SD para análise posterior.

---

## Visão Geral

```
┌─────────────────────────────────────────────────────────┐
║                     ESP32                               ║
║                                                         ║
║  GPS (NEO-6M, UBX setup) ─► TinyGPS ─► processarGPS()   ║
║  DHT22 (GPIO32) ───────────────────────────┘            ║
║  MPU6050 (I2C) ─► média ~10 amostras/ciclo              ║
║  WiFi scan (Não-bloqueante/Assíncrono)                  ║
║                          │                              ║
║                flushBuffers() (RAM buffer)              ║
║                          │                              ║
║               ┌──────────┴──────────┐                   ║
║           log.txt              wifi.txt                 ║
║         (SD Card)             (SD Card)                 ║
└─────────────────────────────────────────────────────────┘
```

O programa opera em ciclos de **~1 segundo**: durante esse ciclo lê o GPS via UART e amostra o MPU6050 a cada 100 ms. Quando o GPS produz um fix válido, todos os dados são formatados, exibidos no Serial Monitor e gravados no cartão SD.

---

## Hardware

| Componente | Interface | Pinos ESP32 |
|-----------|-----------|-------------|
| Módulo GPS (ex: NEO-6M) | UART2 | RX=GPIO17, TX=GPIO16 |
| DHT22 | 1-Wire digital | GPIO32 |
| MPU6050 (IMU 6 eixos) | I2C | SDA=GPIO21, SCL=GPIO22 (padrão ESP32) |
| Cartão SD (via SPI) | SPI | Pinos padrão da placa (CS varia por shield) |

> **Alimentação:** todos os sensores a 3.3 V. O módulo GPS pode exigir 5 V dependendo do modelo — verificar datasheet.

---

## Bibliotecas Utilizadas

| Biblioteca | Função | Instalação |
|-----------|--------|------------|
| `TinyGPS` | Parser de sentenças NMEA do GPS | Arduino Library Manager |
| `FS` / `SD` / `SPI` | Sistema de arquivos e acesso ao cartão SD | Embutida no ESP32 Arduino Core |
| `DHT` (Adafruit) | Leitura de temperatura e umidade do DHT22 | Arduino Library Manager → "DHT sensor library" |
| `WiFi` | Scan de redes WiFi (modo estação) | Embutida no ESP32 Arduino Core |
| `Adafruit_MPU6050` | Leitura de aceleração e giroscópio | Arduino Library Manager → "Adafruit MPU6050" |
| `Adafruit_Sensor` | Interface unificada Adafruit (dependência) | Instalada automaticamente com MPU6050 |
| `Wire` | Comunicação I2C com o MPU6050 | Embutida no Arduino Core |

---

## Configuração (Defines e Constantes)

```cpp
// GPS
#define GPS_RX          17      // Pino RX da UART2 (recebe TX do GPS)
#define GPS_TX          16      // Pino TX da UART2 (envia RX do GPS)
#define GPS_Serial_Baud 9600    // Baud rate padrão dos módulos GPS NMEA

// DHT22
#define DHTPIN  32              // Pino de dados do DHT22
#define DHTTYPE DHT22           // Modelo do sensor

// Buffer e deduplicação
#define LOG_BUFFER_MAX    10     // Linhas de log GPS acumuladas antes de flush
#define WIFI_BUFFER_SIZE  4096   // Bytes do buffer WiFi em RAM
#define SSID_CACHE_MAX    64     // Máx SSIDs rastreadas para deduplicação

// Arquivos de log no SD
const char* logFileName  = "/log.txt";   // Dados GPS + DHT22 + MPU6050
const char* wifiFileName = "/wifi.txt";  // Redes WiFi encontradas
```

---

## Objetos Globais

| Objeto | Tipo | Descrição |
|--------|------|-----------|
| `gps` | `TinyGPS` | Parser dos dados NMEA recebidos via Serial2 |
| `dht` | `DHT` | Sensor de temperatura e umidade |
| `mpu` | `Adafruit_MPU6050` | Sensor IMU de aceleração e giroscópio |
| `mpuDisponivel` | `bool` | Flag: `true` se MPU6050 foi inicializado com sucesso |

---

## Sistema de Buffers em RAM

Para reduzir o desgaste do cartão SD e melhorar performance, os dados são acumulados em RAM antes de serem gravados em disco.

| Buffer | Tamanho | Flush |
|--------|---------|-------|
| `logBuffer` | ~1.6KB (10 linhas × 160 bytes) | A cada 10 fixes GPS (~10s) |
| `wifiBuffer` | 4KB | Junto com o log buffer |
| `ssidCache` | ~2.1KB (64 SSIDs × 33 bytes) | Reset a cada flush |

**Uso total de RAM:** ~7.7KB de ~160KB disponíveis

**Redução de operações SD:** de ~11 open/close por segundo para **2 open/close a cada 10 segundos** (~98% de redução)

> **Nota:** em caso de perda abrupta de energia, os últimos ~10 segundos de dados no buffer serão perdidos.

---

## Deduplicação de WiFi (SSID Cache)

O sistema mantém um cache de SSIDs já vistas desde o último flush. Se uma rede WiFi já foi registrada nesse período, ela é ignorada silenciosamente.

**Comportamento:**
- Cache armazena até 64 SSIDs distintas
- Comparação por nome exato (case-sensitive)
- Reset automático a cada flush (novas redes podem ser gravadas no próximo ciclo)
- Cache cheio → novas SSIDs passam sem filtro

**Impacto:** em cenários estacionários (parado 30min perto de 10 redes), reduz `wifi.txt` de ~1800 linhas para ~10 linhas.

---

## Estrutura de Dados

```cpp
struct DadosMPU {
  float acX, acY, acZ;  // Aceleração nos eixos X, Y, Z em m/s²
  float gyX, gyY, gyZ;  // Velocidade angular nos eixos X, Y, Z em rad/s
};
```

Usada para passar a média de leituras do MPU6050 entre o `loop()` e `processarDadosGPS()`.

---

## Funções

### `contarLinhas(fs::FS &fs, const char *path) → int`
Conta o número de linhas (ocorrências de `\n`) em um arquivo do SD.
Usada no `setup()` para informar quantas entradas já existem em `log.txt`.

---

### `appendFile(fs::FS &fs, const char *path, const char *message)`
Abre um arquivo no SD em modo **append** e grava a string `message` ao final.
Exibe aviso no Serial se o arquivo não puder ser aberto.
Usada por todas as funções de gravação do projeto.

---

### `inicializarArquivoLog()`
Verifica se `log.txt` já existe no SD.
Se **não existir**, grava a linha de cabeçalho CSV:

```
data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
```

Garante que arquivos existentes de sessões anteriores **não sejam modificados**.

---

### `obterTipoCriptografia(wifi_auth_mode_t) → const char*`
Converte o enum de segurança WiFi (`WIFI_AUTH_*`) para string legível
(`"open"`, `"WPA2"`, `"WPA3"`, etc.).

---

### `varrerWiFi(const char* timeStamp, long lat, long lon) → WifiStats`
Executa um scan **assíncrono** (não-bloqueante) de redes WiFi para evitar que a CPU trave a leitura serial do GPS. 
- Filtra duplicadas usando o cache de SSIDs.
- Armazena as redes novas no `wifiBuffer` da RAM.
- Retorna as estatísticas do scan (`total`, `novas`, `dup`).

**Formato de cada linha em `wifi.txt`:**
```
DD/MM/AAAA HH:MM:SS, lat, lon, SSID, RSSI, canal, criptografia
```

---

### `exibirDashboard(const char* timeStamp, long lat, long lon, ...)`
Exibe um painel ASCII didático no Serial Monitor. Mostra coordenadas, velocidade, direção, leituras do DHT22, acelerômetro/giroscópio MPU6050, estatísticas de WiFi do ciclo e uma barra de progresso visual do buffer em RAM (`SD [####------] 4/10`).
- Se o GPS não tiver fix, os dados relativos à posição mostram `---`.

---

### `flushBuffers()`
Grava os dados em RAM para o cartão SD de uma só vez. 
- Exibe o aviso em destaque `>> GRAVANDO SD... NAO DESLIGAR! <<` e em seguida `>> GRAVACAO CONCLUIDA. SEGURO DESLIGAR. <<` para proteger a integridade física do arquivo contra desligamentos inesperados.

---

### `processarDadosGPS(DadosMPU mediaMPU)`
Função principal de processamento. Executada quando o GPS produz um fix válido.

**Passos internos:**
1. Lê posição (`lat`, `lon`), data/hora, satélites, HDOP, velocidade e direção do objeto `gps`
2. Lê temperatura e umidade do DHT22
3. Converte data/hora UTC para **UTC-3 (horário de Brasília)** usando `mktime()`
4. Monta a string CSV com todos os dados (15 colunas)
5. Adiciona os dados ao buffer de log na RAM (`logBuffer`)
6. Inicia/verifica o scan de WiFi assíncrono
7. Exibe o painel visual `exibirDashboard()`
8. Executa `flushBuffers()` se o buffer atingir o limite (`LOG_BUFFER_MAX`)

**Lógica do CSV com MPU:**
- Se `mpuDisponivel == true` → grava valores reais de aceleração e giroscópio
- Se `mpuDisponivel == false` → grava campos vazios (estrutura CSV mantida consistente)

**Formato de cada linha em `log.txt`:**
```
DD/MM/AAAA HH:MM:SS, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
```

| Campo | Unidade | Descrição |
|-------|---------|-----------|
| `data_hora` | — | Timestamp UTC-3 formatado |
| `lat` / `lon` | milionésimos de grau | Coordenadas brutas do TinyGPS (÷ 1.000.000 = graus decimais) |
| `sat` | — | Número de satélites em uso |
| `hdop` | — | Horizontal Dilution of Precision (precisão do fix) |
| `kmh` | km/h | Velocidade sobre o solo |
| `direcao` | — | Direção cardinal: N, NE, E, SE, S, SO, O, NO |
| `umidade` | % | Umidade relativa do ar (DHT22) |
| `temp_dht` | °C | Temperatura do ar (DHT22) |
| `ac_x/y/z` | m/s² | Aceleração nos 3 eixos (MPU6050) — média do ciclo |
| `gy_x/y/z` | rad/s | Velocidade angular nos 3 eixos (MPU6050) — média do ciclo |

---

### `setup()`
Inicialização executada uma vez ao ligar:

1. Inicia `Serial` (115200 baud) e `Serial2` (GPS, 9600 baud)
2. Chama `configurarGPS()` para otimizar o NEO-6M (muda modelo dinâmico para *Automotive* e desabilita sentenças inúteis como `GSV`, `GSA`, `VTG` e `GLL`)
3. Configura WiFi em modo estação (`WIFI_STA`)
4. Inicia o DHT22
5. Inicia o MPU6050 (Acelerômetro a ±16G, Giroscópio a ±1000 deg/s e passa-baixa a 44 Hz)
6. Monta o cartão SD, cria cabeçalho CSV e reporta estado de `log.txt` e `wifi.txt`

---

### `loop()`
Executado continuamente:

- Lê os bytes do GPS e alimenta `gps.encode()` durante o ciclo de 1 segundo
- A cada 100ms: amostra o MPU6050 de forma não-bloqueante
- Calcula a média do MPU6050 no ciclo
- Se o GPS tem fix válido (`newData = true`): executa `processarDadosGPS(mediaMPU)`
- Se o GPS **não** tem fix: lê os sensores (DHT, MPU) e renderiza o dashboard mostrando `---` para as informações do GPS.

---

## Fluxo Completo de Dados

```
GPS (NMEA via UART) ──► gps.encode() ──► newData = true
                                              │
                                    processarDadosGPS(mediaMPU)
                                              │
                     ┌────────────────────────┼────────────────────────┐
                     │                        │                        │
              gps.get_position()      dht.readHumidity()        mediaMPU
              gps.get_datetime()      dht.readTemperature()    (média MPU)
              satellites, hdop
              speed, course
                     │                        │                        │
                     └────────────────────────┴────────────────────────┘
                                              │
                                     snprintf(logData)
                                              │
                              ┌───────────────┴───────────────┐
                              │                               │
                        Serial.print()              appendFile(log.txt)
                                                              │
                                                      varrerWiFi()
                                                              │
                                                   appendFile(wifi.txt)
```

---

## Formato dos Arquivos de Saída

### `log.txt` (exemplo)
```
data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
12/07/2026 19:30:00, -234567890, -467890123, 8, 1.20, 45.30, NE, 72.5, 28.3, 1.23, -0.45, 9.81, 0.01, -0.02, 0.00
12/07/2026 19:30:01, -234567912, -467890145, 8, 1.18, 45.50, NE, 72.4, 28.3, 1.31, -0.43, 9.79, 0.02, -0.01, 0.00
```

> `lat` e `lon` em milionésimos de grau. Para converter: `lat_graus = lat / 1000000.0`

### `wifi.txt` (exemplo)
```
12/07/2026 19:30:01, -234567912, -467890145, MinhaRede, -65, 6, WPA2
12/07/2026 19:30:01, -234567912, -467890145, RedeVizinho, -82, 11, WPA2
```

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
2. **DHT sensor library** — Library Manager → buscar `DHT sensor library` (Adafruit)
3. **Adafruit MPU6050** — Library Manager → buscar `Adafruit MPU6050`
   *(instala automaticamente `Adafruit Unified Sensor` como dependência)*
4. **ESP32 Arduino Core** — Boards Manager → `esp32` by Espressif
   *(inclui `FS`, `SD`, `SPI`, `WiFi`, `Wire`)*

---

## Estrutura de Arquivos do Projeto

```
esp32gpsd/
├── esp32gpsd.ino      ← Código principal
└── README.md          ← Esta documentação

gy_readings/
└── gy_readings.ino    ← Sketch de referência/teste do MPU6050 (Adafruit demo)

tinygps_esp32_v1/
└── tinygps_esp32_v1.ino  ← Versão anterior do logger (GPS + DHT22, sem MPU)
```

---

## Notas para Desenvolvimento Futuro

- **Análise dos dados:** os arquivos CSV são compatíveis com Python (pandas), Excel e QGIS (para plotar trajetos com lat/lon)
- **Calibração do MPU6050:** o sensor não possui calibração de offset no código atual; para maior precisão considerar coletar amostras em repouso e subtrair o bias
- **WiFi scan:** consome ~500 ms por execução; em velocidades altas pode impactar registros consecutivos do GPS
- **Tamanho dos buffers:** `logData[256]` e `dadosWifi[256]` — suficiente para o formato atual; aumentar se campos extras forem adicionados

---


