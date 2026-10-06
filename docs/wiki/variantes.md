# Variantes, protótipos e histórico

## `esp32gpsd/` — linha principal

Firmware do logger com GPS, DHT22, IMU opcional, WiFi, BLE e SdFat. Inclui ciclos de rádio coordenados e dados gravados localmente. Consulte o `.ino` e o README da própria pasta.

## `esp32gpsd_v2/` — variante de teste

Adiciona ciclo WiFi/BLE final ao parar, fechamento e flush na entrada do sono,
e retorno ao movimento acima de 5 km/h por 5 atualizações consecutivas. Também
concentra consumo BLE, posição GPS, buffers e escrita SD na `loopTask`, removendo
`bleConsumerTask` e `sdMutex`; aumenta RX GPS para 1024 bytes e reconfigura TWDT.
Consulte o [README da v2](../../esp32gpsd_v2/README.md).

O [hotspot para download](../../esp32gpsd_v2/hotspot.md) é um projeto documentado,
sem implementação HTTP no sketch atual.

## `esp32gpsd_v3/` — hotspot em teste

Evolui a v2 com `WebServer`, página em `http://192.168.4.1/` e download dos
três logs. Depois do check parado e da tentativa de flush, abre o AP; após
5 min sem atividade HTTP, fecha e entra no sono de 5 min. Retorno ao movimento
usa a histerese da v2 e fecha o AP. `servicoModo()` trata temporizadores e
ciclos de rádio mesmo sem fix novo. A tarefa `Hotspot_HTTP` roda no core 1;
`sdMutex` protege o SD, e downloads adiam flush/remount. Arquivos acima de
4.294.967.295 bytes são recusados. BLE é desinicializado antes do AP e reinicializado no próximo scan. Foco HTTP pausa aquisição e descarta UART GPS: não há detecção de movimento nesse período. Consulte o [README da v3](../../esp32gpsd_v3/README.md).

[Atlas DrawIO editável](../../esp32gpsd_v3/docs/diagramas/esp32gpsd-v3.drawio) · [Índice dos 15 diagramas](../../esp32gpsd_v3/docs/diagramas/README.md)

## `archive/esp32gpsd_dualcore/` — variante FreeRTOS sem BLE

Divisão por tarefas/núcleos para amostragem de sensores e trabalho WiFi/SD. Não implementa BLE. A documentação de conceitos e exemplos está em [`DUALCORE.MD`](../../archive/esp32gpsd_dualcore/DUALCORE.MD).

## `archive/ble_scanner_poc/` — PoC BLE histórica

Contém experimentos de varredura BLE e uma narrativa de problemas do sequenciador com reinicializações e persistência RTC. O arquivo `DEBUG.MD` registra tentativas antigas e seus resultados; leia-o como histórico de diagnóstico, não como descrição da arquitetura principal atual.

## `archive/esp32_gsm_gps/` — telemetria celular

Linha independente baseada em SIM800L/GPRS, com sketches Arduino e componentes de backend PHP/SQL. `Arduino_TinyGSM_esp32_ok.ino` e `esp32_gsm_gps_combined/` são entradas diferentes; verifique qual fluxo está sendo usado.

Permanece como submódulo Git, com revisão preservada e origem registrada em
`.gitmodules`. Para obter seus arquivos em um clone, execute
`git submodule update --init --recursive`.

## Experimentos e dependências

`archive/archive_tests/` guarda provas de conceito e testes de cartão/SdFat. `libraries/` inclui código vendorizado NimBLE; outras dependências copiadas ficam junto do projeto GSM. Evite editar cópias vendorizadas como se fossem código da aplicação, a menos que o objetivo seja alterar a própria dependência.

As bibliotecas ativas têm baseline, manifesto e
[procedimento de sincronização](../../libraries/README.md). A cópia DHT anterior
fica em `archive/vendor/DHT-1.4.6/`, fora dos caminhos de compilação.
