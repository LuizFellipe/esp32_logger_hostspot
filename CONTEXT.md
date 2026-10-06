# Contexto do Projeto: ESP32 GSM GPS Logger

Este documento define os termos e conceitos de domínio utilizados no projeto do registrador de dados (logger) GPS/WiFi com ESP32.

## Glossário de Termos

### GPS Logger
Dispositivo embarcado baseado em ESP32 que captura e registra coordenadas geográficas, dados de movimento e telemetria de sensores no cartão SD.

### APP_CPU (Core 1)
Processador de aplicação do ESP32. Na v1, executa o loop de sensores/SD e `bleConsumerTask`; v2/v3 concentram aquisição, consumo BLE e buffers na `loopTask`. Na v3, `Hotspot_HTTP` também roda no core 1. Não há garantia absoluta de ausência de perdas/atrasos: foco HTTP descarta UART GPS e pausa aquisição enquanto ativo.

### PRO_CPU (Core 0)
O processador de protocolo do ESP32. Executa a pilha de rádio (WiFi/BLE controller) e o callback de scan BLE. No `esp32gpsd.ino`, o `loop()` de sensores/SD e a `bleConsumerTask` rodam no Core 1.

### Circular Buffer (Buffer Circular)
Estrutura de dados em RAM (alocada no heap) que acumula temporariamente strings de log (GPS, WiFi e BLE — um buffer por arquivo de saída) em ordem cronológica. Se o buffer encher devido à indisponibilidade do cartão SD, os registros mais antigos são descartados para dar espaço aos mais novos.

### SSID Cache (Cache de SSID)
Conjunto de hashes FNV-1a de 32 bits de SSIDs observados, usado para reduzir repetição de registros WiFi. Na v3, hash é inserido antes da confirmação no SD; não equivale a um índice dos registros efetivamente gravados. Cache comporta 500 hashes: conhecidos seguem filtrados, novos IDs não são inseridos quando cheia. Redes com mesmo SSID são agrupadas e colisões são possíveis. Não é limpa em flush/troca de modo; retenção RTC depende do tipo de reset, não sobrevive a power-off.

### BLE Scanner
Componente do sistema responsável por varrer ativamente o espectro Bluetooth Low Energy em busca de dispositivos anunciantes ao redor. Opera em modo ativo (envia SCAN_REQ para receber SCAN_RSP), capturando o conjunto completo de dados de advertising de cada dispositivo encontrado.

### BLE Device Record (Registro de Dispositivo BLE)
Estrutura alocada no heap para transportar achado BLE por ponteiro via FreeRTOS Queue. Na v1 (`esp32gpsd.ino`), contém MAC, nome, RSSI, TX Power e timestamp. Na v3, record contém apenas MAC, nome e flags de presença, RSSI e TX Power: posição/hora são associados por `drenarFilaBLE()` no consumo. A PoC histórica guarda campos adicionais como Manufacturer Data, Service UUIDs e Appearance.

### BLE Scan Queue
FreeRTOS Queue de profundidade 10, transportando ponteiros do callback NimBLE. V1 usa `bleConsumerTask` no core 1; v2/v3 usam `drenarFilaBLE()` na `loopTask`, sem tarefa consumidora separada. Produtor tenta envio sem bloquear; fila cheia descarta/libera record. Na v3, falha de alocação ou fila cheia incrementa `perdidos`; consumidor libera cada record após tratar o achado.

### BLE Deduplication (Deduplicação BLE)
Controle de duplicatas deve ser descrito por versão. Na v3, filtro nativo do NimBLE atua durante scan e `drenarFilaBLE()` usa cache FNV-1a de MAC com 500 hashes. Não há reinício de scan em callback nem atualização garantida de RSSI para obter o mais recente. Hash entra antes da confirmação SD; colisão ou descarte de linha pode impedir novo registro. Cache cheia deixa novos IDs sem cache, mas mantém filtro dos conhecidos. Retry SD pode duplicar linhas; não garantir escrita única por dispositivo.

### Modo Movimento/Parado (sem restart)
Substitui sequenciador RF histórico. Não há reinício periódico, deep sleep ou Bluetooth Clássico nos loggers v1/v2/v3. Estado vive em RAM comum; sono é lógico. Diferenças:

