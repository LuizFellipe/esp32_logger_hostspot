# ESP32 GPS Logger v3 — Documentação Completa

Rastreador baseado em ESP32 que coleta dados de **GPS**, **temperatura/umidade (DHT22)**, **aceleração/giroscópio (MPU6050)**, **redes WiFi** e **dispositivos BLE** próximos, gravando tudo em arquivos CSV no cartão SD (via SdFat) para análise posterior.

> **v3 (em teste):** `esp32gpsd_v3.ino` = v2 + hotspot de download dos logs quando parado (ver abaixo e [`serial.md`](serial.md)). Este documento descreve a v3 e identifica sua base v2.
>
> **Escopo:** hardware e software de `esp32gpsd_v3.ino`, versão `serial-2026-10-06-painel2`. Diagramas conferidos contra o firmware em 06/10/2026; não representam variantes históricas ou projeto GSM.

## Hotspot de download (v3)

Parado → 1 ciclo WiFi+BLE → flush → abre o AP. Sem atividade por 5 min → fecha o AP → sono de 5 min sem scan → novo check → reabre. Ao detectar retorno ao movimento pela histerese, o AP fecha e a tarefa HTTP aborta a transferência. **Limitação atual:** durante foco HTTP/download, o firmware descarta a UART GPS; portanto, não detecta movimento nesse período. A transição só pode ocorrer após retomar o processamento GPS.

| Item | Valor (editar no topo do `.ino`) |
|---|---|
| SSID / senha | `ESP32GPS-Logs` / `gpsdlogs2026` (`HOTSPOT_SSID`, `HOTSPOT_PASS`; WPA2: 8 a 63 caracteres) |
| Endereço | `http://192.168.4.1/`, canal 1, 1 estação por vez |
| Janela | `HOTSPOT_IDLE_MS` = 5 min; abrir/atualizar a página ou baixar renova (conectar ao WiFi, não) |
| Arquivos | `log.txt`, `wifi.txt`, `ble.txt`, enviados byte a byte como estão no SD |

- Durante o download, flush/remount do SD são adiados e os dados já bufferizados permanecem em RAM. O foco HTTP pausa aquisição: **não são adicionadas novas linhas GPS/sensores durante o download**. Fora do foco, 150 linhas a cada 30 s equivalem a cerca de 75 min de capacidade do buffer GPS parado.
- O download é abortado após 15 s sem progresso (`HTTP_STALL_MS`), por desconexão, erro de leitura SD ou AP fechado. Fechamento por movimento depende de nova leitura GPS; o foco HTTP impede essa detecção enquanto ativo.
- A tarefa `Hotspot_HTTP` (core 1, prioridade 2, stack de 8 KB) atende o servidor; o `sdMutex` serializa o SD entre ela e a `loopTask`.
- **Heap:** com WiFi AP + BLE o heap livre caía a ~13 KB e a página/downloads falhavam. Antes de abrir o AP, `entrarModoParadoHotspot()` esvazia a fila BLE e chama `bleParar()` (`NimBLEDevice::deinit`), subindo o heap livre a ~76 KB; `bleScanLigar()` reinicia o BLE no próximo ciclo.
- **Foco no hotspot:** com download ou acesso HTTP nos últimos 8 s (`HOTSPOT_FOCO_MS`), `loop()` pausa (sem GPS/MPU/SD/painel; só descarta a UART do GPS e alimenta o watchdog).
- A página `/` é enviada em blocos (chunked), sem montar um `String` grande.
- Os temporizadores de sono/hotspot e o fechamento do ciclo de scan rodam nas voltas normais do `loop()`, mesmo sem fix GPS. O retorno antecipado de foco HTTP não executa `servicoModo()`.
- Limite: arquivos acima de 4.294.967.295 bytes (4 GiB − 1 byte) são recusados (`Content-Length` de 32 bits); o log cresce cerca de 1,3 MB/dia.

![Ciclo do hotspot, página local e prioridade HTTP](docs/diagramas/11-hotspot.png)

### Download dos arquivos

`GET /download?file=...` aceita somente `log.txt`, `wifi.txt` e `ble.txt`. O servidor lê blocos de até 2048 bytes sob `sdMutex` e libera o mutex durante o envio TCP. O arquivo permanece aberto e `downloadAtivo` impede escrita/remount até o encerramento.

Nomes inválidos retornam 400; arquivo ausente, 404; SD ocupado/indisponível, falha de abertura, tamanho excessivo ou download ainda ocupado, 503. A confirmação exige todos os bytes enviados e fechamento do arquivo bem-sucedido. Desconexão, AP fechado, leitura SD incompleta ou 15 s sem progresso abortam; a conexão incompleta permite ao navegador detectar falha.

