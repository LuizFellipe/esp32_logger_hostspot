# Visão geral

Este repositório reúne experimentos e firmwares Arduino para ESP32. A linha mais completa é o **GPS Logger**, que grava posição, sensores e observações de rádio em microSD. O projeto GSM funcional com SIM800L e backend PHP/MySQL permanece no repositório independente `esp32_gsm_gps`, acessível aqui pelo submódulo histórico.

## Escolha o caminho

| Objetivo | Comece aqui |
|---|---|
| Entender o logger atual | [`esp32gpsd/README.md`](../../esp32gpsd/README.md) e [`esp32gpsd/esp32gpsd.ino`](../../esp32gpsd/esp32gpsd.ino) |
| Ver variante em teste | [`esp32gpsd_v2/README.md`](../../esp32gpsd_v2/README.md) e [`esp32gpsd_v2/esp32gpsd_v2.ino`](../../esp32gpsd_v2/esp32gpsd_v2.ino) |
| Usar o hotspot em teste | [`esp32gpsd_v3/README.md`](../../esp32gpsd_v3/README.md) e [`esp32gpsd_v3/esp32gpsd_v3.ino`](../../esp32gpsd_v3/esp32gpsd_v3.ino) |
| Consultar a alternativa sem BLE | [`archive/esp32gpsd_dualcore/esp32gpsd_dualcore.ino`](../../archive/esp32gpsd_dualcore/esp32gpsd_dualcore.ino) e [`DUALCORE.MD`](../../archive/esp32gpsd_dualcore/DUALCORE.MD) |
| Ver telemetria com SIM800L | [`archive/esp32_gsm_gps/README_PROJETO.md`](../../archive/esp32_gsm_gps/README_PROJETO.md) |
| Investigar BLE experimental | [`archive/ble_scanner_poc/ble_scanner_poc.ino`](../../archive/ble_scanner_poc/ble_scanner_poc.ino) e [`DEBUG.MD`](../../archive/ble_scanner_poc/DEBUG.MD) |
| Entender os termos do logger | [`CONTEXT.md`](../../CONTEXT.md) |

## Família do logger

O firmware principal lê NMEA do GPS pela UART, coleta DHT22 e MPU6050 opcional, e executa scans WiFi/BLE. O cartão SD recebe arquivos separados para telemetria periódica e achados de rádio. A descrição dos ciclos e da coordenação está em [Arquitetura do logger](arquitetura-logger.md); o formato das saídas está em [Dados e hardware](dados-hardware.md).

## Protótipo celular

`archive/esp32_gsm_gps/` contém sketches com SIM800L/TinyGSM, arquivos PHP e SQL. É uma arquitetura distinta do logger em SD. O README existente descreve a integração planejada; confira o sketch concreto para saber o que está implementado em cada variante.

## Atenção às fontes

O repositório possui conteúdo histórico, bibliotecas copiadas e arquivos de log. Para comportamento atual, prefira o `.ino` em questão e seu README; documentos de diagnóstico podem narrar tentativas passadas que foram revertidas.
