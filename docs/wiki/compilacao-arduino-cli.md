# Compilação e gravação via Arduino CLI

Registro do ambiente e dos comandos usados para compilar o firmware **`esp32gpsd_v3`** (logger + hotspot de download + dashboard) pela linha de comando. Valores medidos em **2026-10-05**.

## 1. Ambiente

| Item | Valor |
|---|---|
| SO | Linux 7.0.0-38-generic (x86_64) |
| `arduino-cli` | 1.5.1 (commit `01f3d4f2b`, 2026-06-05), em `~/.local/bin/arduino-cli` |
| Arduino IDE (também instalada) | 2.3.10 AppImage em `~/Documents/arduino-ide_2.3.10_Linux_64bit.AppImage` |
| Core | `esp32:esp32` **3.3.12** (Espressif Systems, arduino-esp32) |
| Compilador | `xtensa-esp-elf-g++` 14.2.0 (crosstool-NG `esp-14.2.0_20260121`), pacote `esp-x32` 2601 |
| esptool | 5.3.1 (`~/.arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool`) |
| Placa | **ESP32 Dev Module** — FQBN base `esp32:esp32:esp32` |
| Porta serial | `/dev/ttyACM0` (USB, `root:dialout`; usuário `luiz` está no grupo `dialout`) |
| Raiz do projeto | `/home/luiz/Documents/esp32` |
| Sketch | `esp32gpsd_v3/esp32gpsd_v3.ino` (a pasta tem o mesmo nome do `.ino`, exigência do Arduino) |
| Dados do arduino-cli | `directories.data = ~/.arduino15`, `directories.user = ~/Arduino` |
| `additional_urls` | nenhuma (o core vem do índice oficial `package_index.tar.bz2`) |

`arduino-cli board list` mostra `/dev/ttyACM0` como "Unknown": a placa não se identifica por USB, então o FQBN sempre é passado à mão.

### Opções da placa (ESP32 Dev Module, padrões do core)

| Opção | Padrão | Observação |
|---|---|---|
| `UploadSpeed` | 921600 | alternativas: 460800, 230400, 115200 |
| `CPUFreq` | 240 MHz (WiFi/BT) | |
| `FlashFreq` | 80 MHz | |
| `FlashMode` | QIO | |
| `FlashSize` | 4 MB (32 Mb) | flash física da placa |
| `PartitionScheme` | `default` (1,2 MB APP / 1,5 MB SPIFFS) | **não usar** — WiFi + NimBLE não cabem |

Para ver todas: `arduino-cli board details -b esp32:esp32:esp32`.

### Partição obrigatória

O projeto usa **`PartitionScheme=no_ota`**, que no core 3.3.12 aparece como **"No OTA (2MB APP/2MB SPIFFS)"**. É o mesmo esquema que a Arduino IDE antiga chamava de "No OTA (Large APP)" (por isso os READMEs usam esse nome). Resultado: **2.097.152 bytes (2 MB) para o programa**, sem OTA.

Outros esquemas disponíveis no core (referência): `huge_app` (3 MB APP / 1 MB SPIFFS, sem OTA — saída se algum dia faltar espaço), `min_spiffs` (1,9 MB APP com OTA), `noota_3g` (1 MB APP), `no_fs`, `minimal`, `default_8MB` (placas de 8 MB).

## 2. Bibliotecas

O sketch é compilado com as bibliotecas da pasta **`libraries/`** do projeto (fonte principal; ver [`libraries/README.md`](../../libraries/README.md) e `libraries/manifest.json`). A Arduino IDE usa a cópia em `~/Arduino/libraries`; `libraries/sync.py` mantém as duas iguais.

| Biblioteca | Versão | Uso |
|---|---|---|
| TinyGPS | 13.0.0 | parser NMEA |
| SdFat | 2.3.0 | cartão SD |
| DHT sensor library | 1.4.7 | DHT22 |
| Adafruit MPU6050 | 2.2.9 | IMU |
| Adafruit Unified Sensor | 1.1.15 | dependência |
| Adafruit BusIO | 1.17.4 | dependência |
| NimBLE-Arduino | 2.5.0 | BLE |
| Adafruit GFX / SSD1306 | 1.12.6 / 2.5.17 | só dependência declarada, não compilada |

Incluídas no core (sem instalar): `WiFi`, `WebServer`, `NetworkClient`, `SPI`, `Wire`, `esp_task_wdt`.

Antes de compilar, se mexeu em bibliotecas:

```sh
python3 libraries/sync.py --apply   # sincroniza ~/Arduino/libraries <-> libraries/
python3 libraries/sync.py --check   # só confere
```

## 3. Comandos

Todos executados na raiz do projeto (`/home/luiz/Documents/esp32`).

### Compilar

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --libraries libraries \
  esp32gpsd_v3