![Validação, streaming e término do download HTTP](docs/diagramas/12-download.png)

---

## Visão Geral

![Arquitetura completa do logger ESP32 v3](docs/diagramas/01-visao-geral.png)

O `loop()` trabalha em ciclos de aproximadamente 1 s, lê UART GPS, drena a fila
BLE sem bloquear e amostra MPU6050 a cada 100 ms. Dentro desse ciclo,
`vTaskDelay(1)` cede CPU. Posição/hora e buffers pertencem à `loopTask`;
não há `BLE_Consumer`. O `sdMutex` existe só para serializar o SD entre a `loopTask` (`flushBuffers()`) e a tarefa `Hotspot_HTTP`.

`atualizarModo()` (chamada por `receberGPS()` nas transições por velocidade válida de RMC) e `servicoModo()`
(temporizadores, poll do scan WiFi e fechamento do ciclo, nas voltas normais do
`loop()`) controlam modos e ciclos de rádio. A v3 mantém o ciclo final ao parar,
flush antes do hotspot/sono e retorno ao movimento com histerese, e implementa o
hotspot como `MODO_PARADO_HOTSPOT`.

---

### Concorrência e responsabilidade pelos dados

`loopTask` e `Hotspot_HTTP` rodam no core 1. A tarefa HTTP tem prioridade 2 e stack de 8192 bytes. A afinidade interna das pilhas WiFi/NimBLE é controlada pelo core/IDF, não pelo sketch. Callback BLE produz registros/flags; `loopTask` consome, formata CSV, altera buffers/caches e imprime Serial. HTTP lê SD e publica métricas.

`sdMutex` serializa operações de SD. A página usa snapshots `volatile`; contadores/estado HTTP usam `std::atomic`. Durante download, a flag sob mutex protege o arquivo aberto entre leituras.

![Ownership dos buffers, tarefas e exclusão mútua no SD](docs/diagramas/10-concorrencia.png)

---

## Hardware

| Componente | Interface | Pinos ESP32 |
|-----------|-----------|-------------|
| Módulo GPS (ex: NEO-6M) | UART2 | RX=GPIO17, TX=GPIO16 |
| DHT22 | Protocolo digital DHT (não Dallas/Maxim 1-Wire) | GPIO32 |
| MPU6050 (IMU 6 eixos) | I2C | SDA=GPIO21, SCL=GPIO22 (padrão ESP32) |
| Cartão SD (via SPI, SdFat) | SPI | CS=GPIO5, SCK=GPIO18, MOSI=GPIO23, MISO=GPIO19; 16 MHz |

> **Alimentação:** lógica do ESP32 em 3,3 V e GND comum. Tensão de alimentação depende do módulo GPS/DHT/IMU/SD utilizado; conferir especificações antes de ligar. O repositório não contém esquema elétrico completo, BOM, regulador ou valores de pull-up/desacoplamento. Diagrama representa interligações confirmadas, não um circuito pronto para fabricação.

I²C 21/22 e SPI 18/19/23 são padrões do ESP32 Dev Module documentado; o sketch define explicitamente CS5, UART17/16 e DHT32.

![Ligações físicas UART, DHT, I2C, SPI e alimentação](docs/diagramas/02-hardware.png)

> **Build:** Arduino IDE → Tools → Partition Scheme → **"No OTA (Large APP)"**. Necessário porque WiFi + NimBLE excedem a partição APP padrão (~1,2 MB) em uma placa com 4 MB de flash.
>
> **Arduino CLI:** na raiz `esp32/`, após sincronizar bibliotecas, `arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota esp32gpsd_v3` (no core 3.3.12 a partição aparece como "No OTA (2MB APP/2MB SPIFFS)"; v3 ocupa ~1,29 MB de 2 MB). Ambiente, gravação, monitor e problemas conhecidos em [`docs/wiki/compilacao-arduino-cli.md`](../docs/wiki/compilacao-arduino-cli.md).

---

## Bibliotecas Utilizadas

Versões locais e sincronização obrigatória projeto → Arduino IDE estão em
[`libraries/README.md`](../libraries/README.md). Execute o sincronizador antes
de compilar cada nova versão.

