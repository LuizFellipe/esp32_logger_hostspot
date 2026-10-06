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
#define WIFI_BUFFER_MAX   100    // Linhas de log WiFi acumuladas antes de flush
#define BLE_BUFFER_MAX    100    // Linhas de log BLE acumuladas antes de flush
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
WifiStats ultimoWifiStats = {0, 0, 0};  // último resultado de varrerWiFi(), pro dashboard
// Leituras mais recentes pro /api do hotspot (loopTask escreve, Hotspot_HTTP lê).
volatile float dashTemp = NAN, dashUmid = NAN;
volatile int   dashBle = 0;  // dispositivos BLE vistos no ciclo atual/último

// true enquanto um rádio (scan WiFi assíncrono ou scan BLE) está
// em andamento — global pra flushBuffers() poder checar antes de gravar no
// SD e evitar coincidir escrita física com o pico de corrente do rádio.
volatile bool scanEmAndamento = false;
volatile bool bleScanAtivo    = false;

// Última posição/hora conhecida do GPS — usada por drenarFilaBLE() pra
// rotular os achados BLE. Escrita e lida só pela loopTask.
char lastTimeStamp[25] = "---";
long lastLat = 0;
long lastLon = 0;

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

void adicionarHashCache(uint32_t* cache, int &count, int max, const char* s, bool &cheioAvisado, const char* label) {
  if (count < max) {
    cache[count] = hashString(s);
    count++;
  } else if (!cheioAvisado) {
    Serial.printf("Aviso: cache de %s cheio. Deduplicacao desativada a partir daqui.\n", label);
    cheioAvisado = true;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Grava bloco de dados no final de um arquivo no cartão SD (1 open/close)
// Retorna true só se abertura E escrita tiverem sucesso.
bool appendFile(const char *path, const char *message) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.print("Aviso: ");
    Serial.print(path);
    Serial.println(" nao disponivel para escrita.");
    return false;
  }

  bool ok = file.print(message) > 0;
  if (!ok) {
    Serial.print("Erro ao gravar em: ");
    Serial.println(path);
  }
  file.close();
  return ok;
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
      Serial.println("SD nao remontou apos multiplas tentativas. Reiniciando ESP32...");
      esp_restart();
    }
  }

  return ok;
}

// Grava as linhas de um buffer circular direto no arquivo (1 open/close),
// na ordem cronológica (mais antiga primeiro), sem montar uma cópia do
// bloco inteiro em RAM antes. Retorna quantas linhas foram efetivamente
// gravadas (pode ser < count se falhar no meio) — o chamador usa isso pra
// não reenviar linha já gravada numa tentativa seguinte.
int appendLinhasCirculares(const char* path, char* buf, int rows, int lineLen, int head, int count) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.print("Aviso: ");
    Serial.print(path);
    Serial.println(" nao disponivel para escrita.");
    return 0;
  }

  int escritas = 0;
  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    const char* linha = buf + ((size_t)((head + i) % rows) * lineLen);
    ok = file.print(linha) > 0;
    if (ok) escritas++;
  }
  if (!ok) {
    Serial.print("Erro ao gravar em: ");
    Serial.println(path);
  }
  file.close();
  return escritas;
}

// Adiciona uma linha a um buffer circular: se cheio, descarta a mais antiga
// pra abrir espaço (preserva sempre o trecho mais recente do percurso).
void adicionarLinhaCircular(char* buf, int rows, int lineLen, int &head, int &count, const char* linha) {
  int idx;
  if (count < rows) {
    idx = (head + count) % rows;
    count++;
  } else {
    idx = head;                // sobrescreve a mais antiga
    head = (head + 1) % rows;  // avança o início, descartando-a
  }
  char* destino = buf + ((size_t)idx * lineLen);
  strncpy(destino, linha, lineLen - 1);
  destino[lineLen - 1] = '\0';
}

