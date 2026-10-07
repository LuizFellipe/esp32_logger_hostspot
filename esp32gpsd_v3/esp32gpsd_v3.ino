#include <TinyGPS.h>
#define DISABLE_FS_H_WARNING
#include "SdFat.h"
#include "SPI.h"
#include "DHT.h"
#include "WiFi.h"
#include <Adafruit_MPU6050.h>
#include <Adafruit_Sensor.h>
#include <Wire.h>
#include "esp_task_wdt.h"
#include <NimBLEDevice.h>
#include <WebServer.h>
#include <string>
#include <atomic>
#include <stdarg.h>

// Arduino IDE -> Tools -> Partition Scheme -> "No OTA (Large APP)" —
// necessario pra WiFi + NimBLE juntos nao estourar a Flash.

// Configuração SdFat
const uint8_t SD_CS_PIN = 5;
#define SPI_CLOCK SD_SCK_MHZ(16)
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SPI_CLOCK)

// Configuração do GPS
#define GPS_RX 17
#define GPS_TX 16
#define GPS_Serial_Baud 9600

// Configuração do DHT22
#define DHTPIN 32
#define DHTTYPE DHT22

// Configuração de buffer (buffers circulares por linha: se o SD ficar
// indisponível e o buffer encher, as linhas mais antigas são descartadas
// para abrir espaço às mais recentes)
#define LOG_BUFFER_MAX    150    // Linhas de log GPS acumuladas antes de flush
#define WIFI_BUFFER_MAX   50     // Linhas de log WiFi acumuladas antes de flush
#define BLE_BUFFER_MAX    50     // Linhas de log BLE acumuladas antes de flush
#define SSID_CACHE_MAX    500    // Máx SSIDs rastreadas para deduplicação (guarda hash, não string)
#define BLE_CACHE_MAX     500    // Máx MACs BLE rastreadas para deduplicação

// Watchdog: se o loop() não "alimentar" o watchdog nesse tempo, o ESP32
// assume que travou (SD/I2C pendurado etc.) e reseta sozinho.
#define WDT_TIMEOUT_S 60

// Se o remount do SD falhar essa quantidade de vezes seguidas, reinicia
// o ESP32 inteiro (na esperança de que um boot limpo destrave o hardware).
#define SD_REMOUNT_MAX_FALHAS 10

// ─────────────────────────────────────────────────────────────────────────────
// Modo Movimento/Parado — sem restart: WiFi e BLE coexistem no mesmo boot e
// nunca são desligados (energia não é mais economizada, só o cartão SD via
// cadência de scan). Os dois rádios sempre escaneiam juntos, em ciclos:
// em Movimento, um ciclo a cada RADIO_SCAN_INTERVAL_MS; parado, dorme
// PARKED_SLEEP_MS sem escanear, acorda, faz 1 ciclo WiFi+BLE e volta a dormir.
// Volta pro modo Movimento imediatamente se a velocidade subir de novo.
// ─────────────────────────────────────────────────────────────────────────────
#define PARKED_KMH_THRESHOLD 2.0   // <= isso: considerado parado
#define MOVING_KMH_THRESHOLD 5.0   // > isso (por MOVING_DEBOUNCE_FIXES fixes seguidos): volta a Movimento
#define MOVING_DEBOUNCE_FIXES 5    // fixes (~1 s cada) consecutivos acima do limiar de movimento
#define PARKED_SLEEP_MS       (5UL * 60UL * 1000UL)  // sono entre checks
#define RADIO_SCAN_INTERVAL_MS 30000UL  // cadência do ciclo WiFi+BLE em movimento

// Cadência de gravação do log.txt no buffer: em movimento, mais espaçada
// (menos desgaste de flash); parado, ainda mais espaçada (nada muda).
#define LOG_ADD_MOVING_MS  10000UL
#define LOG_ADD_PARKED_MS  30000UL

#define MODO_MOVIMENTO      0
#define MODO_PARADO_SONO    2
#define MODO_PARADO_CHECK   3
#define MODO_PARADO_HOTSPOT 4

// ─────────────────────────────────────────────────────────────────────────────
// Hotspot de download dos logs — aberto parado, após o check WiFi+BLE.
// Fecha após HOTSPOT_IDLE_MS sem atividade (abrir página/baixar renova) e
// dorme PARKED_SLEEP_MS antes do próximo check. Editar a senha antes de gravar
// (WPA2: 8 a 63 caracteres).
#define HOTSPOT_SSID        "ESP32GPS-Logs"
#define HOTSPOT_PASS        "gpsdlogs2026"
#define HOTSPOT_CANAL       1
#define HOTSPOT_IDLE_MS     (5UL * 60UL * 1000UL)
#define HTTP_STALL_MS       15000UL  // aborta download sem progresso por esse tempo
#define HTTP_BLOCO          2048

TinyGPS gps;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;
SdFs sd;

bool mpuDisponivel = false;
int  falhasRemountSD = 0;  // Falhas consecutivas de remount do SD

// SD compartilhado entre loopTask (flush/remount) e tarefa HTTP (leitura).
SemaphoreHandle_t sdMutex = nullptr;

// Hotspot: loopTask decide abrir/fechar; tarefa Hotspot_HTTP só atende
// enquanto hotspotAberto. downloadAtivo adia flush/remount com arquivo aberto.
WebServer server(80);
std::atomic<bool>     hotspotAberto{false};
std::atomic<bool>     downloadAtivo{false};
std::atomic<uint32_t> ultimaAtividade{0};  // millis() da última ação válida
enum DownloadEstado { DOWNLOAD_OCIOSO, DOWNLOAD_INICIADO, DOWNLOAD_CONCLUIDO, DOWNLOAD_ERRO };
std::atomic<int> downloadEstado{DOWNLOAD_OCIOSO};
// HTTP publica metricas; somente loopTask formata e imprime.
std::atomic<uint32_t> webPaginas{0}, webDownloads{0}, webOutros{0}, webUltimoMs{0};
std::atomic<uint32_t> downloadsOk{0}, downloadsErro{0}, downloadsOcupado{0};
std::atomic<uint32_t> downloadInicioMs{0}, downloadFimMs{0}, downloadTotal{0}, downloadEnviado{0};
std::atomic<const char*> downloadMotivo{nullptr};
std::atomic<uint32_t> downloadStalls{0};
std::atomic<int> downloadArquivo{0};  // 0 invalido, 1 log, 2 WiFi, 3 BLE
uint32_t loopNumero = 0, loopInicioMs = 0, loopAnteriorMs = 0;
uint32_t tempoAquisicaoMs = 0, tempoDadosMs = 0, tempoModoMs = 0, tempoSDMs = 0;
const char* faseLoop = "BOOT";
uint32_t ultimoFlushMs = 0, ultimoFlushDuracaoMs = 0;
bool temFlush = false, wifiScanFalhou = false, bleScanFalhou = false;
int ultimoDownloadEstado = DOWNLOAD_OCIOSO, ultimoDownloadArquivo = 0;
uint32_t ultimoDownloadFimMs = 0, ultimoDownloadDuracaoMs = 0, ultimoDownloadBytes = 0;
std::atomic<uint32_t> perdidos{0};
std::atomic<bool> bleFimPendente{false};
std::atomic<int> bleFimMotivo{0};
bool sdConfirmado = false;
bool flushPendente = false;
unsigned long ultimoRetryFlush = 0;
unsigned long ultimaVelocidadeMs = 0;
bool temVelocidade = false;
struct LeituraGPS {
  long lat = TinyGPS::GPS_INVALID_ANGLE, lon = TinyGPS::GPS_INVALID_ANGLE;
  unsigned long date = TinyGPS::GPS_INVALID_DATE, time = TinyGPS::GPS_INVALID_TIME;
  unsigned long posMs = 0;
  unsigned short sat = TinyGPS::GPS_INVALID_SATELLITES;
  float hdop = NAN;
  const char* direcao = "";
} leituraGPS;
float ultimaVelocidade = NAN;
bool temPosicao = false;
unsigned long ultimaPosicaoMs = 0;
unsigned long ultimaLeituraDHT = 0;
bool dhtLido = false;
unsigned long ultimoDHTValido = 0;
bool dhtValido = false;
uint32_t cicloNumero = 0;
int bleBufferizados = 0, bleDuplicados = 0;
bool watchdogAtivo = false;
bool hotspotDisponivel = false;            // false se a tarefa HTTP não subiu no boot

// Estrutura para armazenar dados médios do MPU6050
struct DadosMPU {
  float acX, acY, acZ;  // Aceleração em m/s²
  float gyX, gyY, gyZ;  // Giroscópio em rad/s
};

// Estatísticas do scan WiFi (retornadas por varrerWiFi)
struct WifiStats {
  int total;   // Redes encontradas no scan
  int novas;   // Redes novas (gravadas)
  int dup;     // Redes duplicatas (ignoradas)
};

// Nomes dos arquivos de log
const char* logFileName  = "/log.txt";
const char* wifiFileName = "/wifi.txt";
const char* bleFileName  = "/ble.txt";

// ─────────────────────────────────────────────────────────────────────────────
// Buffers em RAM comum — circulares por linha, mesma lógica pros 4 arquivos.
// Só são esvaziados quando a escrita no SD é confirmada.
char (*logBuffer)[160];  // malloc'd em setup() — ver comentário abaixo
int  logBufferHead  = 0;
int  logBufferCount = 0;

char (*wifiBuffer)[256];
int  wifiBufferHead  = 0;
int  wifiBufferCount = 0;

