# Análise da distribuição de tarefas entre os cores

Data: 05/10/2026. Arquivo analisado: [esp32gpsd_v2.ino](esp32gpsd_v2.ino).

Análise estática, sem execução na placa. O diagnóstico abaixo preserva o estado anterior às correções; a distribuição atual e o status final registram o sketch vigente.

## Distribuição atual após correção

| Contexto | Responsabilidade atual |
|---|---|
| Host NimBLE, Core 0 na configuração instalada | Callback `onResult()` entrega registros à fila sem bloquear. |
| `loopTask`, Core 1 na configuração instalada | GPS, sensores, modos, resultados WiFi, `drenarFilaBLE()`, buffers, dashboard e SD. |

`setupBLE()` não cria tarefa adicional. `BLE_Consumer`, `sdMutex`, vetor de
deduplicação e callback de reinício foram removidos. `vTaskDelay(1)` cede CPU;
RX GPS tem 1024 bytes. Consulte o [README atual](README.md).

## Tarefas por core — diagnóstico anterior

Mapa considerando a configuração instalada: Arduino ESP32 3.3.12, `loopTask` no Core 1 e NimBLE com afinidade padrão no Core 0. Opções da IDE ou configurações de compilação podem alterar essas afinidades; o mapa não foi confirmado em execução no dispositivo.

| Core | Tarefas e responsabilidades |
|---|---|
| **0** | Driver WiFi, controlador Bluetooth e host NimBLE. Os callbacks BLE `onResult()` e `onScanEnd()` executam no contexto do host NimBLE. |
| **1** | `loopTask`: executa `setup()` e `loop()`, recebe GPS/UART, lê MPU6050 e DHT22, controla a máquina de estados, processa resultados WiFi, exibe dashboard e grava no SD. |
| **1** | `BLE_Consumer`: recebe registros pela fila, deduplica MACs, formata CSV e adiciona linhas ao buffer BLE. Prioridade **1**, stack **4096 bytes**. |
| **1**, na configuração instalada | Tarefa interna de eventos Arduino. |

O sketch cria explicitamente apenas uma tarefa adicional: `BLE_Consumer`, fixada no Core 1 por `xTaskCreatePinnedToCore()` em `setupBLE()`. As constantes `BLE_ORCHESTRATOR_STACK` e `BLE_ORCHESTRATOR_PRIORITY` não correspondem a uma tarefa criada nesta versão.

GPS, MPU6050, DHT22, SD e controle dos scans são funções executadas pela `loopTask`, não tarefas FreeRTOS independentes.

## Motivo da distribuição

A intenção compreendida é manter protocolos de rádio no Core 0 e trabalho de aplicação no Core 1.

O callback BLE entrega registros pela fila usando `xQueueSend(..., 0)`, sem esperar espaço. Se a fila estiver cheia, libera o registro e o descarta. Deduplicação persistente e formatação do CSV ficam no consumidor, fora da tarefa NimBLE. O consumidor bloqueia em `xQueueReceive(..., portMAX_DELAY)` quando a fila está vazia, evitando ocupar CPU sem trabalho.

Esse desenho é adequado para reduzir trabalho no contexto do stack BLE. Ainda há deduplicação transiente, manipulação de strings e alocação de memória no callback; ele não está totalmente livre de processamento.

O scan WiFi é assíncrono: a aplicação no Core 1 solicita o scan e processa resultados; o driver executa o trabalho interno no Core 0.

A gravação SD continua junto dos sensores no Core 1. Portanto, essa divisão não isola a recepção GPS e a amostragem MPU dos atrasos de escrita no cartão. Leituras de sensores também são chamadas síncronas.

## Problemas de concorrência e processamento — diagnóstico anterior

### 1. Buffer BLE sem proteção completa

`bleConsumerTask()` altera linhas, `bleBufferHead` e `bleBufferCount`. `flushBuffers()` lê e modifica esses mesmos dados.

O consumidor não adquire `sdMutex`. Esse mutex protege o acesso ao SD dentro do flush, mas não impede alterações concorrentes no buffer BLE. Encerrar o scan também não garante que a fila BLE esteja drenada ou que o consumidor tenha terminado de processar registros.

Consequência possível: perda de registros, linhas inconsistentes ou estado incorreto do buffer durante o flush.

Nesta versão, o consumidor não grava diretamente no SD; apenas acumula registros no buffer. O comentário sobre possíveis escritores concorrentes do SD não resolve a concorrência entre produtor do buffer e flush.

### 2. Posição GPS sem snapshot protegido

`lastTimeStamp`, `lastLat` e `lastLon` são atualizados pela `loopTask` e lidos pelo consumidor sem proteção do conjunto.