// Flush: grava os 3 buffers no SD. Retorna true só se todos os buffers com
// dados foram gravados com sucesso. Em caso de falha parcial, os dados não
// escritos ficam retidos no buffer circular e o SD é remontado, para a
// próxima tentativa.
bool flushBuffers() {
  // Não grava no SD com rádio (WiFi ou BLE) em andamento — evita coincidir
  // escrita física com o pico de corrente do scan.
  if (scanEmAndamento || bleScanAtivo) {
    Serial.println(F("Flush adiado: radio em andamento."));
    return false;
  }

  // Download com arquivo aberto: não grava nem remonta o SD por baixo dele.
  // Checa de novo já com o mutex — downloadAtivo é ligado sob sdMutex.
  if (downloadAtivo) {
    Serial.println(F("Flush adiado: download em andamento."));
    return false;
  }
  xSemaphoreTake(sdMutex, portMAX_DELAY);
  if (downloadAtivo) {
    xSemaphoreGive(sdMutex);
    Serial.println(F("Flush adiado: download em andamento."));
    return false;
  }

  Serial.println(F(">> GRAVANDO SD... NAO DESLIGAR! <<"));
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
    esp_task_wdt_reset();
    if (*b.count > 0) {
      Serial.printf("  %s: %d linhas... ", b.label, *b.count);
      int escritas = appendLinhasCirculares(b.path, b.buf, b.rows, b.lineLen, *b.head, *b.count);
      *b.head   = (*b.head + escritas) % b.rows;
      *b.count -= escritas;
      if (escritas > 0 && *b.count == 0) {
        Serial.println("OK");
      } else {
        Serial.printf("FALHOU (%d/%d linhas gravadas, restante mantido em buffer)\n", escritas, escritas + *b.count);
        falhaAlgum = true;
      }
    }
  }

  if (falhaAlgum) {
    remontarSD();  // tenta recuperar pro próximo ciclo
    xSemaphoreGive(sdMutex);
    return false;
  }
  xSemaphoreGive(sdMutex);

  Serial.println(F(">> GRAVACAO CONCLUIDA. SEGURO DESLIGAR. <<"));
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
    appendFile(logFileName, cabecalho);
    Serial.println("Cabecalho CSV criado em log.txt");
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
void exibirDashboard(const char* timeStamp, long lat, long lon,
                     int sat, float hdop, float kmh, const char* direcao,
                     float umidade, float tempDHT,
                     DadosMPU mediaMPU, WifiStats wifi, bool temFix) {

  // Se o buffer Serial estiver cheio (host desconectado/lento), não bloqueia a loopTask
  if (Serial.availableForWrite() < 128) return;
  char barra[LOG_BUFFER_MAX + 1];
  for (int i = 0; i < LOG_BUFFER_MAX; i++) {
    barra[i] = (i < logBufferCount) ? '#' : '-';
  }
  barra[LOG_BUFFER_MAX] = '\0';

  Serial.println(F("================================================"));

  // Linha GPS
  if (temFix) {
    Serial.printf("  GPS   %s  Sat:%d  HDOP:%.1f\n", timeStamp, sat, hdop);
    Serial.printf("  Pos   Lat:%.6f  Lon:%.6f\n", lat / 1000000.0, lon / 1000000.0);
    Serial.printf("  Mov   Vel:%.1f km/h  Dir:%s\n", kmh, direcao);
  } else {
    Serial.println(F("  GPS   --- Aguardando fix ---"));
    Serial.println(F("  Pos   Lat:---  Lon:---"));
    Serial.println(F("  Mov   Vel:---  Dir:---"));
  }

  // Linha DHT
  if (isnan(tempDHT) || isnan(umidade)) {
    Serial.println(F("  DHT   Temp:---  Umid:---"));
  } else {
    Serial.printf("  DHT   Temp:%.1f C  Umid:%.1f%%\n", tempDHT, umidade);
  }

  // Linha MPU
  if (mpuDisponivel) {
    Serial.printf("  MPU   Ac(%.2f %.2f %.2f)  Gy(%.2f %.2f %.2f)\n",
                  mediaMPU.acX, mediaMPU.acY, mediaMPU.acZ,
                  mediaMPU.gyX, mediaMPU.gyY, mediaMPU.gyZ);
  } else {
    Serial.println(F("  MPU   --- Indisponivel ---"));
  }

  // Linha MODO (Movimento/Parado)
  Serial.printf("  MODO  %s\n", modoNome(modoAtual));

  if (temFix) {
    Serial.printf("  WiFi  %d redes | %d novas | %d dup\n",
                  wifi.total, wifi.novas, wifi.dup);
  } else {
    Serial.println(F("  WiFi  --- (sem fix GPS) ---"));
  }

  switch (modoAtual) {
    case MODO_MOVIMENTO: {
      long remSeg = (long)(RADIO_SCAN_INTERVAL_MS - (millis() - ultimoCicloFim)) / 1000;
      if (radioCicloAtivo) {
        Serial.println(F("  RF    ciclo WiFi+BLE em andamento"));
      } else {
        if (remSeg < 0) remSeg = 0;
        Serial.printf("  RF    proximo ciclo WiFi+BLE em %lds\n", remSeg);
      }
      break;
    }
    case MODO_PARADO_SONO: {
      long remSeg = (long)(PARKED_SLEEP_MS - (millis() - modoInicio)) / 1000;
      if (remSeg < 0) remSeg = 0;
      Serial.printf("  RF    dormindo (sem scan)  acorda em %lds ou ao mover\n", remSeg);
      break;
    }
    case MODO_PARADO_CHECK:
      Serial.println(F("  RF    check unico WiFi+BLE em andamento"));
      break;
    case MODO_PARADO_HOTSPOT: {
      long remSeg = (long)(HOTSPOT_IDLE_MS - (millis() - ultimaAtividade)) / 1000;
      if (remSeg < 0) remSeg = 0;
      Serial.printf("  RF    hotspot %s (192.168.4.1)%s  fecha em %lds sem atividade\n",
                    HOTSPOT_SSID, downloadAtivo ? " [download]" : "", remSeg);
      break;
    }
  }

  // Linha cadência do log.txt (10s em movimento, 30s parado)
  Serial.printf("  LOG   gravacao no buffer a cada %lus\n",
                (modoAtual == MODO_MOVIMENTO ? LOG_ADD_MOVING_MS : LOG_ADD_PARKED_MS) / 1000);

  // Linha Buffer/SD
  Serial.printf("  SD    [%s] %d/%d\n", barra, logBufferCount, LOG_BUFFER_MAX);

  // Linha buffers de todas as fontes + caches de deduplicação
  Serial.printf("  BUF   log:%d/%d  wifi:%d/%d  ble:%d/%d\n",
                logBufferCount, LOG_BUFFER_MAX,
                wifiBufferCount, WIFI_BUFFER_MAX,
                bleBufferCount, BLE_BUFFER_MAX);
  Serial.printf("  CACHE ssid:%d/%d  ble:%d/%d  boot:%lu\n",
                ssidCacheCount, SSID_CACHE_MAX,
                bleCacheCount, BLE_CACHE_MAX,
                (unsigned long)bootCount);

  Serial.println(F("================================================"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Faz scan de redes WiFi de forma assíncrona (não-bloqueante)
// Retorna as estatísticas do scan anterior se um novo estiver em andamento.
WifiStats varrerWiFi(const char* timeStamp, long lat, long lon) {
  static WifiStats lastStats = {0, 0, 0};

  // Se não há scan rodando, inicia um novo de forma assíncrona (true, true para show_hidden e passive)
  if (!scanEmAndamento) {
    WiFi.scanNetworks(true, true);
    scanEmAndamento = true;
    return lastStats; // Retorna último resultado enquanto varre
  }

  // Verifica se o scan assíncrono terminou
  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
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
    snprintf(dadosWifi, sizeof(dadosWifi), "%s, %ld, %ld, %s, %ld, %d, %s\n",
             timeStamp, lat, lon, ssid, WiFi.RSSI(i),
             (int)WiFi.channel(i), obterTipoCriptografia(WiFi.encryptionType(i)));

    // Acumula no buffer circular WiFi (descarta a linha mais antiga se cheio)
    adicionarLinhaCircular(&wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount, dadosWifi);
    stats.novas++;
  }

  WiFi.scanDelete();
  scanEmAndamento = false; // Pronto para o próximo ciclo
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

      char dadosBLE[256];
      if (rec->hasTxPower) {
        snprintf(dadosBLE, sizeof(dadosBLE), "%s, %ld, %ld, %s, %s, %d, %d\n",
                 lastTimeStamp, lastLat, lastLon, rec->address,
                 rec->hasName ? rec->name : "", (int)rec->rssi, (int)rec->txPower);
      } else {
        snprintf(dadosBLE, sizeof(dadosBLE), "%s, %ld, %ld, %s, %s, %d, \n",
                 lastTimeStamp, lastLat, lastLon, rec->address,
                 rec->hasName ? rec->name : "", (int)rec->rssi);
      }
      adicionarLinhaCircular(&bleBuffer[0][0], BLE_BUFFER_MAX, 256, bleBufferHead, bleBufferCount, dadosBLE);
    }
    free(rec);
  }
}