char (*bleBuffer)[256];
int  bleBufferHead  = 0;
int  bleBufferCount = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Estado persistente em RTC Slow Memory (.rtc.bss). As variáveis abaixo NÃO
// têm inicializador (nem "= 0") de propósito: com inicializador o C-startup
// as recopia da flash a cada soft reset (o cache voltaria a 0), sem
// inicializador ficam em .rtc.bss e sobrevivem — só apagam em power-off real.

// Cache de SSIDs já gravadas — nunca reseta enquanto a placa ficar ligada
// (sobrevive a resets do watchdog etc.). Guarda hash FNV-1a de 32 bits em
// vez da string.
RTC_DATA_ATTR uint32_t ssidCacheHash[SSID_CACHE_MAX];
RTC_DATA_ATTR int      ssidCacheCount;
RTC_DATA_ATTR uint32_t bleCacheHash[BLE_CACHE_MAX];
RTC_DATA_ATTR int      bleCacheCount;

// Contador de boot — incrementado a cada setup(). Serve pra diagnosticar
// resets espúrios (watchdog/brownout) no meio de um ciclo: se ele mudar sem
// a placa ter ficado sem energia de verdade, houve reset silencioso.
RTC_DATA_ATTR uint32_t bootCount;

bool ssidCacheCheioAvisado = false;  // evita spam do aviso de cache cheio
bool bleCacheCheioAvisado  = false;

// Estado do modo Movimento/Parado (ver defines PARKED_* acima) — RAM comum,
// não precisa sobreviver a reset (volta a MODO_MOVIMENTO, seguro por padrão).
uint8_t modoAtual   = MODO_MOVIMENTO;
unsigned long modoInicio  = 0;  // millis() de quando entrou no modo/substado atual
unsigned long lastLogAddMs = 0; // último millis() em que log.txt foi ao buffer

// Ciclo de scan WiFi+BLE simultâneo — usado tanto em Movimento (repetido a
// cada RADIO_SCAN_INTERVAL_MS) quanto em Parado/Check (disparado uma vez ao
// acordar). Substitui a antiga alternância de turnos (turnoBLE).
bool radioCicloAtivo     = false;
bool radioCicloWifiDone  = false;
bool radioCicloBleDone   = false;
unsigned long radioCicloInicio = 0;
unsigned long ultimoCicloFim   = 0;
volatile WifiStats ultimoWifiStats = {0, 0, 0};  // último resultado de varrerWiFi(), pro dashboard
// Leituras mais recentes para a pagina do hotspot (loopTask escreve, Hotspot_HTTP lê).
volatile float dashTemp = NAN, dashUmid = NAN;
volatile int   dashBle = 0;  // dispositivos BLE vistos no ciclo atual/último

// true enquanto um rádio (scan WiFi assíncrono ou scan BLE) está
// em andamento — global pra flushBuffers() poder checar antes de gravar no
// SD e evitar coincidir escrita física com o pico de corrente do rádio.
volatile bool scanEmAndamento = false;
std::atomic<bool> bleScanAtivo{false};

// Última posição/hora conhecida do GPS — usada por drenarFilaBLE() pra
// rotular os achados BLE. Escrita e lida só pela loopTask.
char lastTimeStamp[25] = "";
long lastLat = 0;
long lastLon = 0;

// Console ASCII: 80 colunas, sem escapes ANSI, legivel em qualquer monitor.
void tempoTexto(uint32_t ms, char* destino, size_t tamanho) {
  uint32_t sec = ms / 1000;
  snprintf(destino, tamanho, "%02lu:%02lu:%02lu", (unsigned long)(sec / 3600),
    (unsigned long)((sec / 60) % 60), (unsigned long)(sec % 60));
}

uint32_t tempoRestante(uint32_t agora, uint32_t inicio, uint32_t intervalo) {
  uint32_t decorrido = agora - inicio;
  return decorrido >= intervalo ? 0 : intervalo - decorrido;
}

void eventoSerial(const char* tipo, const char* formato, ...) {
  char texto[192], uptime[20];
  va_list args;
  va_start(args, formato);
  vsnprintf(texto, sizeof(texto), formato, args);
  va_end(args);
  tempoTexto(millis(), uptime, sizeof(uptime));
  Serial.printf("[%s] [%s] loop=%lu %-10s | %s\n", uptime, tipo,
    (unsigned long)loopNumero, faseLoop, texto);
}

void linhaPainel(const char* formato, ...) {
  char texto[192];
  va_list args;
  va_start(args, formato);
  vsnprintf(texto, sizeof(texto), formato, args);
  va_end(args);
  Serial.printf("| %-76.76s |\n", texto);
}

const char* nomeDownload(int arquivo) {
  switch (arquivo) {
    case 1: return "log.txt";
    case 2: return "wifi.txt";
    case 3: return "ble.txt";
    default: return "arquivo invalido";
  }
}

// Executado apenas pela tarefa HTTP. Estado terminal publicado por ultimo.
void finalizarDownload(bool sucesso) {
  downloadFimMs.store(millis());
  if (sucesso) downloadsOk.fetch_add(1);
  else downloadsErro.fetch_add(1);
  downloadEstado.store(sucesso ? DOWNLOAD_CONCLUIDO : DOWNLOAD_ERRO);
}

// ─────────────────────────────────────────────────────────────────────────────
// Hash FNV-1a de 32 bits e cache de deduplicação genéricos — usados pra
// SSID e MAC BLE, guardando 4 bytes por entrada em vez da string.
uint32_t hashString(const char* s) {
  uint32_t hash = 2166136261u;
  for (const char* p = s; *p; p++) {
    hash ^= (uint8_t)*p;
    hash *= 16777619u;
  }
  return hash;
}

bool hashJaVisto(const uint32_t* cache, int count, const char* s) {
  uint32_t h = hashString(s);
  for (int i = 0; i < count; i++) {
    if (cache[i] == h) return true;
  }
  return false;
}

bool adicionarHashCache(uint32_t* cache, int &count, int max, const char* s, bool &cheioAvisado, const char* label) {
  if (count >= max) {
    if (!cheioAvisado) eventoSerial("EVT", "Cache %s cheio; novos IDs sem cache", label);
    cheioAvisado = true;
    return false;
  }
  cache[count++] = hashString(s);
  return true;
}

bool confirmarArquivo(FsFile& file, bool escritaOk, const char* path) {
  bool syncOk = file.sync();
  bool closeOk = file.close();
  sdConfirmado = escritaOk && syncOk && closeOk;
  if (!escritaOk) eventoSerial("ERR", "SD %s etapa=escrita", path);
  if (!syncOk) eventoSerial("ERR", "SD %s etapa=sync", path);
  if (!closeOk) eventoSerial("ERR", "SD %s etapa=close", path);
  return sdConfirmado;
}

bool appendFile(const char *path, const char *message) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    sdConfirmado = false;
    eventoSerial("ERR", "SD %s etapa=open", path);
    return false;
  }
  size_t len = strlen(message);
  return confirmarArquivo(file, file.write(message, len) == len, path);
}

// Tenta remontar o cartão SD (sd.end() + sd.begin()). Usado quando uma
// escrita falha, pra recuperar de mau contato/instabilidade sem resetar
// o ESP32 inteiro.
bool remontarSD() {
  sd.end();
  delay(50);
  bool ok = sd.begin(SD_CONFIG);

  if (ok) {
    falhasRemountSD = 0;
  } else {
    falhasRemountSD++;
    if (falhasRemountSD >= SD_REMOUNT_MAX_FALHAS) {
      eventoSerial("ERR", "SD remount falhou %d vezes; reiniciando", falhasRemountSD);
      esp_restart();
    }
  }

  if (!ok) eventoSerial("ERR", "SD etapa=remount tentativa=%d", falhasRemountSD);
  return ok;
}

// Grava as linhas de um buffer circular direto no arquivo (1 open/close),
// na ordem cronológica (mais antiga primeiro), sem montar uma cópia do
// bloco inteiro em RAM antes. Retorna quantas linhas foram efetivamente
// confirmadas: count ou zero. Qualquer falha retem o lote inteiro.
int appendLinhasCirculares(const char* path, char* buf, int rows, int lineLen, int head, int count) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    sdConfirmado = false;
    eventoSerial("ERR", "SD %s etapa=open", path);
    return 0;
  }
  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    const char* linha = buf + ((size_t)((head + i) % rows) * lineLen);
    size_t len = strlen(linha);
    ok = file.write(linha, len) == len;
  }
  // Qualquer etapa incerta retém o lote inteiro; retry pode duplicar linhas.
  return confirmarArquivo(file, ok, path) ? count : 0;
}

// Adiciona uma linha a um buffer circular: se cheio, descarta a mais antiga
// pra abrir espaço (preserva sempre o trecho mais recente do percurso).
void adicionarLinhaCircular(char* buf, int rows, int lineLen, int &head, int &count, const char* linha) {
  if (strlen(linha) >= (size_t)lineLen) { perdidos.fetch_add(1); return; }
  int idx;
  if (count < rows) {
    idx = (head + count) % rows;
    count++;
  } else {
    perdidos.fetch_add(1);
    idx = head;                // sobrescreve a mais antiga
    head = (head + 1) % rows;  // avança o início, descartando-a
  }
  char* destino = buf + ((size_t)idx * lineLen);
  strncpy(destino, linha, lineLen - 1);
  destino[lineLen - 1] = '\0';
}

