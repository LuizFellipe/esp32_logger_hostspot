# Histórico preservado

Os firmwares ativos continuam em [`esp32gpsd/`](../esp32gpsd/README.md) e
[`esp32gpsd_v2/`](../esp32gpsd_v2/README.md). Esta pasta guarda referências;
seu conteúdo não é dependência de compilação dos firmwares ativos.

| Pasta | Conteúdo |
|---|---|
| `archive_tests/` | Experimentos antigos de sensores, rádio, SD e SdFat. |
| `ble_scanner_poc/` | PoC BLE e diagnóstico do sequenciador por restart. |
| `esp32gpsd_dualcore/` | Variante sem BLE e documentação `DUALCORE.MD`. |
| `esp32_gsm_gps/` | Submódulo independente com telemetria celular e backend PHP/SQL. |
| `vendor/DHT-1.4.6/` | Cópia original de DHT preservada antes da baseline de bibliotecas. |

Para recuperar GSM em um clone, execute `git submodule update --init --recursive`.
Não atualize a revisão do submódulo automaticamente ao organizar o projeto.
