# Contexto do Projeto: ESP32 GSM GPS Logger

Este documento define os termos e conceitos de domínio utilizados no projeto do registrador de dados (logger) GPS/WiFi com ESP32.

## Glossário de Termos

### GPS Logger
Dispositivo embarcado baseado em ESP32 que captura e registra coordenadas geográficas, dados de movimento e telemetria de sensores no cartão SD.

### APP_CPU (Core 1)
O processador de aplicação do ESP32. Utilizado para leitura de sensores de alta frequência (MPU6050, DHT22) e recepção de dados via UART (GPS), garantindo que não ocorra perda de pacotes ou atraso na amostragem.

### PRO_CPU (Core 0)
O processador de protocolo do ESP32. Executa a pilha de rádio (WiFi/BLE controller) e o callback de scan BLE. No `esp32gpsd.ino`, o `loop()` de sensores/SD e a `bleConsumerTask` rodam no Core 1.

### Circular Buffer (Buffer Circular)
Estrutura de dados em RAM (alocada no heap) que acumula temporariamente strings de log (GPS, WiFi e BLE — um buffer por arquivo de saída) em ordem cronológica. Se o buffer encher devido à indisponibilidade do cartão SD, os registros mais antigos são descartados para dar espaço aos mais novos.

### SSID Cache (Cache de SSID)
Conjunto de hashes FNV-1a de 32 bits que rastreia identificadores de rede (SSIDs) já gravados no log de WiFi, evitando duplicidades de escrita e economizando memória RAM e espaço em disco.

### BLE Scanner
Componente do sistema responsável por varrer ativamente o espectro Bluetooth Low Energy em busca de dispositivos anunciantes ao redor. Opera em modo ativo (envia SCAN_REQ para receber SCAN_RSP), capturando o conjunto completo de dados de advertising de cada dispositivo encontrado.

### BLE Device Record (Registro de Dispositivo BLE)
Estrutura de dados alocada dinamicamente (heap) que representa um dispositivo BLE detectado. No `esp32gpsd.ino` contém só os campos usados no CSV: endereço MAC, nome, RSSI, TX Power e timestamp (a PoC `ble_scanner_poc.ino` guarda também Manufacturer Data, Service UUIDs, Appearance etc.). Enviada por ponteiro via FreeRTOS Queue.

### BLE Scan Queue
FreeRTOS Queue de profundidade 10 que transporta ponteiros para BLE Device Records do callback do scanner para a `bleConsumerTask` (Core 1). O produtor descarta o record (free) se a queue estiver cheia, sem bloquear o BLE stack.

### BLE Deduplication (Deduplicação BLE)
Estratégia de controle de duplicatas por endereço MAC dentro de um ciclo de scan. Se um dispositivo já foi visto no ciclo atual, o RSSI é atualizado no record existente em vez de criar um novo. Garante que cada dispositivo apareça uma vez por ciclo com o RSSI mais recente.

### Modo Movimento/Parado (sem restart)
Substitui o antigo RF Phase Sequencer. WiFi e BLE ficam ligados no mesmo boot, **nunca são desligados** e escaneiam **juntos** em ciclos (`iniciarCicloRadio()`/`atualizarCicloRadio()`). Não há `esp_restart()` periódico, deep sleep nem Bluetooth Clássico. O modo vive em RAM comum (`modoAtual`) e é reavaliado a cada fix GPS por `atualizarModo(kmh)`, com limiar `PARKED_KMH_THRESHOLD` (2.0 km/h):
- **`MODO_MOVIMENTO`**: um ciclo WiFi+BLE a cada `RADIO_SCAN_INTERVAL_MS` (30 s); `log.txt` entra no buffer a cada `LOG_ADD_MOVING_MS` (10 s).
- **`MODO_PARADO_SONO`**: sem scan por `PARKED_SLEEP_MS` (5 min); `log.txt` a cada `LOG_ADD_PARKED_MS` (30 s). Ao parar, roda antes 1 ciclo WiFi+BLE final (via `MODO_PARADO_CHECK`); ao fechar, entra no sono e `flushBuffers()` grava tudo no SD (momento seguro pro cartão).
- **`MODO_PARADO_CHECK`**: ao fim dos 5 min, 1 ciclo único WiFi+BLE e volta ao sono. Voltar a `MODO_MOVIMENTO` exige > `MOVING_KMH_THRESHOLD` (5 km/h) por `MOVING_DEBOUNCE_FIXES` (5) fixes seguidos (histerese contra deriva do GPS parado).
- Build requer **Partition Scheme → "No OTA (Large APP)"** (WiFi + NimBLE juntos estouram a flash padrão de 4 MB).