| Biblioteca | Função | Instalação |
|-----------|--------|------------|
| `TinyGPS` | Parser de sentenças NMEA do GPS | Arduino Library Manager |
| `SdFat` (Bill Greiman) | Sistema de arquivos e acesso ao cartão SD (substitui `FS`/`SD` do core) | Arduino Library Manager → "SdFat" |
| `DHT` (Adafruit) | Leitura de temperatura e umidade do DHT22 | Arduino Library Manager → "DHT sensor library" |
| `WiFi` | Scan de redes WiFi e hotspot local | Embutida no ESP32 Arduino Core |
| `WebServer` / `NetworkClient` | Página HTTP e download TCP | Embutidas no ESP32 Arduino Core |
| `SPI` / FreeRTOS | Bus SD, tarefas, fila e mutex | Embutidos no ESP32 Arduino Core |
| `Adafruit_MPU6050` | Leitura de aceleração e giroscópio | Arduino Library Manager → "Adafruit MPU6050" |
| `Adafruit_Sensor` | Interface unificada Adafruit (dependência) | Instalada automaticamente com MPU6050 |
| `Wire` | Comunicação I2C com o MPU6050 | Embutida no Arduino Core |
| `esp_task_wdt` | Watchdog de tarefa (reset automático em travamento) | Embutida no ESP-IDF/core |
| `NimBLEDevice` (NimBLE-Arduino) | Scan ativo de dispositivos BLE | Arduino Library Manager → "NimBLE-Arduino" |

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
#define WIFI_BUFFER_MAX   50     // linhas de log WiFi
#define BLE_BUFFER_MAX    50     // linhas de log BLE

// Caches de deduplicação (guardam hash FNV-1a de 32 bits, não a string/MAC)
#define SSID_CACHE_MAX    500
#define BLE_CACHE_MAX     500

// Watchdog: se loop() não "alimentar" o watchdog nesse tempo, o ESP32 reseta
#define WDT_TIMEOUT_S 60

// Falhas seguidas de remount do SD antes de reiniciar o ESP32 inteiro
#define SD_REMOUNT_MAX_FALHAS 10

// Estado Movimento/Parado
#define PARKED_KMH_THRESHOLD 2.0   // <= isso: considerado parado
#define MOVING_KMH_THRESHOLD 5.0   // > isso por MOVING_DEBOUNCE_FIXES fixes seguidos: volta a Movimento
#define MOVING_DEBOUNCE_FIXES 5    // fixes (~1 s cada) consecutivos acima do limiar
#define PARKED_SLEEP_MS       (5UL * 60UL * 1000UL)  // sono entre checks (parado)
#define RADIO_SCAN_INTERVAL_MS 30000UL  // cadência do ciclo WiFi+BLE em movimento

#define LOG_ADD_MOVING_MS  10000UL   // cadência de gravação no buffer de log em movimento
#define LOG_ADD_PARKED_MS  30000UL   // cadência de gravação no buffer de log parado

#define MODO_MOVIMENTO      0
#define MODO_PARADO_SONO    2
#define MODO_PARADO_CHECK   3
#define MODO_PARADO_HOTSPOT 4