// Flush: grava os 3 buffers no SD. Retorna true só se todos os buffers com
// dados foram confirmados. Em caso de falha, o lote inteiro fica
// retido no buffer circular e o SD e remontado, para a
// próxima tentativa.
bool flushBuffers() {
  flushPendente = true;
  ultimoRetryFlush = millis();
  // Não grava no SD com rádio (WiFi ou BLE) em andamento — evita coincidir
  // escrita física com o pico de corrente do scan.
  if (scanEmAndamento || bleScanAtivo) {
    return false;
  }

  // Download com arquivo aberto: não grava nem remonta o SD por baixo dele.
  // Checa de novo já com o mutex — downloadAtivo é ligado sob sdMutex.
  if (downloadAtivo) {
    return false;
  }
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (downloadAtivo) {
    xSemaphoreGive(sdMutex);
    return false;
  }

  eventoSerial("EVT", "SD flush buf=%d", logBufferCount + wifiBufferCount + bleBufferCount);
  uint32_t inicioFlush = millis();
  bool falhaAlgum = false;

  struct BufferFlush {
    const char* path;
    char* buf;
    int rows;
    int lineLen;
    int* head;
    int* count;
    const char* label;
  };

  BufferFlush buffers[] = {
    { logFileName,  &logBuffer[0][0],  LOG_BUFFER_MAX,  160, &logBufferHead,  &logBufferCount,  "log.txt"  },
    { wifiFileName, &wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, &wifiBufferHead, &wifiBufferCount, "wifi.txt" },
    { bleFileName,  &bleBuffer[0][0],  BLE_BUFFER_MAX,  256, &bleBufferHead,  &bleBufferCount,  "ble.txt"  },
  };

  for (auto &b : buffers) {
    if (watchdogAtivo) esp_task_wdt_reset();
    if (*b.count > 0) {

      int escritas = appendLinhasCirculares(b.path, b.buf, b.rows, b.lineLen, *b.head, *b.count);
      *b.head   = (*b.head + escritas) % b.rows;
      *b.count -= escritas;
      if (escritas > 0 && *b.count == 0) {
        eventoSerial("OK ", "SD %s lote confirmado=%d", b.label, escritas);
      } else {
        eventoSerial("ERR", "SD %s lote retido=%d", b.label, *b.count);
        falhaAlgum = true;
      }
    }
  }

  temFlush = true;
  ultimoFlushMs = millis();
  ultimoFlushDuracaoMs = ultimoFlushMs - inicioFlush;
  if (falhaAlgum) {
    sdConfirmado = false;
    remontarSD();  // tenta recuperar pro próximo ciclo
    xSemaphoreGive(sdMutex);
    return false;
  }
  xSemaphoreGive(sdMutex);

  flushPendente = false;
  eventoSerial("OK ", "SD flush confirmado");
  return true;
}

// Escreve cabeçalho CSV em log.txt caso o arquivo ainda não exista
// (wifi.txt/ble.txt/bt.txt seguem o mesmo padrão de wifi.txt já existente:
// sem cabeçalho, formato CSV implícito documentado no README).
void inicializarArquivoLog() {
  if (!sd.exists(logFileName)) {
    const char* cabecalho =
      "data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht,"
      " ac_x, ac_y, ac_z, gy_x, gy_y, gy_z\n";
    if (appendFile(logFileName, cabecalho)) eventoSerial("OK ", "Cabecalho CSV criado");
  }
}

// Converte o tipo de criptografia WiFi para string legível
const char* obterTipoCriptografia(wifi_auth_mode_t encryptionType) {
  switch (encryptionType) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA+WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-EAP";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2+WPA3";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI";
    default:                        return "unknown";
  }
}

// Nome legível do modo atual, pro Serial/dashboard
const char* modoNome(uint8_t modo) {
  switch (modo) {
    case MODO_MOVIMENTO:    return "MOVIMENTO";
    case MODO_PARADO_SONO:  return "PARADO (sono)";
    case MODO_PARADO_CHECK: return "PARADO (check)";
    case MODO_PARADO_HOTSPOT: return "PARADO (hotspot)";
    default:                return "?";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Exibe dashboard ASCII no Serial Monitor
void lerDHT() {
  unsigned long agora = millis();
  if (dhtLido && agora - ultimaLeituraDHT < 2000) return;
  dhtLido = true;
  ultimaLeituraDHT = agora;
  dashUmid = dht.readHumidity();
  dashTemp = dht.readTemperature();
  if (isfinite(dashTemp) && isfinite(dashUmid)) {
    ultimoDHTValido = agora;
    dhtValido = true;
  }
}

void exibirDashboard(DadosMPU mediaMPU) {
  static unsigned long ultimoResumo = 0;
  unsigned long agora = millis();
  // Mudo durante download; espacado com hotspot aberto (nao disputa CPU/heap com a tarefa HTTP).
  if (downloadEstado.load() == DOWNLOAD_INICIADO) return;
  if (agora - ultimoResumo < (hotspotAberto.load() ? 30000UL : 5000UL)) return;
  ultimoResumo = agora;
  char uptime[20], restante[20] = "-";
  tempoTexto(agora, uptime, sizeof(uptime));
  bool hot = hotspotAberto.load();
  if (hot) tempoTexto(tempoRestante(millis(), ultimaAtividade.load(), HOTSPOT_IDLE_MS), restante, sizeof(restante));
  bool vel = temVelocidade && agora - ultimaVelocidadeMs < 3000;
  char pos[40] = "sem fix", dht[24] = "DHT --";
  if (temPosicao) snprintf(pos, sizeof(pos), "%.5f,%.5f", lastLat / 1000000.0, lastLon / 1000000.0);
  if (isfinite(dashTemp) && isfinite(dashUmid)) snprintf(dht, sizeof(dht), "%.1fC %.0f%%", dashTemp, dashUmid);
  Serial.printf("[%s] %s | %.1f km/h | loop #%lu %lums\n", uptime, modoNome(modoAtual),
    vel ? ultimaVelocidade : NAN, (unsigned long)loopNumero, (unsigned long)loopAnteriorMs);
  Serial.printf("  GPS %s | %s | az=%.1f\n", pos, dht, mediaMPU.acZ);
  Serial.printf("  RF #%lu wifi=%d ble=%d | SD %s %d/%d %d/%d %d/%d | heap=%luKB max=%luKB perdas=%lu\n",
    (unsigned long)cicloNumero, ultimoWifiStats.total, (int)dashBle, sdConfirmado ? "ok" : "INCERTO",
    logBufferCount, LOG_BUFFER_MAX, wifiBufferCount, WIFI_BUFFER_MAX, bleBufferCount, BLE_BUFFER_MAX,
    (unsigned long)ESP.getFreeHeap() / 1024, (unsigned long)ESP.getMaxAllocHeap() / 1024, (unsigned long)perdidos.load());
  if (hot) Serial.printf("  WEB %s clientes=%u fecha %s | pag=%lu dl=%lu ok=%lu erro=%lu\n", HOTSPOT_SSID,
    WiFi.softAPgetStationNum(), restante, (unsigned long)webPaginas.load(), (unsigned long)webDownloads.load(),
    (unsigned long)downloadsOk.load(), (unsigned long)downloadsErro.load());
}

// ─────────────────────────────────────────────────────────────────────────────
// Faz scan de redes WiFi de forma assíncrona (não-bloqueante)
// Retorna as estatísticas do scan anterior se um novo estiver em andamento.
WifiStats varrerWiFi(const char* timeStamp, long lat, long lon) {
  static WifiStats lastStats = {0, 0, 0};

  // Se não há scan rodando, inicia um novo de forma assíncrona (true, true para show_hidden e passive)
  if (!scanEmAndamento) {
    int16_t inicio = WiFi.scanNetworks(true, true);
    if (inicio == WIFI_SCAN_FAILED) {
      eventoSerial("ERR", "WiFi scan inicio falhou");
      wifiScanFalhou = true;
      radioCicloWifiDone = true;
      return lastStats;
    }
    scanEmAndamento = true;
    if (inicio == WIFI_SCAN_RUNNING) return lastStats; // Retorna último resultado enquanto varre
  }

  // Verifica se o scan assíncrono terminou
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    eventoSerial("ERR", "WiFi scan falhou");
    wifiScanFalhou = true;
    radioCicloWifiDone = true;
    scanEmAndamento = false; // Falhou, permite tentar de novo
    return lastStats;
  }

  if (n == WIFI_SCAN_RUNNING) {
    return lastStats; // Ainda processando
  }

  // Se chegou aqui, temos novos resultados (n >= 0)
  WifiStats stats = {n, 0, 0};

  for (int i = 0; i < n; ++i) {
    String ssidStr = WiFi.SSID(i);
    const char* ssid = ssidStr.c_str();

    // Deduplicação: ignora SSIDs já gravadas (cache nunca reseta)
    if (hashJaVisto(ssidCacheHash, ssidCacheCount, ssid)) {
      stats.dup++;
      continue;
    }
    adicionarHashCache(ssidCacheHash, ssidCacheCount, SSID_CACHE_MAX, ssid, ssidCacheCheioAvisado, "SSID");

    // Formata dados da rede
    char dadosWifi[256];
    char pos[48] = " , ";
    if (temPosicao) snprintf(pos, sizeof(pos), "%ld, %ld", lat, lon);
    snprintf(dadosWifi, sizeof(dadosWifi), "%s, %s, %s, %ld, %d, %s\n",
             timeStamp, pos, ssid, WiFi.RSSI(i),
             (int)WiFi.channel(i), obterTipoCriptografia(WiFi.encryptionType(i)));

    // Acumula no buffer circular WiFi (descarta a linha mais antiga se cheio)
    adicionarLinhaCircular(&wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount, dadosWifi);
    stats.novas++;
  }

  WiFi.scanDelete();
  scanEmAndamento = false; // Pronto para o próximo ciclo
  radioCicloWifiDone = true;
  lastStats = stats;
  return stats;
}

// ═════════════════════════════════════════════════════════════════════════════
// BLE — NimBLE-Arduino v2.x, scan ativo com deduplicação por MAC
// ═════════════════════════════════════════════════════════════════════════════
#define BLE_SCAN_DURATION_MS       5000   // Duração do scan BLE de cada ciclo
#define BLE_QUEUE_DEPTH            10
#define BLE_DEVICE_NAME_LEN        65

// BLE Device Record — alocado no heap via malloc, transportado por fila.
// Campos ricos do PoC (manufacturer data, service UUIDs, appearance) foram
// deliberadamente descartados: infla o CSV sem consumidor hoje.
struct BLEDeviceRecord {
  char     address[18];
  char     name[BLE_DEVICE_NAME_LEN];
  bool     hasName;
  int8_t   rssi;
  int8_t   txPower;
  bool     hasTxPower;
};

QueueHandle_t bleQueue = nullptr;
NimBLEScan*   pBLEScan = nullptr;

// Drena a fila BLE na loopTask (dona única do bleBuffer — sem lock):
// dedup persistente por hash e acumula no bleBuffer. Não bloqueia.
void drenarFilaBLE() {
  BLEDeviceRecord* rec = nullptr;
  while (xQueueReceive(bleQueue, &rec, 0) == pdTRUE) {
    dashBle = dashBle + 1;
    if (!hashJaVisto(bleCacheHash, bleCacheCount, rec->address)) {
      adicionarHashCache(bleCacheHash, bleCacheCount, BLE_CACHE_MAX, rec->address, bleCacheCheioAvisado, "BLE");

      bleBufferizados++;
      char pos[48] = " , ";
      if (temPosicao) snprintf(pos, sizeof(pos), "%ld, %ld", lastLat, lastLon);
      char dadosBLE[256];
      if (rec->hasTxPower) {
        snprintf(dadosBLE, sizeof(dadosBLE), "%s, %s, %s, %s, %d, %d\n",
                 lastTimeStamp, pos, rec->address,
                 rec->hasName ? rec->name : "", (int)rec->rssi, (int)rec->txPower);
      } else {
        snprintf(dadosBLE, sizeof(dadosBLE), "%s, %s, %s, %s, %d, \n",
                 lastTimeStamp, pos, rec->address,
                 rec->hasName ? rec->name : "", (int)rec->rssi);
      }
      adicionarLinhaCircular(&bleBuffer[0][0], BLE_BUFFER_MAX, 256, bleBufferHead, bleBufferCount, dadosBLE);
    } else bleDuplicados++;
    free(rec);
  }
}

// Callbacks do Scanner BLE — dedup dentro do scan fica com o filtro de
// duplicatas nativo do NimBLE (filter_duplicates, ligado por padrão).
class BLEScanCallbacks : public NimBLEScanCallbacks {
  void onScanEnd(const NimBLEScanResults&, int reason) override {
    bleFimMotivo.store(reason);
    bleScanAtivo = false;
    bleFimPendente.store(true);
  }


  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string addrStr = dev->getAddress().toString();

    BLEDeviceRecord* rec = (BLEDeviceRecord*)malloc(sizeof(BLEDeviceRecord));
    if (rec == nullptr) { perdidos.fetch_add(1); return; }
    memset(rec, 0, sizeof(BLEDeviceRecord));

    rec->rssi      = dev->getRSSI();
    strncpy(rec->address, addrStr.c_str(), sizeof(rec->address) - 1);

    if (dev->haveName()) {
      rec->hasName = true;
      strncpy(rec->name, dev->getName().c_str(), sizeof(rec->name) - 1);
    }
    if (dev->haveTXPower()) {
      rec->hasTxPower = true;
      rec->txPower    = dev->getTXPower();
    }

    if (xQueueSend(bleQueue, &rec, 0) != pdTRUE) {
      perdidos.fetch_add(1);
      free(rec); // Queue cheia: descarta sem bloquear o BLE stack
    }
  }

} bleScanCallbacks;

