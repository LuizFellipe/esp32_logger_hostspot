# Inventário do repositório

| Caminho | Papel |
|---|---|
| `esp32gpsd/` | Firmware principal do logger GPS/sensores/WiFi/BLE e seu README. |
| `esp32gpsd_v2/` | Variante de teste do logger, com sketch e README. |
| `archive/esp32gpsd_dualcore/` | Logger alternativo multi-tarefa, sem BLE. |
| `archive/esp32_gsm_gps/` | Protótipos GSM/GPRS, GPS, DHT, exemplos de bibliotecas e backend PHP/SQL. |
| `archive/ble_scanner_poc/` | Prova de conceito e notas históricas de BLE. |
| `archive/archive_tests/` | Experimentos antigos de SD/SdFat e dependências relacionadas. |
| `libraries/` | Dependências vendorizadas, incluindo NimBLE-Arduino. |
| `log/` | Logs locais preservados, ignorados pelo Git. |
| `CONTEXT.md` | Vocabulário e conceitos de domínio do GPS Logger. |
| `archive/esp32gpsd_dualcore/DUALCORE.MD` | Notas e exemplos sobre a variante dual-core. |
| `README.md` | Visão e instruções gerais do repositório (consultar junto desta wiki). |
| `docs/GRAPHIFY.md` | Diagrama de relações entre projetos e fluxos. |
| `docs/wiki/` | Documentação integrada e inventário. |
| `docs/hardware/pinout.png` | Imagem original de pinout. |
| `graphify-out/` | Grafo, relatório e artefatos gerados versionados. |
| `.serena/` | Configuração e memórias; cache, logs e config local ignorados. |
| `.agents/`, `.claude/` | Skills e configurações das ferramentas; temporários ignorados. |
| `.tokensave/` | Configuração preservada; DB e arquivos auxiliares apenas locais. |
| `archive/vendor/DHT-1.4.6/` | Cópia de DHT anterior à baseline, preservada. |

## Como manter a wiki

Quando o comportamento mudar, atualize a página da wiki e o README da pasta afetada na mesma alteração. Mantenha o Graphify focado nas relações entre componentes; coloque detalhes de configuração e procedimentos nas páginas específicas. Para um detalhe de implementação, o sketch é a fonte de verdade.
