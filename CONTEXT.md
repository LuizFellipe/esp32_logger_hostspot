# Contexto do Projeto: ESP32 GSM GPS Logger

Este documento define os termos e conceitos de domínio utilizados no projeto do registrador de dados (logger) GPS/WiFi com ESP32.

## Glossário de Termos

### GPS Logger
Dispositivo embarcado baseado em ESP32 que captura e registra coordenadas geográficas, dados de movimento e telemetria de sensores no cartão SD.

### APP_CPU (Core 1)
O processador de aplicação do ESP32. Utilizado para leitura de sensores de alta frequência (MPU6050, DHT22) e recepção de dados via UART (GPS), garantindo que não ocorra perda de pacotes ou atraso na amostragem.

### PRO_CPU (Core 0)
O processador de protocolo do ESP32. Utilizado para operações de rede sem fio (WiFi) e escrita física lenta no cartão SD, isolando a latência do barramento SPI dos sensores de tempo real.

### Circular Buffer (Buffer Circular)
Estrutura de dados em RAM que acumula temporariamente strings de log (GPS e WiFi) em ordem cronológica. Se o buffer encher devido à indisponibilidade do cartão SD, os registros mais antigos são descartados para dar espaço aos mais novos.

### SSID Cache (Cache de SSID)
Conjunto de hashes FNV-1a de 32 bits que rastreia identificadores de rede (SSIDs) já gravados no log de WiFi, evitando duplicidades de escrita e economizando memória RAM e espaço em disco.

### BLE Scanner
Componente do sistema responsável por varrer ativamente o espectro Bluetooth Low Energy em busca de dispositivos anunciantes ao redor. Opera em modo ativo (envia SCAN_REQ para receber SCAN_RSP), capturando o conjunto completo de dados de advertising de cada dispositivo encontrado.

### BLE Device Record (Registro de Dispositivo BLE)
Estrutura de dados alocada dinamicamente (heap) que representa um dispositivo BLE detectado. Contém todos os campos disponíveis no pacote de advertising: endereço MAC, tipo de endereço, nome, RSSI, TX Power, Manufacturer Data (raw bytes), Service UUIDs, Service Data, Appearance, flags, tipo de advertising e indicadores de conectabilidade. Enviada por ponteiro via FreeRTOS Queue.

### BLE Scan Queue
FreeRTOS Queue de profundidade 10 que transporta ponteiros para BLE Device Records do callback do scanner (Core 1) para a task consumer (Core 1). O produtor descarta o record (free) se a queue estiver cheia, sem bloquear o BT stack.

### BLE Deduplication (Deduplicação BLE)
Estratégia de controle de duplicatas por endereço MAC dentro de um ciclo de scan. Se um dispositivo já foi visto no ciclo atual, o RSSI é atualizado no record existente em vez de criar um novo. Garante que cada dispositivo apareça uma vez por ciclo com o RSSI mais recente.

### RF Time-Sharing (Compartilhamento de RF)
Política do sistema que impede operação simultânea de WiFi, Bluetooth Clássico e BLE, alternando entre os modos usando `esp_restart()` entre fases. Cada fase ativa exclusivamente uma tecnologia de rádio. Nunca ativos ao mesmo tempo.

### RF Phase Sequencer (Sequenciador de Fases RF)
Mecanismo que determina qual tecnologia de rádio estará ativa após cada `esp_restart()`. O estado da fase atual é persistido em `RTC_DATA_ATTR` (memória RTC Slow, sobrevive a resets de software mas apaga em power-off). No `ble_scanner_poc.ino` (PoC original) o ciclo é: BLE → Bluetooth Clássico → WiFi → BLE. No `esp32gpsd.ino` (logger de produção, fundido a partir da PoC) a ordem é invertida: **WiFi → BLE → BT → WiFi**, porque o WiFi precisa continuar ativo por longos períodos (`WIFI_SLEEP_MS`, ~5 min) para não perder cobertura de rede durante o deslocamento, enquanto BLE (`PHASE_BLE_MS`, 30 s) e BT (`PHASE_BT_MS`, 15 s) são excursões curtas periódicas — ver "Fusão RF no esp32gpsd" abaixo.