// Inicializa o controller BLE e a fila uma única vez no boot.
// Não inicia scan aqui — quem liga/desliga o scan é o state machine de modo.
void bleIniciar();
void setupBLE() {
  bleQueue = xQueueCreate(BLE_QUEUE_DEPTH, sizeof(BLEDeviceRecord*));
  if (bleQueue == nullptr) {
    perdidos.fetch_add(1);
    eventoSerial("ERR", "Falha ao criar bleQueue");
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  bleIniciar();
}

// Sobe o controller BLE. Chamado no boot e ao fechar o hotspot (bleParar()).
void bleIniciar() {
  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&bleScanCallbacks, false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(100);
  pBLEScan->setMaxResults(0);
}

// Derruba o controller BLE enquanto o hotspot esta aberto: devolve ~dezenas de
// KB de heap para o lwIP/HTTP. Chamar so com scan BLE encerrado.
void bleParar() {
  if (!pBLEScan) return;
  NimBLEDevice::deinit(true);
  pBLEScan = nullptr;
}

// Liga o scan BLE por BLE_SCAN_DURATION_MS (termina sozinho)
void bleScanLigar() {
  bleScanAtivo = true;
  if (!pBLEScan) bleIniciar();
  if (!pBLEScan->start(BLE_SCAN_DURATION_MS)) {
    bleScanAtivo = false;
    bleScanFalhou = true;
    radioCicloBleDone = true;
    eventoSerial("ERR", "BLE scan inicio falhou");
  }
}

// Garante o scan BLE parado (no-op se já terminou sozinho)
void bleScanDesligar() {
  if (pBLEScan) pBLEScan->stop();
  bleScanAtivo = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// State machine Movimento/Parado — sem restart, rádios nunca desligam, só
// variam a cadência de scan (economia é só de cartão SD, não de energia)
// ═════════════════════════════════════════════════════════════════════════════

// Dispara um novo ciclo de scan WiFi+BLE simultâneo. WiFi é disparado pelo
// scanWifiAtivo em processarDadosGPS(); BLE começa aqui.
void iniciarCicloRadio() {
  cicloNumero++;
  bleBufferizados = bleDuplicados = 0;
  bleFimPendente = false;
  wifiScanFalhou = bleScanFalhou = false;
  eventoSerial("EVT", "RADIO ciclo #%lu iniciado: WiFi + BLE", (unsigned long)cicloNumero);
  radioCicloAtivo    = true;
  radioCicloWifiDone = false;
  radioCicloBleDone  = false;
  radioCicloInicio   = millis();
  dashBle            = 0;
  scanEmAndamento    = false;
  bleScanLigar();
}

void entrarModoParadoSono();
void entrarModoParadoHotspot();

// Chamado a cada volta do loop enquanto radioCicloAtivo — fecha o ciclo quando
// WiFi e BLE terminaram e, se estava no check parado, abre o hotspot.
void atualizarCicloRadio() {
  if (!radioCicloAtivo) return;
  unsigned long agora = millis();

  if (bleFimPendente.exchange(false)) {
    radioCicloBleDone = true;
    int motivo = bleFimMotivo.load();
    bleScanFalhou = motivo != 0;
    if (motivo != 0) eventoSerial("ERR", "BLE scan terminou motivo=%d", motivo);
    drenarFilaBLE();
  }

  if (radioCicloWifiDone && radioCicloBleDone) {
    radioCicloAtivo = false;
    ultimoCicloFim = agora;
    eventoSerial(wifiScanFalhou || bleScanFalhou ? "ERR" : "OK ",
      "RADIO ciclo #%lu encerrado em %lums | WiFi=%s BLE=%s", (unsigned long)cicloNumero,
      (unsigned long)(agora - radioCicloInicio), wifiScanFalhou ? "falhou" : "concluido", bleScanFalhou ? "falhou" : "concluido");
    if (modoAtual == MODO_PARADO_CHECK) {
      entrarModoParadoHotspot();
    }
  }
}

// Derruba o AP. Um download em curso percebe hotspotAberto=false (ou o
// write falha) e fecha o arquivo na própria tarefa HTTP.
void fecharHotspot() {
  if (!hotspotAberto) return;
  hotspotAberto = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  eventoSerial("EVT", "Hotspot fechado.");
}

void entrarModoMovimento() {
  if (modoAtual == MODO_PARADO_HOTSPOT) {
    eventoSerial("EVT", "Movimento: fechando hotspot.");
    fecharHotspot();
  }
  modoAtual = MODO_MOVIMENTO;
  modoInicio = millis();
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
}

// Só é chamado com o ciclo de rádio já fechado (vem do hotspot, que vem do
// fim do check), então não há scan a interromper aqui.
void entrarModoParadoSono() {
  modoAtual = MODO_PARADO_SONO;
  modoInicio = millis();
  eventoSerial("EVT", "Parado: dormindo (sem scan) por 5 min...");

  // Grava tudo no SD antes do período sem scan (momento seguro pro cartão).
  // Se falhar, os dados ficam no buffer circular e o flush por capacidade
  // tenta de novo.
  flushBuffers();
}

void entrarModoParadoCheck() {
  modoAtual = MODO_PARADO_CHECK;
  modoInicio = millis();
  eventoSerial("EVT", "Acordou: checando WiFi + BLE juntos...");
  iniciarCicloRadio();
}

// Abre o AP de download após o check. Scans já terminaram; grava os buffers
// antes (falha não impede a página — buffers ficam retidos). Falha no AP:
// dorme e tenta de novo no próximo ciclo.
void entrarModoParadoHotspot() {
  if (!hotspotDisponivel) {
    entrarModoParadoSono();
    return;
  }
  if (scanEmAndamento || bleScanAtivo || radioCicloAtivo) return;
  flushBuffers();
  drenarFilaBLE();
  bleParar();
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);
  bool ok = WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) &&
            WiFi.softAP(HOTSPOT_SSID, HOTSPOT_PASS, HOTSPOT_CANAL, 0, 1);
  if (!ok) {
    eventoSerial("ERR", "falha ao abrir hotspot. Tenta de novo no proximo check.");
    WiFi.mode(WIFI_STA);
    entrarModoParadoSono();
    return;
  }
  modoAtual  = MODO_PARADO_HOTSPOT;
  modoInicio = millis();
  ultimaAtividade = millis();
  hotspotAberto = true;
  eventoSerial("EVT", "Hotspot aberto: SSID %s  http://%s/  fecha apos %lus sem atividade",
                HOTSPOT_SSID, WiFi.softAPIP().toString().c_str(), HOTSPOT_IDLE_MS / 1000);
}