```

- `--fqbn ...:PartitionScheme=no_ota` — placa + opção de partição (opções extras entram separadas por vírgula, ex.: `:PartitionScheme=no_ota,UploadSpeed=460800`).
- `--libraries libraries` — usa as bibliotecas do projeto, não as do usuário.
- Primeira compilação: ~minutos; seguintes: **~14 s** (cache em `~/.cache/arduino/sketches/`).

Saída esperada (v3 com dashboard):

```text
Sketch uses 1259762 bytes (60%) of program storage space. Maximum is 2097152 bytes.
Global variables use 62416 bytes (19%) of dynamic memory, leaving 265264 bytes for local variables. Maximum is 327680 bytes.
```

### Compilar guardando os artefatos

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --libraries libraries \
  --output-dir build/v3 \
  esp32gpsd_v3
```

Gera em `build/v3/`: `esp32gpsd_v3.ino.bin` (~1,26 MB, imagem do app), `.bootloader.bin` (~25 KB), `.partitions.bin` (3 KB), `.elf`, `.map` e `.merged.bin` (4 MB, flash inteira num arquivo só, gravável no offset 0). `build/` não é versionado.

### Gravar

```sh
arduino-cli upload \
  --fqbn esp32:esp32:esp32:PartitionScheme=no_ota \
  --port /dev/ttyACM0 \
  esp32gpsd_v3
```

Compilar e gravar de uma vez: `arduino-cli compile --upload --port /dev/ttyACM0 --fqbn ... esp32gpsd_v3`.

> **Porta ocupada:** a porta só abre para um programa por vez. Se a Arduino IDE estiver com o Serial Monitor aberto, `upload` e `esptool` falham com `Could not open /dev/ttyACM0, the port is busy`. Feche o monitor (ou a IDE) antes.

### Monitor serial

```sh
arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
```

O firmware usa `Serial.begin(115200)`.

### Diagnóstico

```sh
arduino-cli version
arduino-cli core list                                  # core instalado
arduino-cli lib list                                   # bibliotecas visíveis
arduino-cli board list                                 # portas e placas
arduino-cli board details -b esp32:esp32:esp32         # opções e partições
arduino-cli config dump                                # diretórios e URLs
ls -l /dev/ttyACM0; groups | tr ' ' '\n' | grep dialout   # permissão da porta
fuser /dev/ttyACM0                                     # quem está usando a porta
~/.arduino15/packages/esp32/tools/esptool_py/5.3.1/esptool --port /dev/ttyACM0 flash-id   # chip/flash (porta livre)
```

Instalar o core do zero: `arduino-cli core update-index && arduino-cli core install esp32:esp32@3.3.12`.

## 4. Orçamento de flash e RAM (v3)

| Momento | Programa | RAM global |
|---|---|---|
| v3 com hotspot, antes do dashboard | 1.254.786 B (59%) | 62.416 B (19%) |
| v3 com dashboard (JS + `/api`, versão descartada) | 1.259.762 B (60%) | 62.416 B (19%) |
| v3 página HTML5 simples | 1.256.074 B (59%) | 62.416 B (19%) |
| v3 página HTML5 + CSS embutido (gravada) | 1.258.270 B (59%) | 62.416 B (19%) |
| Limite (`no_ota`) | 2.097.152 B | 327.680 B |

O painel custou ~3,5 KB de flash e 0 de RAM global (o CSS é um literal na flash, `PAGINA_CSS`). Folga atual: ~839 KB de programa. A placa recebeu 1.265.664 B no upload (imagem do app com preenchimento).

## 5. Problemas conhecidos

| Sintoma | Causa | Solução |
|---|---|---|
| `Sketch too big` / estouro de `.text` | partição `default` (1,2 MB) | usar `PartitionScheme=no_ota` |
| `port is busy` no upload | Serial Monitor da IDE (ou outro programa) na porta | fechar o monitor; conferir com `fuser` |
| `Permission denied` em `/dev/ttyACM0` | usuário fora de `dialout` | `sudo usermod -aG dialout $USER` e relogar |
| Erro de biblioteca/versão | cópia em `~/Arduino/libraries` divergente | `python3 libraries/sync.py --apply` |
| Placa "Unknown" em `board list` | placa sem identificação USB | normal; passar `--fqbn` sempre |

## 6. Página do hotspot (resumo para quem compila)

- Só HTML5 + CSS embutido (`PAGINA_CSS`), **sem JavaScript e sem rota `/api`**. `httpRaiz()` monta a página inteira numa `String` e envia de uma vez (sem `chunked`).
- Os valores vêm das globais `volatile` `dashTemp`, `dashUmid`, `dashBle` e de `ultimoWifiStats.total`; mudam ao clicar em "Atualizar".
- Barras via `<progress>` (WiFi, BLE) e `<meter>` (temperatura, umidade); cores via `accent-color`.
- Abrir a página renova o prazo de 5 min do hotspot (`ultimaAtividade`), como baixar um arquivo.
- Sem internet no hotspot: nenhuma fonte ou biblioteca externa.