### Classic BT Scan Phase (Fase de Scan Bluetooth Clássico)
Fase do RF Phase Sequencer dedicada à descoberta de dispositivos Bluetooth Clássico via inquiry GAP. Utiliza `BluetoothSerial::discoverAsync()` com um timeout de inquiry auto-calibrado (`PHASE_BT_MS - 3000 ms`). Retorna endereço MAC, nome (se anunciado), Class of Device (CoD) e RSSI.

### BTDeviceRecord (Registro de Dispositivo BT Clássico)
Estrutura de dados que representa um dispositivo Bluetooth Clássico detectado durante a Classic BT Scan Phase. Contém: endereço MAC, nome, Class of Device (CoD) e RSSI. Transportada por ponteiro via FreeRTOS Queue do callback de descoberta para a consumer task.

### BT Inquiry
Processo de descoberta ativa de dispositivos Bluetooth Clássico. O ESP32 emite pacotes de inquiry e aguarda respostas dos dispositivos ao redor. Diferente do BLE advertising, o inquiry clássico é mais lento (segundos por dispositivo) e requer que o dispositivo-alvo esteja em modo discoverable.

### Fusão RF no esp32gpsd (esp32gpsd.ino)
Incorporação do RF Phase Sequencer da PoC (`ble_scanner_poc.ino`) ao logger de produção `esp32gpsd.ino`, para agregar dados de BT Clássico e BLE ao log de GPS/WiFi sem interromper a amostragem contínua de GPS/DHT22/MPU6050. Diferenças-chave em relação à PoC:
- **Fase WiFi vira a fase "padrão" de longa duração** (reaproveita `varrerWiFi()`/`wifiDormindo` já existentes, sem task dedicada), e BLE/BT viram excursões curtas periódicas disparadas a cada `WIFI_SLEEP_MS`.
- **Logging de GPS/DHT/MPU/SD roda em todas as fases**, inclusive durante as excursões BLE/BT (tasks de scan pinadas no Core 0, `loop()` de sensores continua no Core 1).
- `ssidCacheHash` (antes em RAM comum) e as novas `bleCacheHash`/`btCacheHash` migram para `RTC_DATA_ATTR`, para sobreviver aos restarts periódicos sem duplicar SSIDs/MACs a cada ciclo.
- `BLEDeviceRecord`/`BTDeviceRecord` do `esp32gpsd.ino` são versões simplificadas das da PoC (sem manufacturer data, service UUIDs, appearance, Class of Device) — só os campos usados no CSV de log.
- Build requer **Partition Scheme → "No OTA (Large APP)"** na Arduino IDE (WiFi + BluetoothSerial + NimBLE juntos estouram a flash padrão de 4MB), mesma exigência documentada em `ble_scanner_poc/upgrade.txt`.

### Cache Genérica de Hash (hashString / hashJaVisto / adicionarHashCache)
Generalização das funções de dedup antes específicas de SSID (`hashSSID`/`ssidJaVista`/`adicionarSSIDCache`). `hashString()` calcula FNV-1a 32-bit de qualquer string; `hashJaVisto()` e `adicionarHashCache()` recebem o array/contador/limite como parâmetros, permitindo reutilizar a mesma lógica para as três caches (SSID, BLE MAC, BT MAC) em vez de três cópias quase idênticas.

### sdMutex
`SemaphoreHandle_t` que protege o objeto `SdFs sd` (compartilhado) contra corrida entre as tasks orquestradoras de BLE/BT (Core 0) e o `loop()` principal (Core 1), já que ambos podem chamar `flushBuffers()` concorrentemente.

### Flush-Antes-de-Restart (com retry)
Política do `esp32gpsd.ino`: antes de trocar de fase e chamar `triggerRestart()`, o código chama `flushBuffers()` e só avança a fase se o flush confirmar sucesso (todos os buffers com dados gravados no SD). Em caso de falha (SD indisponível), a fase atual é mantida e o flush é tentado de novo no próximo ciclo, evitando perda de linhas de log por causa do timing do restart.

### ble.txt / bt.txt
Arquivos de log CSV criados pelo `esp32gpsd.ino` para achados BLE e BT Clássico, no mesmo padrão do `wifi.txt` existente (sem cabeçalho, uma linha por dispositivo único por ciclo: timestamp, lat, lon, MAC, nome, RSSI — BLE inclui TX Power quando anunciado). Escritos via os mesmos buffers circulares (`bleBuffer`/`btBuffer`) e funções (`adicionarLinhaCircular`/`appendLinhasCirculares`) já usados por `log.txt`/`wifi.txt`.