// Parte temporal da máquina de estados — roda a cada volta do loop, com ou
// sem fix novo (GPS mudo não pode travar sono/hotspot/ciclo de scan).
// Achados WiFi usam a última posição conhecida.
void servicoModo() {
  if (radioCicloAtivo && !radioCicloWifiDone) {
    WifiStats stats = varrerWiFi(lastTimeStamp, lastLat, lastLon);
    ultimoWifiStats.total = stats.total;
    ultimoWifiStats.novas = stats.novas;
    ultimoWifiStats.dup = stats.dup;
  }
  atualizarCicloRadio();

  switch (modoAtual) {
    case MODO_PARADO_SONO:
      if (millis() - modoInicio >= PARKED_SLEEP_MS) {
        entrarModoParadoCheck();
      }
      break;

    case MODO_PARADO_HOTSPOT: {
      // Lê ultimaAtividade antes de millis(): a tarefa HTTP pode renová-la
      // entre as duas leituras e a subtração daria underflow (fecha à toa).
      uint32_t ult = ultimaAtividade;
      if (!downloadAtivo && (millis() - ult >= HOTSPOT_IDLE_MS)) {
        eventoSerial("EVT", "Hotspot: 5 min sem atividade.");
        fecharHotspot();
        entrarModoParadoSono();
      }
      break;
    }
  }
}

// Transições por velocidade — chamado a cada fix de GPS
void atualizarModo(float kmh) {
  // Histerese: parado -> movimento exige MOVING_KMH_THRESHOLD por
  // MOVING_DEBOUNCE_FIXES fixes seguidos (ignora picos de deriva do GPS
  // parado); movimento -> parado usa PARKED_KMH_THRESHOLD direto.
  static int fixesRapidos = 0;
  fixesRapidos = (kmh > MOVING_KMH_THRESHOLD) ? fixesRapidos + 1 : 0;

  bool parado;
  if (modoAtual == MODO_MOVIMENTO) {
    parado = kmh <= PARKED_KMH_THRESHOLD;
  } else {
    parado = fixesRapidos < MOVING_DEBOUNCE_FIXES;
  }

  if (!parado) {
    if (modoAtual != MODO_MOVIMENTO) entrarModoMovimento();
    // Ciclo em movimento só dispara com fix (achados precisam de posição
    // atual); o fechamento do ciclo fica com servicoModo().
    if (!radioCicloAtivo && (millis() - ultimoCicloFim >= RADIO_SCAN_INTERVAL_MS)) {
      iniciarCicloRadio();
    }
    return;
  }
  if (modoAtual == MODO_MOVIMENTO) {
    // Acabou de parar: roda 1 ciclo WiFi+BLE completo antes do hotspot
    // (aproveita o ciclo já aberto, se houver). Ao fechar, atualizarCicloRadio()
    // abre o hotspot (que faz o flush).
    modoAtual  = MODO_PARADO_CHECK;
    modoInicio = millis();
    eventoSerial("EVT", "Parou: ciclo WiFi+BLE final antes do hotspot...");
    if (!radioCicloAtivo) iniciarCicloRadio();
  }
  // Demais transições parado (sono/check/hotspot) são temporais: servicoModo().
}

// ─────────────────────────────────────────────────────────────────────────────
// Processa dados do GPS e DHT22, exibe no Serial e acumula no buffer
// Recebe a média das leituras do MPU6050 coletadas durante o ciclo GPS
// TinyGPS nao expoe a idade da velocidade. Inspeciona somente sentencas
// aprovadas pelo checksum do parser; GGA nunca renova velocidade RMC.
bool numeroNMEA(const char* campo, bool decimal = true) {
  bool digito = false, ponto = false;
  for (const char* p = campo; *p; ++p) {
    if (*p >= '0' && *p <= '9') digito = true;
    else if (*p == '.' && decimal && !ponto) ponto = true;
    else return false;
  }
  return digito;
}

bool receberGPS(char c) {
  static char nmea[128];
  static size_t len = 0;
  static bool overflow = false;
  if (c == '$') { len = 0; overflow = false; }
  if (len < sizeof(nmea) - 1) nmea[len++] = c;
  else overflow = true;
  bool valido = gps.encode(c);
  if (!valido || overflow) return false;
  nmea[len] = '\0';
  char* campos[20];
  int count = 0;
  campos[count++] = nmea;
  for (char* t = nmea; *t && count < 20; ++t) {
    if (*t == ',' || *t == '*') { *t = '\0'; campos[count++] = t + 1; }
  }
  bool rmc = strcmp(campos[0], "$GPRMC") == 0;
  bool gga = strcmp(campos[0], "$GPGGA") == 0;
  bool posValida = rmc && count > 9 && *campos[1] && *campos[3] &&
    *campos[4] && *campos[5] && *campos[6] && *campos[9];
  posValida = posValida || (gga && count > 8 && *campos[1] && *campos[2] &&
    *campos[3] && *campos[4] && *campos[5]);
  if (!posValida) return false;
  int latitude = rmc ? 3 : 2, longitude = rmc ? 5 : 4;
  if (!numeroNMEA(campos[1]) || strlen(campos[1]) < 6 ||
      !numeroNMEA(campos[latitude]) || !numeroNMEA(campos[longitude]) ||
      (strcmp(campos[latitude + 1], "N") && strcmp(campos[latitude + 1], "S")) ||
      (strcmp(campos[longitude + 1], "E") && strcmp(campos[longitude + 1], "W")) ||
      (rmc && (!numeroNMEA(campos[9], false) || strlen(campos[9]) != 6))) return false;
  unsigned long idade;
  gps.get_position(&leituraGPS.lat, &leituraGPS.lon, &idade);
  leituraGPS.posMs = millis() - idade;
  gps.get_datetime(&leituraGPS.date, &leituraGPS.time);
  if (rmc) {
    leituraGPS.direcao = numeroNMEA(campos[8]) && gps.f_course() < 360 && gps.course() != TinyGPS::GPS_INVALID_ANGLE ? TinyGPS::cardinal(gps.f_course()) : "";
    char* end = nullptr;
    double knots = strtod(campos[7], &end);
    if (numeroNMEA(campos[7]) && *end == '\0' && isfinite(knots) && knots >= 0) {
      ultimaVelocidade = knots * 1.852;
      ultimaVelocidadeMs = millis();
      temVelocidade = true;
      atualizarModo(ultimaVelocidade);
    }
  } else {
    leituraGPS.sat = numeroNMEA(campos[7], false) && strtoul(campos[7], nullptr, 10) < TinyGPS::GPS_INVALID_SATELLITES ? gps.satellites() : TinyGPS::GPS_INVALID_SATELLITES;
    leituraGPS.hdop = numeroNMEA(campos[8]) ? gps.hdop() / 100.0f : NAN;
  }
  return true;
}

void campoFloat(char* destino, size_t tamanho, float valor, int casas) {
  destino[0] = '\0';
  if (isfinite(valor)) snprintf(destino, tamanho, "%.*f", casas, valor);
}