Uma troca de tarefa durante a atualização ou leitura pode produzir timestamp e coordenadas de atualizações diferentes. Estar no mesmo core não torna essa sequência indivisível.

Além disso, `rec->timestamp` é capturado no callback, mas ignorado na formatação do CSV. O registro usa a última posição/hora disponível no momento do consumo, que pode diferir do momento da detecção BLE.

### 3. Vetor de deduplicação compartilhado entre cores

`seenInCycleBLE` é percorrido e modificado pelos callbacks BLE e limpo por `bleScanLigar()`, chamado pela aplicação.

Não há sincronização explícita no sketch entre parada do scan, callbacks pendentes e início do próximo ciclo. É necessário garantir essa ordem para evitar acesso concorrente ao vetor. `bleParar` é atômico, mas protege apenas a flag; não protege o vetor nem todas as operações de reinício.

### 4. Loop ocupa CPU sem pausa explícita

O ciclo de aproximadamente um segundo em `loop()` consulta UART e tempo continuamente, sem `delay()` ou `vTaskDelay()` explícito.

A `BLE_Consumer`, também de prioridade 1, ainda pode receber CPU pelo escalonamento e divisão de tempo do FreeRTOS. Entretanto, a espera ativa desperdiça processamento e reduz oportunidades de execução da tarefa idle no Core 1.

Afinidade comum não elimina concorrência: tarefas no mesmo core podem alternar execução entre operações de leitura e escrita. Referência: [FreeRTOS da Espressif](https://docs.espressif.com/projects/esp-idf/en/v5.1/esp32/api-guides/freertos-smp.html).

## Limite físico dos rádios

WiFi e BLE compartilham um recurso de rádio no ESP32. A coexistência alterna seu uso no tempo; dois cores não permitem transmissão física simultânea dos dois protocolos.

Referência: [RF Coexistence — Espressif](https://docs.espressif.com/projects/esp-idf/en/v5.4/esp32/api-guides/coexist.html).

## Recomendações do diagnóstico anterior

- Manter inicialmente a distribuição atual e corrigir a sincronização dos dados compartilhados antes de redistribuir tarefas.
- Proteger o buffer BLE em todas as operações de inserção e flush, ou definir um único proprietário. Evitar manter um lock compartilhado com o consumidor durante a escrita lenta no SD; considerar snapshot ou troca de buffers.
- Publicar e consumir posição/hora GPS como um snapshot coerente. Definir se o registro BLE deve representar o momento da detecção ou do consumo.
- Garantir a ordem entre parada, término dos callbacks e reinício do scan; concentrar a manipulação de `seenInCycleBLE` em um contexto ou protegê-la adequadamente.
- Substituir a espera ativa por uma espera curta/bloqueante compatível com a cadência UART e MPU.
- Avaliar uma tarefa dedicada ao SD se medições mostrarem atrasos ou perdas GPS durante gravação. Mover trabalho para o Core 0 exige considerar a carga e a prioridade das tarefas de rádio.

## Status (05/10/2026, após correção)

- **1 e 2 — resolvidos**: tarefa `BLE_Consumer` removida; `drenarFilaBLE()` drena a fila (sem bloquear) na `loopTask`, que passa a ser a dona única do `bleBuffer` e de `lastTimeStamp/lastLat/lastLon`. `sdMutex` removido (havia um único chamador).
- **3 — resolvido**: `seenInCycleBLE`, `bleParar` e o restart em `onScanEnd()` foram removidos. O scan dura `BLE_SCAN_DURATION_MS` e termina sozinho; a dedup dentro do scan usa o `filter_duplicates` nativo do NimBLE (ligado por padrão).
- **4 — resolvido**: `vTaskDelay(1)` no ciclo de 1 s.
- **SD x GPS**: `Serial2.setRxBufferSize(1024)` em vez de criar uma tarefa SD.
- **Extra**: o core 3.3.12 já inicia o TWDT (`CONFIG_ESP_TASK_WDT_INIT=y`, 5 s, panic). `esp_task_wdt_init()` falhava, e o timeout real era 5 s. Agora usa `esp_task_wdt_reconfigure()`, com timeout de 60 s.
- Mortos removidos: `BLE_ORCHESTRATOR_*`, `BLE_CONSUMER_*`, `lastKmh`, `BLEDeviceRecord::timestamp`, parâmetro `kmh` de `varrerWiFi()`.

Conclusão atual: a propriedade dos buffers e da posição GPS está concentrada na `loopTask`. SD e aquisição de sensores continuam no mesmo fluxo; impacto de operações lentas e afinidades de core ainda exigem validação na placa.