// Arquivos de log no SD
const char* logFileName  = "/log.txt";   // GPS + DHT22 + MPU6050
const char* wifiFileName = "/wifi.txt";  // Redes WiFi
const char* bleFileName  = "/ble.txt";   // Dispositivos BLE
```

---

## Objetos e Estado Globais

| Objeto/Variável | Tipo | Descrição |
|--------|------|-----------|
| `gps` | `TinyGPS` | Parser dos dados NMEA recebidos via Serial2 |
| `dht` | `DHT` | Sensor de temperatura e umidade |
| `mpu` | `Adafruit_MPU6050` | Sensor IMU de aceleração e giroscópio |
| `sd` | `SdFs` | Volume SdFat (auto-detecta FAT16/FAT32/exFAT), substitui o `SD` do core |
| `mpuDisponivel` | `bool` | `true` se o MPU6050 foi inicializado com sucesso |
| `bleQueue` | `QueueHandle_t` | Fila de 10 ponteiros; callback NimBLE produz registros e `drenarFilaBLE()` os consome sem bloquear na `loopTask`. |
| `modoAtual` | `uint8_t` | Estado atual da máquina Movimento/Parado (`MODO_MOVIMENTO`/`MODO_PARADO_SONO`/`MODO_PARADO_CHECK`/`MODO_PARADO_HOTSPOT`) — vive em RAM comum, não sobrevive a restart (não há mais restart periódico) |
| `modoInicio` | `unsigned long` | millis() de quando entrou no modo/substado atual |
| `radioCicloAtivo` / `radioCicloWifiDone` / `radioCicloBleDone` / `radioCicloInicio` / `ultimoCicloFim` | `bool`/`bool`/`bool`/`unsigned long`/`unsigned long` | Controlam o ciclo de scan WiFi+BLE simultâneo (Movimento: repetido após `RADIO_SCAN_INTERVAL_MS` desde o fim anterior e com RMC válido; Parado/Check: disparado uma vez ao acordar) |
| `ssidCacheHash` / `bleCacheHash` | `RTC_DATA_ATTR uint32_t[]` | Caches FNV-1a dos IDs já observados, inseridos antes de gravar no SD. Não são limpas em flush/troca de modo; RTC busca retenção em resets, dependente do tipo de reset |
| `bootCount` | `RTC_DATA_ATTR uint32_t` | Contador de boot, incrementado em `setup()`. RTC busca retenção em resets; comportamento depende do tipo de reset. Usado para diagnosticar reset no meio de um ciclo (gravado em `log.txt` a cada boot) |

---

## Estado Movimento/Parado

WiFi e BLE escaneiam em ciclos coordenados. Na v3, **BLE é desinicializado antes do hotspot** para liberar heap; o próximo `bleScanLigar()` o inicializa novamente. Não há deep sleep ou reinício periódico. `PARADO_SONO` é uma espera lógica sem scan, com aquisição normal fora do foco HTTP.

`atualizarModo(kmh)` é chamado por `receberGPS()` para RMC com velocidade válida. `servicoModo()` trata temporizadores e fechamento de ciclo independentemente de novo fix, nas voltas normais do loop.

![Máquina de estados movimento, check, hotspot e sono](docs/diagramas/05-maquina-estados.png)

- **MOVIMENTO → CHECK:** velocidade ≤2 km/h. Aproveita ciclo aberto ou inicia ciclo WiFi+BLE final.
- **CHECK → HOTSPOT:** ambos os ramos de scan encerrados, inclusive falha sinalizada; tenta flush, drena fila e desinicializa BLE antes de abrir AP. Flush falho não impede AP; dados ficam retidos. Se HTTP não foi criado ou AP falha, vai ao SONO.
- **HOTSPOT → SONO:** 5 min sem atividade válida, sem download ativo; fecha AP e tenta flush.
- **SONO → CHECK:** após 5 min, inicia um único ciclo. Retoma hotspot ao terminar.
- **Qualquer parado → MOVIMENTO:** >5 km/h por 5 RMCs válidos consecutivos. Leitura ≤5 zera contador; valores entre 2 e 5 não provocam retorno. RMC inválido não decide modo.
- **Ciclos em movimento:** novo RMC válido pode iniciar ciclo quando passaram ≥30 s desde o fim do ciclo anterior. Não é período fixo entre inícios, e não inicia novo ciclo sem RMC válido.
- **Cadências normais:** janela GPS/IMU ≈1 s, MPU a cada 100 ms, DHT no mínimo a cada 2 s; GPS no buffer a cada 10 s em movimento ou 30 s parado. Foco HTTP pausa essas etapas.

### Coordenação dos scanners

`iniciarCicloRadio()` inicia BLE; `servicoModo()` dispara/polla WiFi assíncrono. WiFi inclui SSIDs ocultos com `scanNetworks(true, true)`; o segundo argumento não configura scan passivo. BLE faz scan ativo de 5 s. Duração WiFi depende do resultado real. Falha de início/término é registrada e conclui o respectivo ramo, evitando confundir falha com scan vazio.

![Início, dois ramos de scan e fechamento coordenado do ciclo](docs/diagramas/07-ciclo-radio.png)

---

## Sistema de Buffers em RAM (Circulares)

Para reduzir o desgaste do cartão SD, os dados são acumulados em 3 buffers circulares (por linha, não por byte) alocados no **heap** (`malloc`, em `setup()`) antes de serem gravados em disco.

| Buffer | Linhas × tamanho | Arquivo destino |
|--------|-------------------|------------------|
| `logBuffer` | 150 × 160 bytes (~24 KB) | `log.txt` |
| `wifiBuffer` | 50 × 256 bytes (~12,8 KB) | `wifi.txt` |
| `bleBuffer` | 50 × 256 bytes (~12,8 KB) | `ble.txt` |

Cada buffer é **circular**: se encher (SD indisponível por tempo suficiente), a linha mais antiga é sobrescrita para abrir espaço à mais recente — nunca trava a gravação em RAM esperando o SD.

A cadência com que `logBuffer` recebe novas linhas depende do modo: a cada `LOG_ADD_MOVING_MS` (10s) em movimento, a cada `LOG_ADD_PARKED_MS` (30s) em qualquer sub-estado parado — GPS/MPU continuam com janela de ≈1 s, DHT no mínimo a cada 2 s e resumo Serial a cada 5 s (30 s com AP), fora do foco HTTP; o *append* no buffer é limitado pela cadência.

`flushBuffers()` grava os 3 buffers no SD numa única passada (1 open/close por arquivo, via `appendLinhasCirculares()`, sem montar cópia intermediária em RAM), só quando:
- nenhum rádio está em scan (`scanEmAndamento == false` e `bleScanAtivo == false` — evita coincidir pico de corrente do rádio com o pico da escrita física, causa já confirmada de brownout em campo);
- não há download em andamento (`downloadAtivo == false`, checado antes e depois de adquirir o `sdMutex`);
- é executado pela `loopTask`, proprietária dos buffers; o `sdMutex` protege o volume contra a leitura da tarefa `Hotspot_HTTP`.

Um lote só sai do buffer após confirmar tamanho de cada escrita, `sync()` e `close()`. A confirmação é por arquivo: lotes bem-sucedidos são removidos mesmo se outro arquivo falhar; não há transação envolvendo os três logs. Qualquer falha retém o lote inteiro e informa a etapa; retry pode duplicar linhas já escritas. Flush adiado permanece pendente e retorna após scan/download; falhas SD tentam remount, com limite de 10 falhas consecutivas.

![Buffers circulares, gates de flush, confirmação por lote e recuperação SD](docs/diagramas/09-armazenamento.png)

> **Nota:** em caso de perda abrupta de energia, os dados ainda no buffer (não gravados) serão perdidos.

---

## Deduplicação por Hash (SSID / BLE MAC)

Generalização das 2 caches de deduplicação: `hashString()` calcula FNV-1a de 32 bits de qualquer string; `hashJaVisto()`/`adicionarHashCache()` recebem array/contador/limite como parâmetros, reaproveitando a mesma lógica para SSID e MAC BLE (guardam 4 bytes/entrada em vez da string/MAC completa).

**Comportamento:**
- Cada cache vive em `RTC_DATA_ATTR` e **não é limpa pelo firmware** no flush, na troca de modo ou no sono/check — não há limpeza explícita durante flush ou mudanças de modo. O atributo RTC não equivale a armazenamento permanente em flash.
- Comparação só pelo hash FNV-1a de 32 bits, sem comparar string original: colisões podem descartar identificadores distintos. WiFi deduplica SSID, não BSSID; redes diferentes com mesmo nome são agrupadas.
- Cache cheio (`SSID_CACHE_MAX`=500 / `BLE_CACHE_MAX`=500) → aviso único no Serial, IDs conhecidos continuam deduplicados; novos IDs são bufferizados, mas não entram no cache.
- O filtro nativo de duplicatas do NimBLE atua no scan (`setScanCallbacks(..., false)`). Não há reinício em `onScanEnd()`. A dedup por hash permanece em `drenarFilaBLE()`; `bleParar()` existe na v3 para liberar heap antes do AP.
- O hash é inserido antes da confirmação no SD. Se uma linha depois for perdida por sobrescrita/falha, a cache não garante sua reobservação.
- `RTC_DATA_ATTR` expressa a intenção de reter caches/boot em resets; retenção depende do tipo de reset e deve ser validada em placa. Power-off não preserva histórico RTC.

![Fila BLE, consumo, caches de hash e limitações da deduplicação](docs/diagramas/08-deduplicacao.png)

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

// Alocado no heap (malloc), transportado por FreeRTOS Queue até drenarFilaBLE()
struct BLEDeviceRecord {
  char     address[18];
  char     name[65];
  bool     hasName;
  int8_t   rssi;
  int8_t   txPower;
  bool     hasTxPower;
};
```