void processarDadosGPS(DadosMPU mediaMPU) {
  long lat = leituraGPS.lat, lon = leituraGPS.lon;
  unsigned long date = leituraGPS.date, time = leituraGPS.time;
  if (lat == TinyGPS::GPS_INVALID_ANGLE ||
      lon == TinyGPS::GPS_INVALID_ANGLE || labs(lat) > 90000000L || labs(lon) > 180000000L) return;
  lastLat = lat;
  lastLon = lon;
  temPosicao = true;
  ultimaPosicaoMs = leituraGPS.posMs;

  int dia = date / 10000, mes = (date / 100) % 100, ano = 2000 + date % 100;
  int hora = time / 1000000, minuto = (time / 10000) % 100, segundo = (time / 100) % 100;
  if (date == TinyGPS::GPS_INVALID_DATE || time == TinyGPS::GPS_INVALID_TIME ||
      mes < 1 || mes > 12 || dia < 1 ||
      hora > 23 || minuto > 59 || segundo > 59) return;
  const int diasMes[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  int limite = diasMes[mes - 1] + (mes == 2 && (ano % 4 == 0 && (ano % 100 != 0 || ano % 400 == 0)));
  if (dia > limite) return;
  struct tm t = {};
  t.tm_year = ano - 1900; t.tm_mon = mes - 1; t.tm_mday = dia;
  t.tm_hour = hora - 3; t.tm_min = minuto; t.tm_sec = segundo;
  t.tm_isdst = 0;
  // TZ definido como UTC no boot; o deslocamento explicito sempre e UTC-3.
  if (mktime(&t) == (time_t)-1) return;
  if (!strftime(lastTimeStamp, sizeof(lastTimeStamp), "%d/%m/%Y %H:%M:%S", &t)) return;

  char sat[8] = "", hdop[16], kmh[16], umid[16], temp[16];
  if (leituraGPS.sat != TinyGPS::GPS_INVALID_SATELLITES) snprintf(sat, sizeof(sat), "%u", leituraGPS.sat);
  campoFloat(hdop, sizeof(hdop), leituraGPS.hdop, 2);
  campoFloat(kmh, sizeof(kmh), temVelocidade && millis() - ultimaVelocidadeMs < 3000 ? ultimaVelocidade : NAN, 2);
  campoFloat(umid, sizeof(umid), dashUmid, 1);
  campoFloat(temp, sizeof(temp), dashTemp, 1);
  const char* direcao = leituraGPS.direcao;
  char imu[112] = ", , , , , ";
  if (mpuDisponivel) {
    char valores[6][16];
    float dados[] = {mediaMPU.acX, mediaMPU.acY, mediaMPU.acZ, mediaMPU.gyX, mediaMPU.gyY, mediaMPU.gyZ};
    for (int i = 0; i < 6; ++i) campoFloat(valores[i], sizeof(valores[i]), dados[i], 2);
    snprintf(imu, sizeof(imu), "%s, %s, %s, %s, %s, %s", valores[0], valores[1], valores[2], valores[3], valores[4], valores[5]);
  }
  char logData[160];
  int len = snprintf(logData, sizeof(logData), "%s, %ld, %ld, %s, %s, %s, %s, %s, %s, %s\n",
    lastTimeStamp, lat, lon, sat, hdop, kmh, direcao, umid, temp, imu);
  unsigned long cadencia = modoAtual == MODO_MOVIMENTO ? LOG_ADD_MOVING_MS : LOG_ADD_PARKED_MS;
  if (millis() - lastLogAddMs >= cadencia) {
    if (len < 0 || len >= (int)sizeof(logData)) perdidos.fetch_add(1);
    else adicionarLinhaCircular(&logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount, logData);
    lastLogAddMs = millis();
  }
}

void imprimirAcessosWeb() {
  static uint32_t paginasAntes = 0, downloadsAntes = 0, outrosAntes = 0;
  uint32_t paginas = webPaginas.load(), downloads = webDownloads.load(), outros = webOutros.load();
  if (paginas != paginasAntes || downloads != downloadsAntes || outros != outrosAntes) {
    eventoSerial("EVT", "WEB acesso: pagina +%lu | download +%lu | outros/404 +%lu",
      (unsigned long)(paginas - paginasAntes), (unsigned long)(downloads - downloadsAntes), (unsigned long)(outros - outrosAntes));
    paginasAntes = paginas; downloadsAntes = downloads; outrosAntes = outros;
  }
}

void imprimirDownload() {
  static bool iniciadoImpresso = false;
  int estado = downloadEstado.load();
  if (estado == DOWNLOAD_OCIOSO) return;
  if (!iniciadoImpresso) {
    eventoSerial("EVT", "Download iniciado: %s", nomeDownload(downloadArquivo.load()));
    iniciadoImpresso = true;
  }
  if (estado == DOWNLOAD_CONCLUIDO || estado == DOWNLOAD_ERRO) {
    ultimoDownloadEstado = estado;
    ultimoDownloadArquivo = downloadArquivo.load();
    ultimoDownloadFimMs = downloadFimMs.load();
    ultimoDownloadDuracaoMs = ultimoDownloadFimMs - downloadInicioMs.load();
    ultimoDownloadBytes = downloadEnviado.load();
    const char* motivo = downloadMotivo.load();
    eventoSerial(estado == DOWNLOAD_CONCLUIDO ? "OK " : "ERR", "Download %s: %s | %lu/%lu bytes em %lums | motivo=%s stalls=%lu heap=%lu maxblk=%lu",
      estado == DOWNLOAD_CONCLUIDO ? "concluido" : "erro", nomeDownload(ultimoDownloadArquivo),
      (unsigned long)ultimoDownloadBytes, (unsigned long)downloadTotal.load(), (unsigned long)ultimoDownloadDuracaoMs,
      motivo ? motivo : "-", (unsigned long)downloadStalls.load(), (unsigned long)ESP.getFreeHeap(), (unsigned long)ESP.getMaxAllocHeap());
    iniciadoImpresso = false;
    downloadEstado.store(DOWNLOAD_OCIOSO);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
// Hotspot HTTP — tarefa Hotspot_HTTP (core 1) é dona do WebServer e do FsFile
// do download. SD sempre sob sdMutex; nenhum lock durante envio pela rede.
// ═════════════════════════════════════════════════════════════════════════════
static uint8_t httpBuf[HTTP_BLOCO];

// Nome recebido -> caminho constante. Nunca concatena entrada ao caminho SD.
const char* caminhoPermitido(const String& nome) {
  if (nome == "log.txt")  return logFileName;
  if (nome == "wifi.txt") return wifiFileName;
  if (nome == "ble.txt")  return bleFileName;
  return nullptr;
}

String tamanhoLegivel(uint64_t b) {
  char s[48];
  if (b >= 1048576ULL)  snprintf(s, sizeof(s), "%.1f MB (%llu bytes)", b / 1048576.0, b);
  else if (b >= 1024)   snprintf(s, sizeof(s), "%.1f KB (%llu bytes)", b / 1024.0, b);
  else                  snprintf(s, sizeof(s), "%llu bytes", b);
  return String(s);
}

// CSS fica na flash; <meter>/<progress> fazem as barras (sem JS, sem recursos externos).
static const char PAGINA_CSS[] = R"CSS(
:root{color-scheme:light dark;--bg:#edf0ee;--card:#fff;--ink:#13202a;--mut:#5d6b74;--line:#d5dcd9;--wifi:#0b7a75;--ble:#4b3fc4;--heat:#d2452b}
@media(prefers-color-scheme:dark){:root{--bg:#0f1a21;--card:#17252e;--ink:#e6eeea;--mut:#8fa1aa;--line:#26373f;--wifi:#3fc7bf;--ble:#9a90ff;--heat:#ff7a5c}}
*{box-sizing:border-box}
body{margin:0;padding:16px;background:var(--bg);color:var(--ink);font:16px/1.4 system-ui,sans-serif}
main{max-width:560px;margin:0 auto}
h1{font-size:1.2rem;margin:4px 0 2px}
.sub{margin:0 0 16px;color:var(--mut);font-size:.85rem}
.grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:14px}
.lbl{display:block;font-size:.7rem;letter-spacing:.08em;text-transform:uppercase;color:var(--mut)}
.num{display:block;margin:4px 0 8px;font:600 2rem/1 ui-monospace,monospace}
.num small{font-size:.9rem;color:var(--mut);font-weight:400}
progress,meter{width:100%;height:10px}
progress{accent-color:var(--wifi)}.b progress{accent-color:var(--ble)}
.t .num{color:var(--heat)}.t meter,.h meter{accent-color:var(--heat)}.h meter{accent-color:var(--wifi)}
h2{font-size:.75rem;letter-spacing:.08em;text-transform:uppercase;color:var(--mut);margin:24px 0 8px}
.file{display:flex;align-items:center;gap:12px;background:var(--card);border:1px solid var(--line);border-radius:12px;padding:12px 14px;margin-bottom:8px}
.file b{flex:1;font:600 .95rem ui-monospace,monospace}.file span{color:var(--mut);font-size:.8rem}
a.btn{padding:8px 14px;border-radius:8px;background:var(--wifi);color:#fff;text-decoration:none;font-weight:600}
@media(prefers-color-scheme:dark){a.btn{color:#06161a}}
button.del{padding:8px 14px;border:0;border-radius:8px;background:var(--heat);color:#fff;font:600 1rem system-ui,sans-serif;cursor:pointer}
a.btn:focus-visible,button.del:focus-visible{outline:3px solid var(--ink);outline-offset:2px}
.foot{margin-top:16px;color:var(--mut);font-size:.8rem}
@media(max-width:380px){.grid{grid-template-columns:1fr}}
)CSS";

// GET / — sensores (valores do último scan) + lista dos logs. HTML5 + CSS, sem JS.
// Renova a janela. "Atualizar" recarrega a página.
void httpRaiz() {
  webUltimoMs.store(millis());
  webPaginas.fetch_add(1);
  ultimaAtividade = millis();
  const char* nomes[] = { "log.txt", "wifi.txt", "ble.txt" };
  float t = dashTemp, h = dashUmid;
  char v[16];

  // Envio em blocos (chunked): evita um String de varios KB com heap baixo.
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/html; charset=utf-8", "");
  String html = F("<!doctype html><html lang=pt-BR><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Logs ESP32 GPS</title><style>");
  server.sendContent(html); html = "";
  server.sendContent(PAGINA_CSS);
  html += F("</style></head><body><main><h1>Logs ESP32 GPS</h1>"
            "<p class=sub>Último scan WiFi+BLE e última leitura do DHT22</p><section class=grid>");

  snprintf(v, sizeof(v), "%d", ultimoWifiStats.total);
  html += String(F("<div class=card><span class=lbl>WiFi</span><span class=num>")) + v +
          F(" <small>redes</small></span><progress max=30 value=") + v + F("></progress></div>");
  snprintf(v, sizeof(v), "%d", (int)dashBle);
  html += String(F("<div class='card b'><span class=lbl>BLE</span><span class=num>")) + v +
          F(" <small>disp.</small></span><progress max=60 value=") + v + F("></progress></div>");
  if (isnan(t)) html += F("<div class='card t'><span class=lbl>Temperatura</span><span class=num>--</span><small>sem leitura</small></div>");
  else { snprintf(v, sizeof(v), "%.1f", t);
         html += String(F("<div class='card t'><span class=lbl>Temperatura</span><span class=num>")) + v +
                 F(" <small>°C</small></span><meter min=0 max=60 value=") + v + F("></meter></div>"); }
  if (isnan(h)) html += F("<div class='card h'><span class=lbl>Umidade</span><span class=num>--</span><small>sem leitura</small></div>");
  else { snprintf(v, sizeof(v), "%.0f", h);
         html += String(F("<div class='card h'><span class=lbl>Umidade</span><span class=num>")) + v +
                 F(" <small>%</small></span><meter min=0 max=100 value=") + v + F("></meter></div>"); }
  html += F("</section><h2>Arquivos</h2>");
  server.sendContent(html); html = "";

  int achados = 0;
  bool sdOk = false;
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) == pdTRUE) {
    sdOk = sd.fatType() != 0;  // 0 = volume não montado (remount falhou)
    if (sdOk) {
      for (const char* nome : nomes) {
        FsFile f = sd.open(caminhoPermitido(nome), O_RDONLY);
        if (!f) continue;
        uint64_t tam = f.fileSize();
        f.close();
        achados++;
        html += "<div class=file><b>" + String(nome) + "</b><span>" + tamanhoLegivel(tam) +
                "</span><a class=btn href='/download?file=" + nome + "' download>Baixar</a>"
                "<form method=post action='/apagar?file=" + nome + "' onsubmit=\"return confirm('Apagar " + nome +
                "?')\"><button class=del>Apagar</button></form></div>";
      }
    }
    xSemaphoreGive(sdMutex);
  }
  if (!sdOk)             html += F("<p><b>Cartão SD indisponível no momento.</b></p>");
  else if (achados == 0) html += F("<p>Nenhum arquivo de log no cartão.</p>");

  html += F("<p><a class=btn href='/'>Atualizar</a></p>"
            "<p class=foot>O hotspot fecha após 5 minutos sem atividade (abrir/atualizar esta "
            "página ou baixar um arquivo renova o prazo).</p></main></body></html>");
  server.sendContent(html);
  server.sendContent("");  // fecha o ultimo chunk
}

// Motivo pra abortar a transferência, ou nullptr pra seguir.
const char* motivoAborto(NetworkClient& client, unsigned long ultimoProgresso) {
  if (!hotspotAberto)        return "hotspot fechado";
  if (!client.connected())   return "desconexao";
  if (millis() - ultimoProgresso >= HTTP_STALL_MS) return "timeout sem progresso";
  return nullptr;
}

// GET /download?file= — envia o arquivo byte a byte como está no SD.
// Enquanto o arquivo está aberto (downloadAtivo), flushBuffers() é adiado,
// e com ele remontarSD(); os dados seguem acumulando nos buffers circulares.
void httpDownload() {
  webUltimoMs.store(millis());
  webDownloads.fetch_add(1);
  // Mantem a transicao terminal ate a loopTask imprimi-la; sem fila.
  if (downloadEstado.load() != DOWNLOAD_OCIOSO) {
    downloadsOcupado.fetch_add(1);
    server.send(503, "text/plain", "Download ocupado\n");
    return;
  }
  String nome = server.arg("file");
  downloadArquivo.store(nome == "log.txt" ? 1 : nome == "wifi.txt" ? 2 : nome == "ble.txt" ? 3 : 0);
  downloadTotal.store(0); downloadEnviado.store(0);
  downloadMotivo.store(nullptr); downloadStalls.store(0);
  downloadInicioMs.store(millis());
  downloadEstado.store(DOWNLOAD_INICIADO);
  const char* path = caminhoPermitido(nome);
  if (!path) { server.send(400, "text/plain", "Arquivo invalido\n"); finalizarDownload(false); return; }

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "text/plain", "SD ocupado\n");
    finalizarDownload(false);
    return;
  }
  if (sd.fatType() == 0) {
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "SD indisponivel\n");
    finalizarDownload(false);
    return;
  }
  if (!sd.exists(path)) {
    xSemaphoreGive(sdMutex);
    server.send(404, "text/plain", "Arquivo nao encontrado\n");
    finalizarDownload(false);
    return;
  }
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) {
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "Falha ao abrir arquivo\n");
    finalizarDownload(false);
    return;
  }
  uint64_t total = f.fileSize();
  // ponytail: Content-Length via WebServer é size_t (32 bits) — arquivos >4 GB
  // recusados. Log cresce ~1,3 MB/dia; se um dia precisar, cabeçalho manual.
  if (total > 0xFFFFFFFFULL) {
    f.close();
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "Arquivo > 4 GB nao suportado\n");
    finalizarDownload(false);
    return;
  }
  downloadTotal.store((uint32_t)total);
  downloadAtivo = true;
  xSemaphoreGive(sdMutex);

  ultimaAtividade = millis();
  downloadEstado.store(DOWNLOAD_INICIADO);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + nome + "\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength((size_t)total);
  server.send(200, "text/plain; charset=utf-8", "");

  NetworkClient client = server.client();
  uint64_t enviado = 0;
  unsigned long ultimoProgresso = millis();
  const char* motivo = nullptr;
  while (enviado < total && !motivo) {
    size_t pedir = (total - enviado < HTTP_BLOCO) ? (size_t)(total - enviado) : HTTP_BLOCO;
    xSemaphoreTake(sdMutex, portMAX_DELAY);
    int lidos = f.read(httpBuf, pedir);
    xSemaphoreGive(sdMutex);
    if (lidos != (int)pedir) { motivo = "erro de leitura SD"; break; }

    size_t off = 0;
    while (off < pedir && !(motivo = motivoAborto(client, ultimoProgresso))) {
      size_t w = client.write(httpBuf + off, pedir - off);
      if (w > 0) { off += w; downloadEnviado.store((uint32_t)(enviado + off)); ultimoProgresso = millis(); }
      else { downloadStalls.fetch_add(1); vTaskDelay(pdMS_TO_TICKS(2)); }
    }
    enviado += off;
    vTaskDelay(1);  // cede CPU entre blocos
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  bool closeOk = f.close();
  downloadAtivo = false;
  xSemaphoreGive(sdMutex);
  downloadMotivo.store(motivo);
  client.stop();  // incompleto: conexão fecha antes do Content-Length — navegador acusa falha

  ultimaAtividade = millis();
  finalizarDownload(!motivo && closeOk && enviado == total);
}

// POST /apagar?file= — remove o arquivo; log.txt volta só com o cabeçalho CSV.
void httpApagar() {
  webUltimoMs.store(millis());
  const char* path = caminhoPermitido(server.arg("file"));
  if (!path) { server.send(400, "text/plain", "Arquivo invalido\n"); return; }
  if (downloadEstado.load() != DOWNLOAD_OCIOSO || downloadAtivo) {
    server.send(503, "text/plain", "Download em andamento\n");
    return;
  }
  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "text/plain", "SD ocupado\n");
    return;
  }
  bool ok = sd.fatType() != 0 && (!sd.exists(path) || sd.remove(path));
  if (ok && path == logFileName) inicializarArquivoLog();
  xSemaphoreGive(sdMutex);
  if (!ok) { server.send(503, "text/plain", "Falha ao apagar\n"); return; }
  eventoSerial("EVT", "Apagado %s", path);
  server.sendHeader("Location", "/");
  server.send(303);
}