| Regra | V1 | V2 | V3 |
|---|---|---|---|
| Parada | ≤2 km/h, entra no sono | ≤2 km/h, ciclo final antes do sono | ≤2 km/h, CHECK final antes do hotspot |
| Retorno | >2 km/h | >5 km/h por 5 fixes | >5 km/h por 5 RMCs válidos |
| Caminho parado | SONO → CHECK → SONO | SONO → CHECK → SONO | CHECK → HOTSPOT → SONO → CHECK |
| BLE | Inicializado durante operação | Inicializado durante operação | Desinicializa antes do AP; reinicializa no próximo scan |

Na v3, `receberGPS()` chama `atualizarModo(kmh)` em RMC com velocidade válida. Leitura ≤5 km/h zera contador de retorno. GGA não renova velocidade; sem RMC válido, modo é mantido.

- **MOVIMENTO:** novo ciclo WiFi/BLE exige RMC válido e ≥`RADIO_SCAN_INTERVAL_MS` (30 s) desde fim anterior. Telemetria entra no buffer a cada 10 s, fora do foco HTTP.
- **PARADO_CHECK:** reaproveita ciclo aberto ao parar ou inicia ciclo final; ao acordar, inicia ciclo único. Após ambos os ramos encerrarem, tenta flush e abre hotspot.
- **PARADO_HOTSPOT:** AP local; 5 min sem atividade HTTP válida e sem download → fechar AP e entrar no sono. HTTP indisponível/AP falhou → sono.
- **PARADO_SONO:** 5 min sem scan; entrada solicita flush; fim do intervalo → CHECK. Telemetria parada tem cadência 30 s, fora do foco HTTP.
- `servicoModo()` atende timers/scans sem depender de novo fix, nas voltas normais. Foco HTTP retorna antes desse serviço e não detecta movimento.

Build: partição `no_ota` (No OTA / APP 2 MB); WiFi+NimBLE excedem APP padrão de cerca de 1,2 MB em placa com 4 MB de flash.

### Histórico: RF Phase Sequencer / BT Clássico
Arquitetura antiga (WiFi → BLE → BT Clássico alternados via `esp_restart()`, estado em `RTC_DATA_ATTR`, `bt.txt`, deep sleep de 5 min) — já substituída no `esp32gpsd.ino`. Permanece apenas na PoC `archive/ble_scanner_poc/`.

### Cache Genérica de Hash (hashString / hashJaVisto / adicionarHashCache)
Generalização das funções de dedup antes específicas de SSID (`hashSSID`/`ssidJaVista`/`adicionarSSIDCache`). `hashString()` calcula FNV-1a 32-bit de qualquer string; `hashJaVisto()` e `adicionarHashCache()` recebem o array/contador/limite como parâmetros, permitindo reutilizar a mesma lógica para as duas caches (SSID, BLE MAC) em vez de três cópias quase idênticas.

### sdMutex
`SemaphoreHandle_t` para operações SdFat. V1 usa mutex com consumidor BLE separado; v2 concentra SD na `loopTask`, sem mutex SD. V3 serializa escrita/remount da `loopTask` e leitura HTTP. `downloadAtivo`, definido sob mutex, impede flush/remount enquanto arquivo de download permanece aberto, mesmo quando mutex é liberado entre blocos.

### Flush no Sono (com retry)
V1 não tem gatilho específico de flush ao entrar no sono; v2 solicita flush nessa transição, após fechar ciclo. V3 solicita flush por capacidade de qualquer buffer e ao entrar em HOTSPOT/SONO. Sem scan WiFi/BLE, sem download e com SD sob mutex, grava lotes por arquivo. Confirmação completa libera somente aquele lote; falha mantém lote incerto e tenta remount. Retry pendente na v3 respeita gates e intervalo mínimo de 1 s. Após 10 remounts consecutivos falhos, reinicia ESP32. Sono lógico não preserva RAM contra perda de energia.

### ble.txt
Arquivo CSV de achados BLE, sem cabeçalho: timestamp, lat, lon, MAC, nome, RSSI e TX Power quando anunciado. Na v3, posição/hora refletem últimos dados GPS no consumo da fila. Dedup por hash reduz repetição, mas não garante linha única por MAC devido a cache cheia/colisões/retry. Escrito via `bleBuffer`/`adicionarLinhaCircular`/`appendLinhasCirculares`.

