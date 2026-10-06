# Serial v3 — plano enxuto

## Base verificada

V3 atual: **1.348 linhas**, página HTML/CSS com atualização manual, sem `/api`. Decisões mantidas: resumo **5 s**, eventos imediatos, ASCII, GPS antigo mantém modo, lote SD incerto fica retido para retry.

## Console (legível, sem subsistema novo)

- Resumo a cada 5 s, gerado após processamento do modo, em bloco ASCII curto (~60 col), alinhado, uma linha por grupo:

```
+-- 00:05:10 | modo=MOVE | ciclo=42 ---------------------+
| GPS  ok  lat=-23.550520 lon=-46.633308 vel=12.3 idade=1s
| AMB  DHT 24.5C 51% | MPU 0.02 m/s2 0.00 rad/s
| RF   WiFi f=12 buf=10 dup=2 | BLE dren=30 buf=25 dup=5
| MEM  heap=182k | perdidos=0
| SD   ok buf=35 | HOT off
+-------------------------------------------------------+
```

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
- **Download:** a tarefa HTTP grava um estado atômico (`iniciado`/`concluido`/`erro`); a `loopTask` imprime na transição `[EVT] Download iniciado`, `[OK ] Download concluido` ou `[ERR] Download erro`. Sem bytes, duração ou motivo.

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
- Decisão: sem debug detalhado de HTTP (fila, bytes, duração, motivo). Só estados `Download iniciado`, `Download concluido`, `Download erro`, impressos pela `loopTask`.
- Serial só escrita pela `loopTask`, para não intercalar quadros do resumo.
