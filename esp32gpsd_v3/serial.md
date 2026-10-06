# Serial v3 — plano enxuto

## Base verificada

V3 atual: página HTML/CSS com atualização manual, sem `/api`. Decisões mantidas: resumo compacto (5 s; 30 s com hotspot; mudo em download), eventos imediatos, ASCII, GPS antigo mantém modo, lote SD incerto fica retido para retry.

## Console (legível, sem subsistema novo)

- Resumo compacto de 3 a 4 linhas, sem moldura, gerado após o processamento do modo. Intervalo de **5 s**; **30 s** com o hotspot aberto; **mudo** durante download (não disputa CPU/heap com a tarefa HTTP):

```
[00:05:10] PARADO (hotspot) | 0.5 km/h | loop #310 1001ms
  GPS -15.82641,-47.98718 | 26.2C 58% | az=10.8
  RF #3 wifi=19 ble=10 | SD ok 10/150 0/50 0/50 | heap=76KB max=41KB perdas=0
  WEB ESP32GPS-Logs clientes=1 fecha 00:04:40 | pag=5 dl=0 ok=0 erro=0
```

  A linha `WEB` só aparece com o hotspot aberto. `heap`/`max` = heap livre e maior bloco contíguo (diagnóstico de falta de RAM).
- Eventos (boot, scan falho, SD falho, download início/fim) em linha única com prefixo `[EVT]`, `[ERR]`, `[OK ]`.
- Impressão direta com `Serial.printf` só na `loopTask`; a tarefa `Hotspot_HTTP` e callbacks BLE só atualizam flags/contadores atômicos. Sem fila.
- Contadores: WiFi encontrados/bufferizados/duplicados; BLE drenados (`dashBle` atual) e `perdidos` global (alocação, fila, sobrescrita). Cache cheio: conhecidos deduplicados, novos IDs não entram.
- DHT: idade da leitura efetiva, intervalo mínimo 2 s. MPU: unidades `m/s²`, `rad/s`. Idade só da última posição GPS.

## Correções de processamento

- **GPS:** velocidade válida só vem de RMC; GGA não a renova. Velocidade inválida não vira zero. Após 3 s sem velocidade válida, suspender decisões por velocidade e manter modo.
- **Movimento:** limiares 2/5 km/h atuais, sem regra nova.
- **CSV:** inicializar `tm`, UTC-3, validar posição/data/hora antes de adicionar GPS. Campos inválidos vazios. Achados WiFi/BLE usam última posição conhecida (idade no console).
- **Scans:** checar retorno de início WiFi/BLE; concluir WiFi por resultado real e BLE por callback. Falha não vira scan vazio. Sem flush/hotspot com scan ativo.
- **SD:** confirmar tamanho escrito, `sync()` e `close()`. Remover lote só com confirmação completa; falha mantém lote e loga a etapa.
- **Retry:** preservar flush adiado; retomar após scan/download. Manter laço e limite atual de 10 falhas de remount.
- **Boot:** cabeçalho, marcador e watchdog só anunciam sucesso após conferir retornos.
- **Download:** a tarefa HTTP grava um estado atômico (`iniciado`/`concluido`/`erro`); a `loopTask` imprime na transição `[EVT] Download iniciado`, `[OK ] Download concluido` ou `[ERR] Download erro`. A linha final traz bytes, duração, `motivo` do aborto, esperas de TCP (`stalls`), `heap` e `maxblk`.

## Compatibilidade e documentação

- Manter HTML/CSS atual, atualização manual, endpoints `/` e `/download`; **não recriar `/api`**.
- Preservar colunas CSV, coordenadas em milionésimos, deduplicação SSID/MAC e cadências.
- HTTP roda na tarefa `Hotspot_HTTP` (core 1), não na `loopTask`. `sdMutex` atual cobre o SD; dashboard continua `volatile`/atômico. Sem mutex novo.
- Corrigir README e doc de compilação que ainda citam polling/API.
- Sem upload, commit ou push automático.

## Validação

- Compilar ESP32 **3.3.12**, perfil `esp32:esp32:esp32:PartitionScheme=no_ota`; sincronizar bibliotecas e rodar `--check`.
- Na placa: sem GPS, troca de modo, hotspot + download, falha SD (retirar cartão); conferir que nenhum lote incerto é anunciado como confirmado. Capturar log com versão.

## Observações verificadas

- `server.handleClient()` roda na tarefa `Hotspot_HTTP` (core 1, `esp32gpsd_v3.ino:1160,1172`), não na `loopTask`; SD já protegido por `sdMutex` (`:95`); `dashTemp`/`dashUmid`/`dashBle` são `volatile` (`:175-178`).
- Decisão revista: o download imprime motivo, stalls, heap e maior bloco no fim (diagnóstico da falha de heap). Estados `Download iniciado`, `Download concluido`, `Download erro`, impressos pela `loopTask`.
- Serial só escrita pela `loopTask`, para não intercalar quadros do resumo.
