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