// Callbacks do Scanner BLE — dedup dentro do scan fica com o filtro de
// duplicatas nativo do NimBLE (filter_duplicates, ligado por padrão).
class BLEScanCallbacks : public NimBLEScanCallbacks {

  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string addrStr = dev->getAddress().toString();

    BLEDeviceRecord* rec = (BLEDeviceRecord*)malloc(sizeof(BLEDeviceRecord));
    if (rec == nullptr) return;
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
      free(rec); // Queue cheia: descarta sem bloquear o BLE stack
    }
  }

} bleScanCallbacks;

// Inicializa o controller BLE e a fila uma única vez no boot.
// Não inicia scan aqui — quem liga/desliga o scan é o state machine de modo.
void setupBLE() {
  bleQueue = xQueueCreate(BLE_QUEUE_DEPTH, sizeof(BLEDeviceRecord*));
  if (bleQueue == nullptr) {
    Serial.println(F("ERRO: falha ao criar bleQueue"));
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  NimBLEDevice::init("");
  pBLEScan = NimBLEDevice::getScan();
  pBLEScan->setScanCallbacks(&bleScanCallbacks, false);
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(100);
  pBLEScan->setMaxResults(0);
}

// Liga o scan BLE por BLE_SCAN_DURATION_MS (termina sozinho)
void bleScanLigar() {
  bleScanAtivo = true;
  pBLEScan->start(BLE_SCAN_DURATION_MS);
}

// Garante o scan BLE parado (no-op se já terminou sozinho)
void bleScanDesligar() {
  pBLEScan->stop();
  bleScanAtivo = false;
}

// ═════════════════════════════════════════════════════════════════════════════
// State machine Movimento/Parado — sem restart, rádios nunca desligam, só
// variam a cadência de scan (economia é só de cartão SD, não de energia)
// ═════════════════════════════════════════════════════════════════════════════

// Dispara um novo ciclo de scan WiFi+BLE simultâneo. WiFi é disparado pelo
// scanWifiAtivo em processarDadosGPS(); BLE começa aqui.
void iniciarCicloRadio() {
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

  if (!radioCicloWifiDone && !scanEmAndamento && (agora - radioCicloInicio > 500)) {
    radioCicloWifiDone = true;
  }
  if (!radioCicloBleDone && (agora - radioCicloInicio >= BLE_SCAN_DURATION_MS)) {
    bleScanDesligar();
    radioCicloBleDone = true;
  }

  if (radioCicloWifiDone && radioCicloBleDone) {
    radioCicloAtivo = false;
    ultimoCicloFim = agora;
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
  Serial.println(F("Hotspot fechado."));
}

void entrarModoMovimento() {
  if (modoAtual == MODO_PARADO_HOTSPOT) {
    Serial.println(F("Movimento: fechando hotspot."));
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
  Serial.println(F("Parado: dormindo (sem scan) por 5 min..."));

  // Grava tudo no SD antes do período sem scan (momento seguro pro cartão).
  // Se falhar, os dados ficam no buffer circular e o flush por capacidade
  // tenta de novo.
  flushBuffers();
}

void entrarModoParadoCheck() {
  modoAtual = MODO_PARADO_CHECK;
  modoInicio = millis();
  Serial.println(F("Acordou: checando WiFi + BLE juntos..."));
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
  flushBuffers();
  WiFi.mode(WIFI_AP);
  IPAddress ip(192, 168, 4, 1);
  bool ok = WiFi.softAPConfig(ip, ip, IPAddress(255, 255, 255, 0)) &&
            WiFi.softAP(HOTSPOT_SSID, HOTSPOT_PASS, HOTSPOT_CANAL, 0, 1);
  if (!ok) {
    Serial.println(F("ERRO: falha ao abrir hotspot. Tenta de novo no proximo check."));
    WiFi.mode(WIFI_STA);
    entrarModoParadoSono();
    return;
  }
  modoAtual  = MODO_PARADO_HOTSPOT;
  modoInicio = millis();
  ultimaAtividade = millis();
  hotspotAberto = true;
  Serial.printf("Hotspot aberto: SSID %s  http://%s/  fecha apos %lus sem atividade\n",
                HOTSPOT_SSID, WiFi.softAPIP().toString().c_str(), HOTSPOT_IDLE_MS / 1000);
}

// Parte temporal da máquina de estados — roda a cada volta do loop, com ou
// sem fix novo (GPS mudo não pode travar sono/hotspot/ciclo de scan).
// Achados WiFi usam a última posição conhecida.
void servicoModo() {
  if (radioCicloAtivo && !radioCicloWifiDone) {
    ultimoWifiStats = varrerWiFi(lastTimeStamp, lastLat, lastLon);
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
        Serial.println(F("Hotspot: 5 min sem atividade."));
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
    Serial.println(F("Parou: ciclo WiFi+BLE final antes do hotspot..."));
    if (!radioCicloAtivo) iniciarCicloRadio();
  }
  // Demais transições parado (sono/check/hotspot) são temporais: servicoModo().
}

// ─────────────────────────────────────────────────────────────────────────────
// Processa dados do GPS e DHT22, exibe no Serial e acumula no buffer
// Recebe a média das leituras do MPU6050 coletadas durante o ciclo GPS
void processarDadosGPS(DadosMPU mediaMPU) {
  long lat, lon;
  unsigned long fix_age, date, time;

  // Coleta posição em números inteiros (milionésimos de grau)
  gps.get_position(&lat, &lon, &fix_age);

  // Coleta data e hora brutas
  gps.get_datetime(&date, &time, &fix_age);

  // Coleta outros dados
  int sat    = (gps.satellites() == TinyGPS::GPS_INVALID_SATELLITES) ? 0 : gps.satellites();
  float hdop = (gps.hdop() == TinyGPS::GPS_INVALID_HDOP) ? 0 : gps.hdop() / 100.0;
  float kmh  = (gps.f_speed_kmph() == TinyGPS::GPS_INVALID_F_SPEED) ? 0.0 : gps.f_speed_kmph();

  // Coleta direção em formato cardinal ("N", "S", "SE", etc.)
  const char* direcao = TinyGPS::cardinal(gps.f_course());

  // Coleta dados do DHT22
  float umidade = dht.readHumidity();
  float tempDHT = dht.readTemperature();
  dashTemp = tempDHT;
  dashUmid = umidade;

  // Extrai componentes da data (DDMMYY)
  int dia  = date / 10000;
  int mes  = (date / 100) % 100;
  int ano  = 2000 + (date % 100);

  // Extrai componentes da hora (HHMMSSCC)
  int hora    = time / 1000000;
  int minuto  = (time / 10000) % 100;
  int segundo = (time / 100) % 100;

  // Ajuste de fuso horário (-3h) com tratamento de virada de dia
  struct tm t;
  t.tm_year  = ano - 1900;
  t.tm_mon   = mes - 1;
  t.tm_mday  = dia;
  t.tm_hour  = hora;
  t.tm_min   = minuto;
  t.tm_sec   = segundo;
  t.tm_isdst = -1; // Não considerar horário de verão

  // Subtrai 3 horas (UTC-3)
  t.tm_hour -= 3;

  // mktime "normaliza" a estrutura (ajustando dia/mês/ano se necessário)
  mktime(&t);

  // Buffer para o timestamp
  char timeStamp[25];
  snprintf(timeStamp, sizeof(timeStamp), "%02d/%02d/%04d %02d:%02d:%02d",
           t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);

  // Atualiza a última posição/hora conhecida — usada por drenarFilaBLE().
  strncpy(lastTimeStamp, timeStamp, sizeof(lastTimeStamp) - 1);
  lastTimeStamp[sizeof(lastTimeStamp) - 1] = '\0';
  lastLat = lat;
  lastLon = lon;

  // Monta linha do CSV
  char logData[160];
  if (mpuDisponivel) {
    snprintf(logData, sizeof(logData),
             "%s, %ld, %ld, %d, %.2f, %.2f, %s, %.1f, %.1f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f\n",
             timeStamp, lat, lon, sat, hdop, kmh, direcao, umidade, tempDHT,
             mediaMPU.acX, mediaMPU.acY, mediaMPU.acZ,
             mediaMPU.gyX, mediaMPU.gyY, mediaMPU.gyZ);
  } else {
    // MPU indisponível: campos vazios para manter estrutura do CSV
    snprintf(logData, sizeof(logData),
             "%s, %ld, %ld, %d, %.2f, %.2f, %s, %.1f, %.1f, , , , , , \n",
             timeStamp, lat, lon, sat, hdop, kmh, direcao, umidade, tempDHT);
  }

  // Atualiza o modo Movimento/Parado com base na velocidade atual
  atualizarModo(kmh);

  // Acumula no buffer circular de log — 10s em movimento, 30s parado
  // (leitura/dashboard continuam a cada segundo, só o append é espaçado)
  unsigned long cadenciaLog = (modoAtual == MODO_MOVIMENTO) ? LOG_ADD_MOVING_MS : LOG_ADD_PARKED_MS;
  if (millis() - lastLogAddMs >= cadenciaLog) {
    adicionarLinhaCircular(&logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount, logData);
    lastLogAddMs = millis();
  }

  // Scan WiFi (poll do ciclo) roda em servicoModo(), a cada volta do loop.
  exibirDashboard(timeStamp, lat, lon, sat, hdop, kmh, direcao,
                  umidade, tempDHT, mediaMPU, ultimoWifiStats, true);

  // Flush do buffer de log por capacidade
  if (logBufferCount >= LOG_BUFFER_MAX) {
    flushBuffers();
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
a.btn:focus-visible{outline:3px solid var(--ink);outline-offset:2px}
.foot{margin-top:16px;color:var(--mut);font-size:.8rem}
@media(max-width:380px){.grid{grid-template-columns:1fr}}
)CSS";

// GET / — sensores (valores do último scan) + lista dos logs. HTML5 + CSS, sem JS.
// Renova a janela. "Atualizar" recarrega a página.
void httpRaiz() {
  ultimaAtividade = millis();
  const char* nomes[] = { "log.txt", "wifi.txt", "ble.txt" };
  float t = dashTemp, h = dashUmid;
  char v[16];

  String html = F("<!doctype html><html lang=pt-BR><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Logs ESP32 GPS</title><style>");
  html += PAGINA_CSS;
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
                "</span><a class=btn href='/download?file=" + nome + "' download>Baixar</a></div>";
      }
    }
    xSemaphoreGive(sdMutex);
  }
  if (!sdOk)             html += F("<p><b>Cartão SD indisponível no momento.</b></p>");
  else if (achados == 0) html += F("<p>Nenhum arquivo de log no cartão.</p>");

  html += F("<p><a class=btn href='/'>Atualizar</a></p>"
            "<p class=foot>O hotspot fecha após 5 minutos sem atividade (abrir/atualizar esta "
            "página ou baixar um arquivo renova o prazo).</p></main></body></html>");
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", html);
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
  String nome = server.arg("file");
  const char* path = caminhoPermitido(nome);
  if (!path) { server.send(400, "text/plain", "Arquivo invalido\n"); return; }

  if (xSemaphoreTake(sdMutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    server.send(503, "text/plain", "SD ocupado\n");
    return;
  }
  if (sd.fatType() == 0) {
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "SD indisponivel\n");
    return;
  }
  if (!sd.exists(path)) {
    xSemaphoreGive(sdMutex);
    server.send(404, "text/plain", "Arquivo nao encontrado\n");
    return;
  }
  FsFile f = sd.open(path, O_RDONLY);
  if (!f) {
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "Falha ao abrir arquivo\n");
    return;
  }
  uint64_t total = f.fileSize();
  // ponytail: Content-Length via WebServer é size_t (32 bits) — arquivos >4 GB
  // recusados. Log cresce ~1,3 MB/dia; se um dia precisar, cabeçalho manual.
  if (total > 0xFFFFFFFFULL) {
    f.close();
    xSemaphoreGive(sdMutex);
    server.send(503, "text/plain", "Arquivo > 4 GB nao suportado\n");
    return;
  }
  downloadAtivo = true;
  xSemaphoreGive(sdMutex);

  ultimaAtividade = millis();
  Serial.printf("Download %s: %llu bytes\n", nome.c_str(), total);
  server.sendHeader("Content-Disposition", "attachment; filename=\"" + nome + "\"");
  server.sendHeader("Cache-Control", "no-store");
  server.setContentLength((size_t)total);
  server.send(200, "text/plain; charset=utf-8", "");

  NetworkClient client = server.client();
  uint64_t enviado = 0;
  unsigned long inicio = millis(), ultimoProgresso = millis();
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
      if (w > 0) { off += w; ultimoProgresso = millis(); }
      else vTaskDelay(pdMS_TO_TICKS(10));
    }
    enviado += off;
    vTaskDelay(1);  // cede CPU entre blocos
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);
  f.close();
  downloadAtivo = false;
  xSemaphoreGive(sdMutex);
  client.stop();  // incompleto: conexão fecha antes do Content-Length — navegador acusa falha

  ultimaAtividade = millis();
  if (motivo) {
    Serial.printf("Download %s ABORTADO (%s): %llu/%llu bytes\n", nome.c_str(), motivo, enviado, total);
  } else {
    Serial.printf("Download %s concluido: %llu bytes em %lus\n", nome.c_str(), enviado, (millis() - inicio) / 1000);
  }
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
  server.onNotFound([]() { server.send(404, "text/plain", "Nao encontrado\n"); });
  server.begin();
  hotspotDisponivel = xTaskCreatePinnedToCore(tarefaHotspotHTTP, "Hotspot_HTTP",
                                              8192, nullptr, 1, nullptr, 1) == pdPASS;
  if (!hotspotDisponivel) Serial.println(F("ERRO: falha ao criar tarefa HTTP. Hotspot desabilitado."));
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  // RX maior (padrão 256 B ≈ 266 ms @9600) pra não perder NMEA durante flush do SD
  Serial2.setRxBufferSize(1024);
  Serial2.begin(GPS_Serial_Baud, SERIAL_8N1, GPS_RX, GPS_TX);

  // Aloca os 3 buffers circulares no heap (antes eram arrays estáticos)
  logBuffer  = (char(*)[160])malloc((size_t)LOG_BUFFER_MAX  * 160);
  wifiBuffer = (char(*)[256])malloc((size_t)WIFI_BUFFER_MAX * 256);
  bleBuffer  = (char(*)[256])malloc((size_t)BLE_BUFFER_MAX  * 256);
  sdMutex = xSemaphoreCreateMutex();
  if (!logBuffer || !wifiBuffer || !bleBuffer || !sdMutex) {
    Serial.println(F("ERRO CRITICO: falha ao alocar buffers de log. Travando."));
    while (1) delay(1000);
  }

  // Inicialização do DHT22
  dht.begin();

  Serial.println(F("\n--- ESP32 GPS Logger v3 com SD, DHT22, MPU6050, WiFi, BLE e hotspot ---"));
  Serial.printf("Buffer log:%d wifi:%d ble:%d | Cache ssid:%d ble:%d\n",
                LOG_BUFFER_MAX, WIFI_BUFFER_MAX, BLE_BUFFER_MAX,
                SSID_CACHE_MAX, BLE_CACHE_MAX);

  // Inicialização do MPU6050 (I2C padrão: SDA=21, SCL=22)
  if (!mpu.begin()) {
    Serial.println("Aviso: MPU6050 nao encontrado. Dados de IMU desabilitados.");
    mpuDisponivel = false;
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);   // ±16G para offroad
    mpu.setGyroRange(MPU6050_RANGE_1000_DEG);        // ±1000 deg/s para offroad
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);      // Filtro 44 Hz
    mpuDisponivel = true;
    Serial.println("MPU6050 inicializado. Range: +-16G / +-1000 deg/s / 44Hz");
  }

  // Inicialização do SD Card — retry infinito até montar com sucesso.
  // Sem SD o log não serve pra nada, então vale esperar aqui em vez de
  // seguir gravando no vazio (causa raiz de "às vezes precisa religar
  // várias vezes até o SD pegar").
  int tentativaSD = 0;
  while (!sd.begin(SD_CONFIG)) {
    tentativaSD++;
    Serial.printf("Falha ao montar o cartao SD (tentativa %d). Tentando novamente...\n", tentativaSD);
    sd.end();
    delay(500);
  }

  uint8_t cardType = sd.card()->type();
  Serial.print("Cartao SD montado. Tipo: ");
  if (cardType == SD_CARD_TYPE_SD1)       Serial.println("SDSC");
  else if (cardType == SD_CARD_TYPE_SD2)  Serial.println("SDSC");
  else if (cardType == SD_CARD_TYPE_SDHC) Serial.println("SDHC/SDXC");
  else                                    Serial.println("DESCONHECIDO");

  uint64_t cardSize = (uint64_t)sd.card()->sectorCount() * 512ULL / (1024 * 1024);
  Serial.printf("Tamanho do Cartao: %llu MB\n", cardSize);

  // Cria cabeçalho CSV se o arquivo ainda não existir
  inicializarArquivoLog();

  // Verificação dos arquivos de log — só existência, sem abrir/ler (poupa
  // ciclos de leitura do cartão)
  Serial.printf("Arquivo %s: %s\n", logFileName,  sd.exists(logFileName)  ? "encontrado" : "sera criado na primeira gravacao");
  Serial.printf("Arquivo %s: %s\n", wifiFileName, sd.exists(wifiFileName) ? "encontrado" : "sera criado na primeira gravacao");
  Serial.printf("Arquivo %s: %s\n", bleFileName,  sd.exists(bleFileName)  ? "encontrado" : "sera criado na primeira gravacao");

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
  esp_task_wdt_reconfigure(&wdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.printf("Watchdog ativado: %ds\n", WDT_TIMEOUT_S);
  int resetReason = (int)esp_reset_reason();
  Serial.printf("Reset reason: %d\n", resetReason);

  // Contador de boot (RTC, sobrevive a soft-reset/watchdog) — grava um
  // marcador em log.txt pra diagnosticar reset espúrio no meio de um ciclo
  // sem precisar de captura serial ao vivo.
  bootCount++;
  Serial.printf("Boot count: %lu\n", (unsigned long)bootCount);
  char bootMarker[64];
  snprintf(bootMarker, sizeof(bootMarker), "# BOOT bootCount=%lu reset_reason=%d\n",
           (unsigned long)bootCount, resetReason);
  appendFile(logFileName, bootMarker);

  // BLE inicializado uma única vez no boot (fica pronto, só liga/desliga
  // scan conforme o modo Movimento/Parado); WiFi começa ligado (movimento).
  setupBLE();
  entrarModoMovimento();
  setupHotspot();  // depois do WiFi.mode(): precisa da pilha de rede iniciada

  Serial.println(F("Aguardando fix do GPS...\n"));
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset(); // alimenta o watchdog a cada volta do loop

  bool newData = false;

  // Acumuladores para calcular a média do MPU6050 durante o ciclo GPS
  DadosMPU somaMPU = {0, 0, 0, 0, 0, 0};
  int contadorMPU = 0;
  unsigned long ultimaLeituraMPU = millis();

  // Analisa dados do GPS por 1 segundo, amostrando o MPU a cada 100ms
  for (unsigned long start = millis(); millis() - start < 1000;) {
    esp_task_wdt_reset();
    while (Serial2.available()) {
      char c = Serial2.read();
      if (gps.encode(c)) {
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

  if (newData) {
    processarDadosGPS(mediaMPU);
  } else {
    // Sem fix GPS: exibe dashboard parcial (DHT + MPU visíveis)
    float umidade = dht.readHumidity();
    float tempDHT = dht.readTemperature();
    dashTemp = tempDHT;
    dashUmid = umidade;
    WifiStats semWifi = {0, 0, 0};
    exibirDashboard("---", 0, 0, 0, 0, 0, "---",
                    umidade, tempDHT, mediaMPU, semWifi, false);
  }

  // Temporizadores/ciclo de scan independem de fix novo
  servicoModo();
}