---

## Funções

### `appendFile(const char *path, const char *message) → bool`
Abre um arquivo no SD em modo **append** e grava a string `message` ao final. Retorna `true` somente com abertura, tamanho escrito, `sync()` e `close()` confirmados.

### `remontarSD() → bool`
Tenta remontar o cartão (`sd.end()` + `sd.begin()`), usado quando uma escrita falha. Após `SD_REMOUNT_MAX_FALHAS` falhas seguidas, reinicia o ESP32 inteiro.

### `appendLinhasCirculares(...) → int`
Grava as linhas de um buffer circular direto no arquivo (1 open/close), em ordem cronológica, sem montar cópia do buffer inteiro em RAM. Retorna `count` se o lote inteiro foi confirmado, ou zero em qualquer falha.

### `adicionarLinhaCircular(...)`
Adiciona uma linha a um buffer circular; se cheio, descarta a mais antiga.

### `flushBuffers() → bool`
Grava os 3 buffers no SD. Ver [Sistema de Buffers em RAM](#sistema-de-buffers-em-ram-circulares).

### `inicializarArquivoLog()`
Se `log.txt` ainda não existir, grava a linha de cabeçalho CSV. `wifi.txt`/`ble.txt` não têm cabeçalho (formato documentado abaixo).

### `hashString`, `hashJaVisto`, `adicionarHashCache`
Ver [Deduplicação por Hash](#deduplicação-por-hash-ssid--ble-mac).

### `obterTipoCriptografia(wifi_auth_mode_t) → const char*`
Converte o enum de segurança WiFi para string legível (`"open"`, `"WPA2"`, `"WPA3"`, etc.).

### `modoNome(uint8_t) → const char*`
Nome legível do modo atual (`"MOVIMENTO"`/`"PARADO (sono)"`/`"PARADO (check)"`/`"PARADO (hotspot)"`), usado no Serial/dashboard.

### `varrerWiFi(timeStamp, lat, lon) → WifiStats`
Scan assíncrono (não-bloqueante) de redes WiFi. Filtra duplicadas pela cache de hash, acumula no `wifiBuffer`. Só é chamado por `servicoModo()` enquanto há um ciclo de scan ativo e o WiFi ainda não terminou a parte dele (`radioCicloAtivo && !radioCicloWifiDone`), usando a última posição/hora conhecida — a função em si não sabe nada sobre modos, é scan+dedup+buffer puro.

### `exibirDashboard(...)`
Resumo compacto (3 a 4 linhas) a cada 5 s, ou 30 s com o hotspot aberto; mudo durante download. Mostra uptime, modo, velocidade RMC, duração do loop, posição, DHT, aceleração Z, ciclo RF (WiFi/BLE), ocupação dos 3 buffers, estado do SD, heap livre e maior bloco, perdas e, com hotspot aberto, SSID, clientes, prazo de fechamento e contadores web/download. Eventos imediatos trazem uptime, `[EVT]`/`[ERR]`/`[OK ]`, número e fase do loop; a linha final de download inclui motivo, esperas de TCP, heap e maior bloco. Apenas `loopTask` escreve Serial (incluindo `setup()`); HTTP e callbacks BLE publicam contadores/estado atômicos.

### BLE — `setupBLE()`, `bleScanLigar()`, `bleScanDesligar()`, `drenarFilaBLE()`
`setupBLE()` cria a fila e chama `bleIniciar()`; `bleParar()` desinicializa o controller antes de abrir o hotspot. `onResult()` aloca o registro
e tenta enviá-lo sem espera; fila cheia ou falha de alocação descarta o achado e incrementa `perdidos`, assim como sobrescrita dos buffers.
O scan dura 5 s e seu callback de término publica flags; `atualizarCicloRadio()` conclui a parte BLE na `loopTask`; falha de início é registrada. Não há reinício no callback. `drenarFilaBLE()` usa
`xQueueReceive(..., 0)` no `loop()`, deduplica MAC, formata CSV com a última
posição/hora GPS, acumula `bleBuffer` e libera cada registro. Nenhuma tarefa
consumidora separada é criada.

### Estado Movimento/Parado — `atualizarModo(kmh)`, `entrarModoMovimento()`, `entrarModoParadoSono()`, `entrarModoParadoCheck()`, `iniciarCicloRadio()`, `atualizarCicloRadio()`
Ver [Estado Movimento/Parado](#estado-movimentoparado). `atualizarModo()` é chamado a cada RMC com velocidade válida e decide, com base em `kmh` e na cadência de rádio (`ultimoCicloFim`), se deve trocar por velocidade ou iniciar um ciclo em movimento — as funções `entrarModo*()` fazem a transição (resetam o timer do novo estado). `iniciarCicloRadio()`/`atualizarCicloRadio()` controlam o ciclo de scan WiFi+BLE simultâneo, reusado tanto em Movimento (repetido) quanto no check parado (único).

### `processarDadosGPS(DadosMPU mediaMPU)`
Função principal de processamento, executada quando o GPS produz um fix válido, **em qualquer modo**.

Posição, data e hora são validadas antes de adicionar GPS ao CSV. `struct tm` inicializada, TZ UTC e deslocamento explícito UTC-3 normalizam viradas de dia. Campos inválidos ficam vazios. WiFi/BLE usam última posição conhecida; sem posição, coordenadas ficam vazias.

Somente RMC com velocidade válida renova leitura e dispara decisões pelos limiares 2/5 km/h. GGA não renova velocidade. Após 3 s sem velocidade válida, nenhuma decisão por velocidade ocorre; modo permanece. Temporizadores continuam funcionando.

Cadência CSV: 10 s em movimento, 30 s parado. DHT é lido separadamente, no mínimo a cada 2 s. O resumo é emitido após `servicoModo()`.

![Pipeline NMEA, velocidade RMC, sensores e validação CSV](docs/diagramas/06-aquisicao.png)

### `servicoModo()`
Chamado a cada volta normal do `loop()`, com ou sem fix (não executa no retorno antecipado de foco HTTP): poll de `varrerWiFi()` durante o ciclo, `atualizarCicloRadio()`, expiração do sono (→ `entrarModoParadoCheck()`) e inatividade do hotspot (→ `fecharHotspot()` + `entrarModoParadoSono()`).

### `setup()`

1. Inicia `Serial` a 115200; configura RX de `Serial2` em 1024 bytes antes de iniciar GPS a 9600.
2. Aloca os três buffers circulares no heap; falha impede inicialização.
3. Inicia DHT22 e MPU6050 opcional.
4. Monta SD com retry, cria cabeçalho e informa existência dos três arquivos.
5. No core Arduino 3.x, reconfigura TWDT com `esp_task_wdt_reconfigure()` para `WDT_TIMEOUT_S` (60 s); registra a tarefa atual.
6. Incrementa `bootCount` e grava marcador `# BOOT` com motivo do reset.
7. Inicializa fila/NimBLE (`setupBLE()`), entra em movimento e depois configura servidor/tarefa HTTP (`setupHotspot()`), com a pilha de rede já iniciada. Não inicia scan BLE imediatamente.

Falha na alocação de buffers/mutex ou criação da fila BLE impede o loop. Montagem inicial do SD tenta indefinidamente com intervalo de 500 ms. MPU ausente e falha da tarefa HTTP permitem continuidade com recursos reduzidos.

![Sequência real de setup e tratamento das falhas de boot](docs/diagramas/03-inicializacao.png)

### `loop()`

- Alimenta watchdog; recebe UART, chama `drenarFilaBLE()` e amostra MPU durante aproximadamente 1 s.
- `vTaskDelay(1)` cede CPU dentro do ciclo de aquisição.
- Lê DHT respeitando 2 s; com novos dados GPS, chama `processarDadosGPS(mediaMPU)`.
- Ao fim de cada volta normal, `servicoModo()` trata temporizadores e scans; imprime transições de download, retoma flush pendente e emite resumo (5 s; 30 s com hotspot; mudo em download), mesmo sem GPS. Em foco de hotspot, `loop()` apenas pausa (ver Hotspot).

![Ciclo de aquisição, serviço, retry e retorno antecipado HTTP](docs/diagramas/04-loop.png)

---

## Formato dos Arquivos de Saída

Todos sem cabeçalho, exceto `log.txt`. Este também contém marcadores de boot iniciados por `#`; ferramentas de análise devem ignorar essas linhas. Downloads mantêm conteúdo original.

![Contratos CSV, unidades e distribuição de RAM, RTC e flash](docs/diagramas/13-dados-csv.png)

### `log.txt`
```
data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht, ac_x, ac_y, ac_z, gy_x, gy_y, gy_z
12/07/2026 19:30:00, -23456789, -46789123, 8, 1.20, 45.30, NE, 72.5, 28.3, 1.23, -0.45, 9.81, 0.01, -0.02, 0.00
```
> `lat`/`lon` em milionésimos de grau (`÷ 1.000.000` = graus decimais). Sem MPU6050: campos `ac_*`/`gy_*` ficam vazios.

### `wifi.txt`
```
DD/MM/AAAA HH:MM:SS, lat, lon, SSID, RSSI, canal, criptografia
12/07/2026 19:30:01, -23456791, -46789145, MinhaRede, -65, 6, WPA2
```

### `ble.txt`
```
DD/MM/AAAA HH:MM:SS, lat, lon, MAC, nome, RSSI, tx_power
12/07/2026 19:31:10, -23456791, -46789145, AA:BB:CC:DD:EE:FF, MeuFone, -70, 4
```
> `nome` vazio se o dispositivo não anunciar. `tx_power` vazio se não anunciado (campo `hasTxPower == false`). `lat`/`lon`/`data_hora` de `ble.txt` são a **última posição conhecida do GPS** (`lastTimeStamp`/`lastLat`/`lastLon`) no consumo por `drenarFilaBLE()`, dentro da `loopTask`; não representam a posição exata no instante da detecção.

---

## Fuso Horário

O GPS fornece hora em **UTC**. O código subtrai 3 horas para **UTC-3 (Brasília)** usando `struct tm` e `mktime()`, que trata automaticamente a virada de meia-noite, mudança de dia, mês e ano.

Para outros fusos, alterar a linha:
```cpp
t.tm_hour = hora - 3;  // UTC-3 → alterar para o offset desejado
```

---

## Dependências de Instalação (Arduino IDE)

Antes de cada nova versão, execute a sincronização projeto → IDE e confira
as versões conforme [`libraries/README.md`](../libraries/README.md). O Library
Manager serve para obter os pacotes; as cópias locais são a fonte principal.

1. **TinyGPS** — Library Manager → buscar `TinyGPS`
2. **SdFat** (Bill Greiman) — Library Manager → buscar `SdFat`
3. **DHT sensor library** (Adafruit) — Library Manager → buscar `DHT sensor library`
4. **Adafruit MPU6050** — Library Manager → buscar `Adafruit MPU6050` *(instala `Adafruit Unified Sensor` automaticamente)*
5. **NimBLE-Arduino** — Library Manager → buscar `NimBLE-Arduino`
6. **ESP32 Arduino Core** — Boards Manager → `esp32` by Espressif *(inclui `WiFi`, `WebServer`, `NetworkClient`, `Wire`, `SPI`, FreeRTOS e `esp_task_wdt`)*
7. **Partition Scheme:** Tools → Partition Scheme → **"No OTA (Large APP)"**

![Bibliotecas, sincronização, compilação no_ota e gravação](docs/diagramas/14-build.png)

---

### Painel da página de download

`http://192.168.4.1/` mostra, acima da lista de arquivos, quatro cards: **WiFi** (redes no último scan), **BLE** (dispositivos do ciclo), **temperatura** e **umidade** (DHT22). É HTML5 + CSS embutido (`PAGINA_CSS`, na flash), sem JavaScript e sem recursos externos; os valores são gravados no HTML a cada carga e só mudam ao clicar em **Atualizar**. Segue o tema claro/escuro do aparelho. Detalhes de build em [`compilacao-arduino-cli.md`](../docs/wiki/compilacao-arduino-cli.md).

---

## Estrutura de Arquivos do Projeto

```
esp32gpsd_v3/
├── esp32gpsd_v3.ino          ← Firmware v3
├── README.md                ← Documentação com diagramas
├── serial.md                ← Console e critérios de validação
├── tests/test_serial.py     ← Regressões no host
└── docs/diagramas/
    ├── esp32gpsd-v3.drawio   ← Atlas editável: 15 páginas
    ├── 01-*.png … 15-*.png   ← Imagens com fonte DrawIO embutida
    ├── gerar_diagramas.py    ← Gerador do XML editável
    ├── paginas.tsv          ← Índice de exportação
    └── README.md            ← Fontes, legenda e reprodução
```

---

## Diagramas editáveis

[Baixar atlas DrawIO completo](docs/diagramas/esp32gpsd-v3.drawio) · [Fontes, legenda e instruções de exportação](docs/diagramas/README.md). Os 15 PNGs também contêm o diagrama individual embutido: abrir no DrawIO permite editar blocos e conexões.

## Notas para Desenvolvimento Futuro

- **Análise dos dados:** os CSVs são compatíveis com Python (pandas), Excel e QGIS (plotar trajetos com lat/lon)
- **Calibração do MPU6050:** sem calibração de offset no código atual; considerar coletar amostras em repouso e subtrair o bias
- **WiFi scan:** consome tempo por execução; em velocidades altas pode impactar registros consecutivos do GPS
- **Tamanho das linhas dos buffers:** `logBuffer[160]`, `wifiBuffer`/`bleBuffer[256]` — suficiente para o formato atual; aumentar se campos extras forem adicionados
- **BLE:** `BLEDeviceRecord` descarta de propósito manufacturer data, service UUIDs e appearance (sem consumidor hoje) — reavaliar se o CSV precisar desses campos

---

## Validação Serial v3

Build: ESP32 3.3.12, `esp32:esp32:esp32:PartitionScheme=no_ota`. Bibliotecas sincronizadas com `libraries/sync.py --apply` e verificadas com `--check`.

Na placa: hotspot, páginas e downloads dos 3 arquivos validados (heap ~76 KB com BLE desligado). Pendente: testar sem GPS, troca de modo, hotspot/download e remoção do SD. Lote incerto deve permanecer retido e nunca aparecer como confirmado.

Regressões no host (requer `g++`): `python3 esp32gpsd_v3/tests/test_serial.py`. Usa TinyGPS real e funções do sketch com SD simulado: checksum, GGA/idade da velocidade, RMC vazio, posição incompleta, CSV/UTC-3, escrita parcial, sync/close, cache cheio e sobrescrita. Não substitui testes em placa.

O painel mede aquisição da volta atual; `anterior` inclui envio do resumo Serial da volta anterior. Tempos são medidos, não prazos garantidos. Acessos são requisições HTTP (conectar ao AP não conta como acesso). Eventos agregam requisições entre voltas para evitar spam. Resultado do último download permanece visível depois de liberar o estado para nova transferência.

![Observabilidade, recuperação automática e validação em host e placa](docs/diagramas/15-diagnostico.png)