void tarefaHotspotHTTP(void*) {
  for (;;) {
    if (hotspotAberto) server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(hotspotAberto ? 2 : 100));
  }
}

// Recursos do hotspot criados uma vez no boot. Falha não para o logger:
// só desabilita o hotspot (o check parado volta direto pro sono).
void setupHotspot() {
  server.on("/", HTTP_GET, httpRaiz);
  server.on("/download", HTTP_GET, httpDownload);
  server.on("/apagar", HTTP_POST, httpApagar);
  server.onNotFound([]() { webUltimoMs.store(millis()); webOutros.fetch_add(1); server.send(404, "text/plain", "Nao encontrado\n"); });
  server.begin();
  hotspotDisponivel = xTaskCreatePinnedToCore(tarefaHotspotHTTP, "Hotspot_HTTP",
                                              8192, nullptr, 2, nullptr, 1) == pdPASS;
  if (!hotspotDisponivel) eventoSerial("ERR", "falha ao criar tarefa HTTP. Hotspot desabilitado.");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  setenv("TZ", "UTC0", 1);
  tzset();
  // RX maior (padrão 256 B ≈ 266 ms @9600) pra não perder NMEA durante flush do SD
  Serial2.setRxBufferSize(1024);
  Serial2.begin(GPS_Serial_Baud, SERIAL_8N1, GPS_RX, GPS_TX);

  // Aloca os 3 buffers circulares no heap (antes eram arrays estáticos)
  logBuffer  = (char(*)[160])malloc((size_t)LOG_BUFFER_MAX  * 160);
  wifiBuffer = (char(*)[256])malloc((size_t)WIFI_BUFFER_MAX * 256);
  bleBuffer  = (char(*)[256])malloc((size_t)BLE_BUFFER_MAX  * 256);
  sdMutex = xSemaphoreCreateMutex();
  if (!logBuffer || !wifiBuffer || !bleBuffer || !sdMutex) {
    perdidos.fetch_add(!logBuffer + !wifiBuffer + !bleBuffer);
    eventoSerial("ERR", "Falha ao alocar buffers/mutex; perdidos=%lu", (unsigned long)perdidos.load());
    while (1) delay(1000);
  }

  // Inicialização do DHT22
  dht.begin();

  eventoSerial("EVT", "ESP32 GPS Logger v3 serial-2026-10-06-painel2 com SD, DHT22, MPU6050, WiFi, BLE e hotspot ---");
  eventoSerial("EVT", "Buffer log:%d wifi:%d ble:%d | Cache ssid:%d ble:%d",
                LOG_BUFFER_MAX, WIFI_BUFFER_MAX, BLE_BUFFER_MAX,
                SSID_CACHE_MAX, BLE_CACHE_MAX);

  // Inicialização do MPU6050 (I2C padrão: SDA=21, SCL=22)
  if (!mpu.begin()) {
    eventoSerial("EVT", "Aviso: MPU6050 nao encontrado. Dados de IMU desabilitados.");
    mpuDisponivel = false;
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // ±16G para offroad
    mpu.setGyroRange(MPU6050_RANGE_1000_DEG);        // ±1000 deg/s para offroad
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);      // Filtro 44 Hz
    mpuDisponivel = true;
    eventoSerial("EVT", "MPU6050 inicializado. Range: +-16G / +-1000 deg/s / 44Hz");
  }

  // Inicialização do SD Card — retry infinito até montar com sucesso.
  // Sem SD o log não serve pra nada, então vale esperar aqui em vez de
  // seguir gravando no vazio (causa raiz de "às vezes precisa religar
  // várias vezes até o SD pegar").
  int tentativaSD = 0;
  while (!sd.begin(SD_CONFIG)) {
    tentativaSD++;
    eventoSerial("ERR", "Falha ao montar o cartao SD (tentativa %d). Tentando novamente...", tentativaSD);
    sd.end();
    delay(500);
  }

  uint8_t cardType = sd.card()->type();
  eventoSerial("OK ", "SD montado tipo=%u", cardType);
  if (cardType == SD_CARD_TYPE_SD1)       eventoSerial("EVT", "SDSC");
  else if (cardType == SD_CARD_TYPE_SD2)  eventoSerial("EVT", "SDSC");
  else if (cardType == SD_CARD_TYPE_SDHC) eventoSerial("EVT", "SDHC/SDXC");
  else                                    eventoSerial("EVT", "DESCONHECIDO");

  uint64_t cardSize = (uint64_t)sd.card()->sectorCount() * 512ULL / (1024 * 1024);
  eventoSerial("EVT", "Tamanho do Cartao: %llu MB", cardSize);

  // Cria cabeçalho CSV se o arquivo ainda não existir
  inicializarArquivoLog();

  // Verificação dos arquivos de log — só existência, sem abrir/ler (poupa
  // ciclos de leitura do cartão)
  eventoSerial("EVT", "Arquivo %s: %s", logFileName,  sd.exists(logFileName)  ? "encontrado" : "sera criado na primeira gravacao");
  eventoSerial("EVT", "Arquivo %s: %s", wifiFileName, sd.exists(wifiFileName) ? "encontrado" : "sera criado na primeira gravacao");
  eventoSerial("EVT", "Arquivo %s: %s", bleFileName,  sd.exists(bleFileName)  ? "encontrado" : "sera criado na primeira gravacao");

  // Watchdog: se loop() travar por mais de WDT_TIMEOUT_S sem "alimentar"
  // o watchdog, o ESP32 reseta sozinho. Cobre travamentos de qualquer
  // origem (SD, I2C do MPU, WiFi scan, etc.) sem intervenção manual.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  // Core 3.x já inicia o TWDT (5 s) no boot: init falharia, então reconfigura
  esp_err_t wdtResultado = esp_task_wdt_reconfigure(&wdtConfig);
  if (wdtResultado == ESP_ERR_INVALID_STATE) wdtResultado = esp_task_wdt_init(&wdtConfig);