### bootCount
`RTC_DATA_ATTR` incrementado em `setup()` e gravado em `log.txt` como `# BOOT bootCount=N reset_reason=R`. RTC expressa intenção de retenção em resets; comportamento depende do tipo de reset e deve ser validado em placa. Power-off não preserva histórico RTC. Contador e reset_reason apoiam diagnóstico de reinícios inesperados.

### Hotspot de download (v3)

`MODO_PARADO_HOTSPOT` ocorre após o ciclo CHECK e tentativa de flush. A v3 drena fila BLE, desinicializa NimBLE e abre AP local para página e download dos três logs. A tarefa `Hotspot_HTTP` roda no core 1, prioridade 2, stack 8192 bytes. Página HTML/CSS é enviada em chunks e atualizada por recarga manual. Não há `/api`, upload ou backend. Inatividade HTTP de 5 min, sem download, fecha AP e inicia sono lógico de 5 min. Conectar ao WiFi sem acessar página/download não renova atividade.

Fonte: `esp32gpsd_v3/esp32gpsd_v3.ino` — `entrarModoParadoHotspot()` (linha 852), `servicoModo()` (882), `httpRaiz()` (1154), `setupHotspot()` (1333).

### Foco HTTP (v3)

AP aberto e download ativo ou acesso HTTP nos últimos 8 s ativam `hotspotEmFoco()`. O loop descarta UART GPS, alimenta watchdog, espera 50 ms e retorna antes de aquisição/sensores/serviço do modo/retry SD/painel. Não detecta movimento nesse período. Processamento GPS precisa retomar antes de fechar AP por velocidade.

Fonte: `esp32gpsd_v3/esp32gpsd_v3.ino` — `hotspotEmFoco()` (1460), `loop()` (1467).

### Confirmação SD por lote (v3)

Um lote é removido do buffer somente se abertura, tamanho escrito, `sync()` e `close()` forem confirmados. Confirmação ocorre por arquivo, sem transação dos três logs. Falha retém o lote incerto; retry pode duplicar linhas já gravadas parcialmente. Retry pendente roda com gates liberados e intervalo mínimo de 1 s. Dez remounts consecutivos falhos reiniciam ESP32.

Fonte: `esp32gpsd_v3/esp32gpsd_v3.ino` — `confirmarArquivo()` (313), `appendLinhasCirculares()` (360), `flushBuffers()` (399), retry no `loop()` (1536).

### Download ativo e estado atômico (v3)

`downloadAtivo` sob `sdMutex` protege arquivo aberto e impede flush/remount. HTTP lê até 2048 bytes por bloco e libera mutex durante envio TCP. Allowlist limita acesso a `log.txt`, `wifi.txt`, `ble.txt`. Arquivos >4.294.967.295 bytes são recusados. Desconexão, AP fechado, erro de leitura ou 15 s sem progresso abortam. Resultado atômico é impresso/liberado pela `loopTask`.

Fonte: `esp32gpsd_v3/esp32gpsd_v3.ino` — `httpDownload()` (1229), `motivoAborto()` (1219), `imprimirDownload()` (1079).


## Ordem obrigatória de navegação no código (hook)

Antes de procurar ou abrir arquivos de código, leia `graphify-out/GRAPH_REPORT.md`. Em seguida, use `graphify query`, `graphify path` e `graphify explain` para localizar o fluxo e os componentes relacionados. Abra somente os arquivos que o grafo apontar; se um sketch `.ino` não estiver representado, use os caminhos indicados no README indexado. O grafo gerado é versionado; depois de reorganizar caminhos ou alterar código, atualize-o com `graphify update .`.

## Organização e fontes de verdade

`esp32gpsd/` é o firmware principal; `esp32gpsd_v2/` é a base experimental sem HTTP; `esp32gpsd_v3/` implementa hotspot de download e permanece em teste. O [atlas v3](esp32gpsd_v3/docs/diagramas/README.md) reúne 15 PNGs com fonte embutida, DrawIO editável, gerador e índice de exportação.
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
