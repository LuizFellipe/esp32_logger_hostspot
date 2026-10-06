# Bibliotecas do projeto

Esta pasta é a fonte principal das bibliotecas externas dos firmwares ativos.
A baseline foi importada das cópias usadas pela Arduino IDE após compilar v1 e v2
com sucesso em 2026-10-05. O core ESP32 instalado é **3.3.12**.

| Diretório | Versão | Uso |
|---|---|---|
| `TinyGPS` | 13.0.0 | Parser GPS. |
| `SdFat` | 2.3.0 | Cartão SD. |
| `DHT_sensor_library` | 1.4.7 | Sensor DHT22. |
| `Adafruit_MPU6050` | 2.2.9 | Sensor IMU. |
| `Adafruit_Unified_Sensor` | 1.1.15 | Dependência de sensores. |
| `Adafruit_BusIO` | 1.17.4 | Dependência de comunicação. |
| `NimBLE-Arduino` | 2.5.0 | BLE. |
| `Adafruit_GFX_Library` | 1.12.6 | Dependência declarada pelo pacote MPU6050; não compilada pelos sketches atuais. |
| `Adafruit_SSD1306` | 2.5.17 | Dependência declarada pelo pacote MPU6050; não compilada pelos sketches atuais. |

[`manifest.json`](manifest.json) registra diretórios, versões e dependências.
A DHT local anterior, v1.4.6, permanece em
[`archive/vendor/DHT-1.4.6/`](../archive/vendor/DHT-1.4.6/).
SPI, Wire, WiFi, Network e demais componentes integrados ficam no pacote ESP32.

## Antes de cada nova versão

Execute na raiz:

```sh
python3 libraries/sync.py --apply
python3 libraries/sync.py --check
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota esp32gpsd
arduino-cli compile --fqbn esp32:esp32:esp32:PartitionScheme=no_ota esp32gpsd_v2
```

O sincronizador compara caminhos e SHA-256 de todos os arquivos gerenciados.
`--check` não escreve e retorna erro quando há divergências. `--apply` copia
**projeto → IDE**, verifica o resultado e preserva cópias substituídas em
`~/Arduino/.esp32-library-backups/`. Bibliotecas não gerenciadas permanecem intactas.
Destino padrão: `~/Arduino/libraries`; neste ambiente,
`/home/luiz/Arduino/libraries`. Use `--ide-dir /caminho/libraries` em outro ambiente.

Atualizações entram primeiro no projeto: substitua a biblioteca, atualize versão
e dependências no manifesto e sincronize antes de compilar. Preserve alterações
locais que precisar consultar antes de substituir uma biblioteca. Dependências
declaradas novas também devem entrar no manifesto. Não use `--libraries` para
contornar divergências com a IDE.

O perfil antigo salvo da v1 (DOIT DevKit com opção `PartitionScheme=huge_app`)
não é aceito pelo core 3.3.12. A baseline atual de ambos os sketches usa o perfil
acima: **ESP32 Dev Module → No OTA (Large APP)**. Isto registra a configuração
validada; não exige alterar o código nem fazer upload para validar organização.