#else
  esp_err_t wdtResultado = esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_err_t wdtRegistro = esp_task_wdt_add(NULL);
  watchdogAtivo = wdtResultado == ESP_OK && (wdtRegistro == ESP_OK || esp_task_wdt_status(NULL) == ESP_OK);
  if (watchdogAtivo) eventoSerial("OK ", "Watchdog ativado: %ds", WDT_TIMEOUT_S);
  else eventoSerial("ERR", "Watchdog config=%d registro=%d", wdtResultado, wdtRegistro);
  int resetReason = (int)esp_reset_reason();
  eventoSerial("EVT", "Reset reason: %d", resetReason);

  // Contador de boot (RTC, sobrevive a soft-reset/watchdog) — grava um
  // marcador em log.txt pra diagnosticar reset espúrio no meio de um ciclo
  // sem precisar de captura serial ao vivo.
  bootCount++;
  eventoSerial("EVT", "Boot count: %lu", (unsigned long)bootCount);
  char bootMarker[64];
  snprintf(bootMarker, sizeof(bootMarker), "# BOOT bootCount=%lu reset_reason=%d\n",
           (unsigned long)bootCount, resetReason);
  if (appendFile(logFileName, bootMarker)) eventoSerial("OK ", "Marcador boot gravado");

  // BLE inicializado uma única vez no boot (fica pronto, só liga/desliga
  // scan conforme o modo Movimento/Parado); WiFi começa ligado (movimento).
  setupBLE();
  entrarModoMovimento();
  setupHotspot();  // depois do WiFi.mode(): precisa da pilha de rede iniciada

  eventoSerial("EVT", "Aguardando fix do GPS...");
}

// ─────────────────────────────────────────────────────────────────────────────
// Hotspot com acesso recente (ou download): a loopTask cede o core e a RAM
// de rede para a tarefa HTTP. GPS/MPU/SD/painel pausam; o buffer da UART do
// GPS e descartado pra nao estourar. Dura so enquanto houver atividade.
#define HOTSPOT_FOCO_MS 8000UL
bool hotspotEmFoco() {
  if (!hotspotAberto.load()) return false;
  if (downloadAtivo) return true;
  uint32_t ult = webUltimoMs.load();
  return ult != 0 && millis() - ult < HOTSPOT_FOCO_MS;
}

void loop() {
  if (hotspotEmFoco()) {
    while (Serial2.available()) Serial2.read();
    if (watchdogAtivo) esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(50));
    return;
  }
  loopNumero++;
  loopInicioMs = millis();
  faseLoop = "GPS/MPU";
  if (watchdogAtivo) esp_task_wdt_reset(); // alimenta o watchdog a cada volta do loop

  bool newData = false;

  // Acumuladores para calcular a média do MPU6050 durante o ciclo GPS
  DadosMPU somaMPU = {0, 0, 0, 0, 0, 0};
  int contadorMPU = 0;
  unsigned long ultimaLeituraMPU = millis();

  // Analisa dados do GPS por 1 segundo, amostrando o MPU a cada 100ms
  for (unsigned long start = millis(); millis() - start < 1000;) {
    if (watchdogAtivo) esp_task_wdt_reset();
    while (Serial2.available()) {
      char c = Serial2.read();
      if (receberGPS(c)) {
        newData = true;
      }
    }
    drenarFilaBLE();

    // Lê o MPU6050 a cada 100ms durante o ciclo GPS (sem bloquear o loop)
    if (mpuDisponivel && (millis() - ultimaLeituraMPU >= 100)) {
      sensors_event_t a, g, t;
      mpu.getEvent(&a, &g, &t);
      somaMPU.acX += a.acceleration.x;
      somaMPU.acY += a.acceleration.y;
      somaMPU.acZ += a.acceleration.z;
      somaMPU.gyX += g.gyro.x;
      somaMPU.gyY += g.gyro.y;
      somaMPU.gyZ += g.gyro.z;
      contadorMPU++;
      ultimaLeituraMPU = millis();
    }
    vTaskDelay(1);  // cede a CPU (1 ms) em vez de espera ativa
  }

  // Calcula a média das amostras coletadas do MPU
  DadosMPU mediaMPU = {0, 0, 0, 0, 0, 0};
  if (contadorMPU > 0) {
    mediaMPU.acX = somaMPU.acX / contadorMPU;
    mediaMPU.acY = somaMPU.acY / contadorMPU;
    mediaMPU.acZ = somaMPU.acZ / contadorMPU;
    mediaMPU.gyX = somaMPU.gyX / contadorMPU;
    mediaMPU.gyY = somaMPU.gyY / contadorMPU;
    mediaMPU.gyZ = somaMPU.gyZ / contadorMPU;
  }

  tempoAquisicaoMs = millis() - loopInicioMs;
  uint32_t etapaInicio = millis();
  faseLoop = "DADOS";
  lerDHT();
  if (newData) processarDadosGPS(mediaMPU);
  tempoDadosMs = millis() - etapaInicio;
  etapaInicio = millis();
  faseLoop = "MODO/RF";
  servicoModo();
  tempoModoMs = millis() - etapaInicio;
  faseLoop = "WEB";
  imprimirAcessosWeb();
  imprimirDownload();
  etapaInicio = millis();
  faseLoop = "SD/RETRY";
  if (logBufferCount >= LOG_BUFFER_MAX || wifiBufferCount >= WIFI_BUFFER_MAX || bleBufferCount >= BLE_BUFFER_MAX)
    flushPendente = true;
  if (flushPendente && !radioCicloAtivo && !scanEmAndamento && !bleScanAtivo && !downloadAtivo &&
      millis() - ultimoRetryFlush >= 1000) flushBuffers();
  tempoSDMs = millis() - etapaInicio;
  faseLoop = "RESUMO";
  exibirDashboard(mediaMPU);
  loopAnteriorMs = millis() - loopInicioMs;
}
