# Hardware, configuração e dados

## Ligações do logger principal

| Módulo | Interface | GPIO no firmware principal |
|---|---|---|
| GPS NEO-6M | UART2 | RX do ESP32 = 17; TX do ESP32 = 16 |
| DHT22 | Digital | 32 |
| MPU6050 (opcional) | I²C | SDA 21; SCL 22 |
| Cartão SD | SPI | CS 5; MOSI 23; MISO 19; SCK 18 |

Confirme a pinagem no sketch antes de ligar uma placa diferente. Alimentação depende do módulo específico; não presuma que qualquer periférico aceita 5 V.

## Configurações relevantes

O sketch define baud rate GPS, pinos, clock SPI, tamanhos dos buffers, limites de cache, timeout do watchdog e limiares/intervalos de movimento e scan. Essas definições são a referência para os valores atuais. O README do logger explica a seleção de partição e as bibliotecas utilizadas.

## Dados registrados

`log.txt` agrega data/hora, latitude/longitude, satélites, HDOP, velocidade, direção, umidade/temperatura e medidas IMU. Se o MPU6050 não inicializar, os campos IMU podem ficar sem valor.

`wifi.txt` registra data/hora, posição associada, SSID, RSSI, canal e tipo de segurança. `ble.txt` registra data/hora, posição associada, endereço MAC, nome, RSSI e TX power quando disponível. A coordenada e a hora refletem a última posição GPS conhecida no momento em que o registro é composto.

Arquivos de exemplo da pasta [`log/`](../../log) são amostras, não contratos de formato; confirme cabeçalhos/ordem no firmware correspondente.

## Logger celular

O sketch combinado da pasta [`archive/esp32_gsm_gps/`](../../archive/esp32_gsm_gps) usa SIM800L em UART, TinyGSM e um endpoint HTTP; o conjunto inclui `get.php`, `db_connect.php` e `database_setup.sql`. Os pinos e APN variam entre exemplos. Há valores de configuração sensíveis/documentados em arquivos antigos: não publique credenciais reais nem reutilize valores do README sem validar e substituir no ambiente de implantação.