### Histórico: RF Phase Sequencer / BT Clássico
Arquitetura antiga (WiFi → BLE → BT Clássico alternados via `esp_restart()`, estado em `RTC_DATA_ATTR`, `bt.txt`, deep sleep de 5 min) — já substituída no `esp32gpsd.ino`. Permanece apenas na PoC `archive/ble_scanner_poc/`.

### Cache Genérica de Hash (hashString / hashJaVisto / adicionarHashCache)
Generalização das funções de dedup antes específicas de SSID (`hashSSID`/`ssidJaVista`/`adicionarSSIDCache`). `hashString()` calcula FNV-1a 32-bit de qualquer string; `hashJaVisto()` e `adicionarHashCache()` recebem o array/contador/limite como parâmetros, permitindo reutilizar a mesma lógica para as duas caches (SSID, BLE MAC) em vez de três cópias quase idênticas.

### sdMutex
`SemaphoreHandle_t` que protege o objeto `SdFs sd` contra escrita concorrente em `flushBuffers()`.

### Flush no Sono (com retry)
`flushBuffers()` só grava com nenhum rádio ativo (`scanEmAndamento`/`bleScanAtivo` falsos) e adia caso contrário. É disparado quando `logBuffer` enche e ao entrar em `MODO_PARADO_SONO`. Se falhar (SD indisponível/rádio ativo), os dados ficam retidos no buffer circular e o SD é remontado (`remontarSD()`; após `SD_REMOUNT_MAX_FALHAS` falhas seguidas o ESP32 reinicia).

### ble.txt
Arquivo CSV de achados BLE, mesmo padrão do `wifi.txt` (sem cabeçalho, 1 linha por MAC único: timestamp, lat, lon, MAC, nome, RSSI, TX Power quando anunciado). Escrito via `bleBuffer`/`adicionarLinhaCircular`/`appendLinhasCirculares`, como `log.txt`/`wifi.txt`.

### bootCount
`RTC_DATA_ATTR` incrementado a cada `setup()`; gravado em `log.txt` como `# BOOT bootCount=N reset_reason=R`. Se mudar sem power-off real, houve reset espúrio (watchdog/brownout).

## Ordem obrigatória de navegação no código (hook)

Antes de procurar ou abrir arquivos de código, leia `graphify-out/GRAPH_REPORT.md`. Em seguida, use `graphify query`, `graphify path` e `graphify explain` para localizar o fluxo e os componentes relacionados. Abra somente os arquivos que o grafo apontar; se um sketch `.ino` não estiver representado, use os caminhos indicados no README indexado. O grafo gerado é versionado; depois de reorganizar caminhos ou alterar código, atualize-o com `graphify update .`.

## Organização e fontes de verdade

`esp32gpsd/` é o firmware principal; `esp32gpsd_v2/` é a variante experimental ativa.
Projetos antigos ficam em `archive/`; GSM continua como submódulo independente.
Documentação integrada fica em `docs/`, com wiki, mapa e pinout. Serena está ativo
para este projeto: preservar `.serena/project.yml` e memórias; caches, logs e config
local ficam fora do Git. Logs de execução e DB TokenSave permanecem apenas locais.

## Regra obrigatória de bibliotecas e compilação

Antes de compilar cada nova versão, sincronize as bibliotecas locais de `libraries/`
com o diretório de bibliotecas da Arduino IDE, neste ambiente
`/home/luiz/Arduino/libraries/`. Após a baseline inicial importada da IDE, **o projeto
é a fonte principal**: a direção normal é projeto → IDE. Atualizações de biblioteca
devem entrar primeiro no projeto, incluindo suas dependências declaradas e versões.

Execute `python3 libraries/sync.py --apply` e depois
`python3 libraries/sync.py --check` antes de compilar; use `--ide-dir` em outro
ambiente. A verificação compara o conteúdo de todas as bibliotecas gerenciadas;
divergência impede considerar a compilação validada. Não use `--libraries` para
mascarar diferença entre projeto e IDE: o build deve consumir as cópias sincronizadas
da IDE. Cópias substituídas na IDE são preservadas pelo sincronizador.

Bibliotecas incluídas no core ESP32 ficam no pacote Arduino, não em `libraries/`.
Versões, dependências, perfis de build e procedimento ficam em
[`libraries/README.md`](libraries/README.md) e `libraries/manifest.json`.
