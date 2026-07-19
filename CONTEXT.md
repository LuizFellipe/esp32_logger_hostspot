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
Mecanismo que determina qual tecnologia de rádio estará ativa após cada `esp_restart()`. O estado da fase atual é persistido em `RTC_DATA_ATTR` (memória RTC Slow, sobrevive a resets de software mas apaga em power-off). O ciclo padrão é: BLE → Bluetooth Clássico → WiFi → BLE.

### Classic BT Scan Phase (Fase de Scan Bluetooth Clássico)
Fase do RF Phase Sequencer dedicada à descoberta de dispositivos Bluetooth Clássico via inquiry GAP. Utiliza `BluetoothSerial::discoverAsync()` com um timeout de inquiry auto-calibrado (`PHASE_BT_MS - 3000 ms`). Retorna endereço MAC, nome (se anunciado), Class of Device (CoD) e RSSI.

### BTDeviceRecord (Registro de Dispositivo BT Clássico)
Estrutura de dados que representa um dispositivo Bluetooth Clássico detectado durante a Classic BT Scan Phase. Contém: endereço MAC, nome, Class of Device (CoD) e RSSI. Transportada por ponteiro via FreeRTOS Queue do callback de descoberta para a consumer task.

### BT Inquiry
Processo de descoberta ativa de dispositivos Bluetooth Clássico. O ESP32 emite pacotes de inquiry e aguarda respostas dos dispositivos ao redor. Diferente do BLE advertising, o inquiry clássico é mais lento (segundos por dispositivo) e requer que o dispositivo-alvo esteja em modo discoverable.
