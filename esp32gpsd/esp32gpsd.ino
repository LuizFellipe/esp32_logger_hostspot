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
#include <BluetoothSerial.h>
#include <vector>
#include <string>
#include <atomic>

// Arduino IDE -> Tools -> Partition Scheme -> "No OTA (Large APP)" —
// necessario pra WiFi + BluetoothSerial + NimBLE juntos nao estourar a Flash.

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
#define BT_BUFFER_MAX     100    // Linhas de log BT Clássico acumuladas antes de flush
#define SSID_CACHE_MAX    500    // Máx SSIDs rastreadas para deduplicação (guarda hash, não string)
#define BLE_CACHE_MAX     200    // Máx MACs BLE rastreadas para deduplicação
#define BT_CACHE_MAX      200    // Máx MACs BT Clássico rastreadas para deduplicação

// Watchdog: se o loop() não "alimentar" o watchdog nesse tempo, o ESP32
// assume que travou (SD/I2C pendurado etc.) e reseta sozinho.
#define WDT_TIMEOUT_S 15

// Se o remount do SD falhar essa quantidade de vezes seguidas, reinicia
// o ESP32 inteiro (na esperança de que um boot limpo destrave o hardware).
#define SD_REMOUNT_MAX_FALHAS 10

// Sono do WiFi: parado (abaixo desse km/h) e sem SSID novo -> desliga o
// rádio WiFi por WIFI_SLEEP_MS pra evitar scan (pico de corrente) coincidindo
// com escrita no SD, que já causou brownout/travamento em campo.
// Reaproveitado também como intervalo da fase WiFi: a cada WIFI_SLEEP_MS a
// fase WiFi cede o rádio pra uma excursão de BLE/BT (ver RF Phase Sequencer).
#define WIFI_SLEEP_KMH_THRESHOLD 2.0
#define WIFI_SLEEP_MS            (5UL * 60UL * 1000UL)

// ─────────────────────────────────────────────────────────────────────────────
// RF Phase Sequencer — WiFi é a fase "padrão" de longa duração; BLE e BT são
// excursões curtas fora dela. Ciclo: WIFI(~5min) -> BLE(30s) -> BT(15s) -> WIFI.
// ─────────────────────────────────────────────────────────────────────────────
#define PHASE_WIFI  0
#define PHASE_BLE   1
#define PHASE_BT    2

#define PHASE_BLE_MS   30000UL
#define PHASE_BT_MS    15000UL
#define BT_INQUIRY_MS  (PHASE_BT_MS - 3000UL)  // margem de 3s pra flush/encerrar

TinyGPS gps;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;
SdFs sd;

bool mpuDisponivel = false;
int  falhasRemountSD = 0;  // Falhas consecutivas de remount do SD

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
const char* btFileName   = "/bt.txt";

// ─────────────────────────────────────────────────────────────────────────────
// Buffers em RAM comum — circulares por linha, mesma lógica pros 4 arquivos.
// Só são esvaziados quando a escrita no SD é confirmada.
char logBuffer[LOG_BUFFER_MAX][160];
int  logBufferHead  = 0;
int  logBufferCount = 0;

char wifiBuffer[WIFI_BUFFER_MAX][256];
int  wifiBufferHead  = 0;
int  wifiBufferCount = 0;

char bleBuffer[BLE_BUFFER_MAX][256];
int  bleBufferHead  = 0;
int  bleBufferCount = 0;

char btBuffer[BT_BUFFER_MAX][256];
int  btBufferHead  = 0;
int  btBufferCount = 0;

// ─────────────────────────────────────────────────────────────────────────────
// Estado persistente em RTC Slow Memory (.rtc.bss). As variáveis abaixo NÃO
// têm inicializador (nem "= 0") de propósito: com inicializador o C-startup
// as recopia da flash a cada soft reset (currentPhase/caches voltariam a 0
// a cada esp_restart()/deep sleep), sem inicializador ficam em .rtc.bss e
// sobrevivem — só apagam em power-off real.
RTC_DATA_ATTR uint8_t  currentPhase;
RTC_DATA_ATTR uint32_t phaseCount;

// Cache de SSIDs já gravadas — nunca reseta enquanto a placa ficar ligada.
// Precisa estar em RTC porque agora há restarts programados (RF Phase
// Sequencer) a cada poucos minutos; em RAM comum voltaria a duplicar toda
// SSID a cada ciclo. Guarda hash FNV-1a de 32 bits em vez da string.
RTC_DATA_ATTR uint32_t ssidCacheHash[SSID_CACHE_MAX];
RTC_DATA_ATTR int      ssidCacheCount;
RTC_DATA_ATTR uint32_t bleCacheHash[BLE_CACHE_MAX];
RTC_DATA_ATTR int      bleCacheCount;
RTC_DATA_ATTR uint32_t btCacheHash[BT_CACHE_MAX];
RTC_DATA_ATTR int      btCacheCount;

bool ssidCacheCheioAvisado = false;  // evita spam do aviso de cache cheio
bool bleCacheCheioAvisado  = false;
bool btCacheCheioAvisado   = false;

// Estado do sono do WiFi (ver WIFI_SLEEP_KMH_THRESHOLD / WIFI_SLEEP_MS)
bool wifiDormindo = false;
unsigned long wifiSleepStart = 0;

// Início da fase WiFi atual — usado só dentro do boot corrente (a fase WiFi
// nunca reinicia no meio de si mesma), não precisa ser RTC.
unsigned long wifiPhaseStart = 0;

// Timer próprio do log.txt (desacoplado do timer do WiFi acima, pra não
// interferir na lógica de despertar dele) — enquanto parado, log.txt só
// grava em rajada a cada WIFI_SLEEP_MS; 0 = não está no modo "parado".
unsigned long logParadoStart = 0;

// true enquanto um rádio (scan WiFi assíncrono, scan BLE ou inquiry BT) está
// em andamento — global pra flushBuffers() poder checar antes de gravar no
// SD e evitar coincidir escrita física com o pico de corrente do rádio.
bool scanEmAndamento = false;

// Última posição/hora conhecida do GPS — consumida pelas consumer tasks de
// BLE/BT (rodam no Core 0, fora do fluxo de processarDadosGPS) pra rotular
// os achados de rádio com a posição do veículo no momento da detecção.
char lastTimeStamp[25] = "---";
long lastLat = 0;
long lastLon = 0;

// Protege o acesso ao objeto `sd` entre o loop() (Core 1, flush por
// cadência normal) e as orchestrator tasks de BLE/BT (Core 0, flush antes
// de trocar de fase) — sem isso, as duas escritas concorrentes correm risco
// de corromper o SdFat.
SemaphoreHandle_t sdMutex = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Hash FNV-1a de 32 bits e cache de deduplicação genéricos — usados pra
// SSID, MAC BLE e MAC BT, guardando 4 bytes por entrada em vez da string.
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

// Flush: grava os 4 buffers no SD. Retorna true só se todos os buffers com
// dados foram gravados com sucesso — usado pelo RF Phase Sequencer pra só
// trocar de fase (e disparar o restart) depois de confiar que nada ficou
// pra trás. Em caso de falha parcial, os dados não escritos ficam retidos
// no buffer circular e o SD é remontado, para o próximo ciclo tentar de novo.
bool flushBuffers() {
  // Não grava no SD com um rádio em andamento (WiFi scan, BLE scan, BT
  // inquiry): os dois picos de corrente juntos (rádio + escrita física) já
  // causaram brownout/travamento em campo. Adia pro próximo ciclo.
  if (scanEmAndamento) {
    Serial.println(F("Flush adiado: radio em andamento."));
    return false;
  }

  xSemaphoreTake(sdMutex, portMAX_DELAY);

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
    { btFileName,   &btBuffer[0][0],   BT_BUFFER_MAX,   256, &btBufferHead,   &btBufferCount,   "bt.txt"   },
  };

  for (auto &b : buffers) {
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

  Serial.println(F(">> GRAVACAO CONCLUIDA. SEGURO DESLIGAR. <<"));
  xSemaphoreGive(sdMutex);
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

// Nome legível da fase atual, pro Serial/dashboard
const char* faseNome(uint8_t fase) {
  switch (fase) {
    case PHASE_WIFI: return "WIFI";
    case PHASE_BLE:  return "BLE";
    case PHASE_BT:   return "BT";
    default:         return "?";
  }
}

// Restart garantido preservando RTC_DATA_ATTR — deep sleep de 10ms em vez de
// esp_restart() puro, mesmo efeito de limpeza de RAM/rádio, mas o bootloader
// garante a preservação do RTC_DATA_ATTR no wakeup.
void triggerRestart() {
  Serial.flush();
  esp_sleep_enable_timer_wakeup(10000ULL);
  esp_deep_sleep_start();
}

// ─────────────────────────────────────────────────────────────────────────────
// Exibe dashboard ASCII no Serial Monitor
void exibirDashboard(const char* timeStamp, long lat, long lon,
                     int sat, float hdop, float kmh, const char* direcao,
                     float umidade, float tempDHT,
                     DadosMPU mediaMPU, WifiStats wifi, bool temFix) {

  // Monta barra de progresso do buffer
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

  // Linha FASE (RF Phase Sequencer)
  Serial.printf("  FASE  %s (ciclo %lu)\n", faseNome(currentPhase), (unsigned long)phaseCount);

  // Linha WiFi + sono: só fazem sentido na fase WiFi (nas fases BLE/BT o
  // rádio WiFi está desligado por definição)
  if (currentPhase == PHASE_WIFI) {
    if (temFix) {
      Serial.printf("  WiFi  %d redes | %d novas | %d dup\n",
                    wifi.total, wifi.novas, wifi.dup);
    } else {
      Serial.println(F("  WiFi  --- (sem fix GPS) ---"));
    }

    if (wifiDormindo) {
      long remSeg = (long)(WIFI_SLEEP_MS - (millis() - wifiSleepStart)) / 1000;
      if (remSeg < 0) remSeg = 0;
      Serial.printf("  RF    WiFi:OFF (dormindo)  acorda em %lds ou ao mover\n", remSeg);
    } else {
      Serial.println(F("  RF    WiFi:ON"));
    }
  } else {
    Serial.printf("  RF    %s ativo (WiFi volta ao fim da fase)\n", faseNome(currentPhase));
  }

  // Linha estado do log.txt parado (rajada a cada WIFI_SLEEP_MS)
  if (logParadoStart != 0) {
    long remSeg = (long)(WIFI_SLEEP_MS - (millis() - logParadoStart)) / 1000;
    if (remSeg < 0) remSeg = 0;
    Serial.printf("  LOG   Parado: gravacao em rajada em %lds\n", remSeg);
  } else {
    Serial.println(F("  LOG   Em movimento: gravacao normal (por buffer)"));
  }

  // Linha Buffer/SD
  Serial.printf("  SD    [%s] %d/%d\n", barra, logBufferCount, LOG_BUFFER_MAX);

  // Linha buffers de todas as fontes + caches de deduplicação
  Serial.printf("  BUF   log:%d/%d  wifi:%d/%d  ble:%d/%d  bt:%d/%d\n",
                logBufferCount, LOG_BUFFER_MAX,
                wifiBufferCount, WIFI_BUFFER_MAX,
                bleBufferCount, BLE_BUFFER_MAX,
                btBufferCount, BT_BUFFER_MAX);
  Serial.printf("  CACHE ssid:%d/%d  ble:%d/%d  bt:%d/%d\n",
                ssidCacheCount, SSID_CACHE_MAX,
                bleCacheCount, BLE_CACHE_MAX,
                btCacheCount, BT_CACHE_MAX);

  Serial.println(F("================================================"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Faz scan de redes WiFi de forma assíncrona (não-bloqueante)
// Retorna as estatísticas do scan anterior se um novo estiver em andamento.
WifiStats varrerWiFi(const char* timeStamp, long lat, long lon, float kmh) {
  static WifiStats lastStats = {0, 0, 0};

  // Sono: WiFi desligado enquanto o veículo está parado e sem redes novas.
  // Acorda se voltar a se mover ou se o tempo de sono estourar.
  if (wifiDormindo) {
    bool voltouAMover = kmh >= WIFI_SLEEP_KMH_THRESHOLD;
    bool tempoEsgotado = millis() - wifiSleepStart >= WIFI_SLEEP_MS;
    if (voltouAMover || tempoEsgotado) {
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      wifiDormindo = false;
      // ssidCache NÃO reseta aqui de propósito: se o scan ao acordar não
      // achar rede nova, nada é gravado e ele volta a dormir (sem duplicar
      // nome de rede já visto antes do sono).
      Serial.println(voltouAMover ? "WiFi acordado: veiculo em movimento."
                                   : "WiFi acordado: tempo de sono esgotado.");
    } else {
      return lastStats; // continua dormindo
    }
  }

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
             WiFi.channel(i), obterTipoCriptografia(WiFi.encryptionType(i)));

    // Acumula no buffer circular WiFi (descarta a linha mais antiga se cheio)
    adicionarLinhaCircular(&wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount, dadosWifi);
    stats.novas++;
  }

  WiFi.scanDelete();
  scanEmAndamento = false; // Pronto para o próximo ciclo
  lastStats = stats;

  // Parado e nenhuma rede nova nesta varredura -> dorme
  if (kmh < WIFI_SLEEP_KMH_THRESHOLD && stats.novas == 0) {
    WiFi.mode(WIFI_OFF);
    wifiDormindo = true;
    wifiSleepStart = millis();
    Serial.println("WiFi dormindo: veiculo parado, sem redes novas.");
  }

  return stats;
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE_BLE — NimBLE-Arduino v2.x, scan ativo com deduplicação por MAC
// ═════════════════════════════════════════════════════════════════════════════
#define BLE_SCAN_DURATION_MS       5000   // Duração de cada ciclo interno de scan
#define BLE_QUEUE_DEPTH            10
#define BLE_CONSUMER_STACK         4096
#define BLE_CONSUMER_PRIORITY      1
#define BLE_ORCHESTRATOR_STACK     4096
#define BLE_ORCHESTRATOR_PRIORITY  2
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
  uint32_t timestamp;
};

QueueHandle_t            bleQueue = nullptr;
NimBLEScan*               pBLEScan = nullptr;
std::vector<std::string>  seenInCycleBLE;
std::atomic<bool>         blePhaseEnding{false}; // Guarda race entre onScanEnd e orchestrator

// Consumer Task (Core 1): recebe BLEDeviceRecord da fila, dedup persistente
// e acumula no bleBuffer
void bleConsumerTask(void* pvParameters) {
  (void)pvParameters;
  BLEDeviceRecord* rec = nullptr;

  for (;;) {
    if (xQueueReceive(bleQueue, &rec, portMAX_DELAY) == pdTRUE && rec != nullptr) {
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
      rec = nullptr;
    }
  }
}

// Callbacks do Scanner BLE
class BLEScanCallbacks : public NimBLEScanCallbacks {

  void onResult(const NimBLEAdvertisedDevice* dev) override {
    std::string addrStr = dev->getAddress().toString();

    // Deduplicação transiente por MAC dentro do ciclo de scan interno
    for (const auto& seen : seenInCycleBLE) {
      if (seen == addrStr) return;
    }
    seenInCycleBLE.push_back(addrStr);

    BLEDeviceRecord* rec = (BLEDeviceRecord*)malloc(sizeof(BLEDeviceRecord));
    if (rec == nullptr) return;
    memset(rec, 0, sizeof(BLEDeviceRecord));

    rec->timestamp = (uint32_t)millis();
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
      free(rec); // Queue cheia: descarta sem bloquear o BT stack
    }
  }

  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    // Se o orchestrator sinalizou fim de fase, NÃO reinicia o scan
    if (blePhaseEnding) return;

    seenInCycleBLE.clear();
    pBLEScan->clearResults();
    pBLEScan->start(BLE_SCAN_DURATION_MS);
  }

} bleScanCallbacks;

// Orchestrator Task BLE (Core 0): aguarda PHASE_BLE_MS, flush confirmado e
// só então avança de fase
void bleOrchestratorTask(void* pvParameters) {
  (void)pvParameters;

  vTaskDelay(pdMS_TO_TICKS(PHASE_BLE_MS));

  // Sinaliza fim de fase ANTES de parar o scan para evitar race com onScanEnd
  blePhaseEnding = true;
  vTaskDelay(pdMS_TO_TICKS(10));

  pBLEScan->stop();
  vTaskDelay(pdMS_TO_TICKS(300));
  NimBLEDevice::deinit(true);
  vTaskDelay(pdMS_TO_TICKS(100));

  scanEmAndamento = false;
  while (!flushBuffers()) {
    vTaskDelay(pdMS_TO_TICKS(500)); // SD indisponível: tenta de novo antes de trocar de fase
  }

  currentPhase = PHASE_BT;
  triggerRestart();
}

void initBLEPhase() {
  Serial.printf("=== PHASE_BLE | ciclo %lu ===\n", (unsigned long)phaseCount);
  scanEmAndamento = true;

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

  xTaskCreatePinnedToCore(bleConsumerTask,     "BLE_Consumer",
                          BLE_CONSUMER_STACK,     nullptr,
                          BLE_CONSUMER_PRIORITY,  nullptr, 1);

  xTaskCreatePinnedToCore(bleOrchestratorTask, "BLE_Orch",
                          BLE_ORCHESTRATOR_STACK,     nullptr,
                          BLE_ORCHESTRATOR_PRIORITY,  nullptr, 0);

  pBLEScan->start(BLE_SCAN_DURATION_MS);
}

// ═════════════════════════════════════════════════════════════════════════════
// PHASE_BT — BluetoothSerial, inquiry ativo de dispositivos Bluetooth Clássico
// ═════════════════════════════════════════════════════════════════════════════
#define BT_QUEUE_DEPTH              10
#define BT_CONSUMER_STACK           3072
#define BT_CONSUMER_PRIORITY        1
#define BT_ORCHESTRATOR_STACK       4096
#define BT_ORCHESTRATOR_PRIORITY    1
#define BT_DEVICE_NAME_LEN          65

// BT Device Record — alocado no heap via malloc, transportado por fila.
// Class of Device do PoC foi descartada: sem consumidor hoje.
struct BTDeviceRecord {
  char     address[18];
  char     name[BT_DEVICE_NAME_LEN];
  bool     hasName;
  int8_t   rssi;
  bool     hasRSSI;
  uint32_t timestamp;
};

BluetoothSerial SerialBT;
QueueHandle_t   btQueue = nullptr;

// Callback do inquiry — chamado pelo BT stack para cada dispositivo encontrado
void btDeviceFoundCB(BTAdvertisedDevice* pDevice) {
  BTDeviceRecord* rec = (BTDeviceRecord*)malloc(sizeof(BTDeviceRecord));
  if (rec == nullptr) return;
  memset(rec, 0, sizeof(BTDeviceRecord));

  rec->timestamp = (uint32_t)millis();
  strncpy(rec->address, pDevice->getAddress().toString().c_str(), sizeof(rec->address) - 1);

  if (pDevice->haveName()) {
    rec->hasName = true;
    strncpy(rec->name, pDevice->getName().c_str(), sizeof(rec->name) - 1);
  }
  if (pDevice->haveRSSI()) {
    rec->hasRSSI = true;
    rec->rssi    = (int8_t)pDevice->getRSSI();
  }

  if (xQueueSend(btQueue, &rec, 0) != pdTRUE) {
    free(rec); // Queue cheia: descarta
  }
}

// Consumer Task (Core 1): recebe BTDeviceRecord da fila, dedup persistente
// e acumula no btBuffer
void btConsumerTask(void* pvParameters) {
  (void)pvParameters;
  BTDeviceRecord* rec = nullptr;

  for (;;) {
    if (xQueueReceive(btQueue, &rec, portMAX_DELAY) == pdTRUE && rec != nullptr) {
      if (!hashJaVisto(btCacheHash, btCacheCount, rec->address)) {
        adicionarHashCache(btCacheHash, btCacheCount, BT_CACHE_MAX, rec->address, btCacheCheioAvisado, "BT");

        char dadosBT[256];
        snprintf(dadosBT, sizeof(dadosBT), "%s, %ld, %ld, %s, %s, %d\n",
                 lastTimeStamp, lastLat, lastLon, rec->address,
                 rec->hasName ? rec->name : "",
                 rec->hasRSSI ? (int)rec->rssi : 0);
        adicionarLinhaCircular(&btBuffer[0][0], BT_BUFFER_MAX, 256, btBufferHead, btBufferCount, dadosBT);
      }
      free(rec);
      rec = nullptr;
    }
  }
}

// Orchestrator Task Classic BT (Core 0): conduz inquiry, flush confirmado e
// só então avança de fase
void btOrchestratorTask(void* pvParameters) {
  (void)pvParameters;

  SerialBT.discoverAsync(btDeviceFoundCB);
  vTaskDelay(pdMS_TO_TICKS(BT_INQUIRY_MS));

  SerialBT.discoverClear();
  vTaskDelay(pdMS_TO_TICKS(2000)); // drena a fila antes de encerrar

  SerialBT.end();
  vTaskDelay(pdMS_TO_TICKS(100));

  scanEmAndamento = false;
  while (!flushBuffers()) {
    vTaskDelay(pdMS_TO_TICKS(500)); // SD indisponível: tenta de novo antes de trocar de fase
  }

  currentPhase = PHASE_WIFI;
  triggerRestart();
}

void initBTPhase() {
  Serial.printf("=== PHASE_BT | ciclo %lu ===\n", (unsigned long)phaseCount);
  scanEmAndamento = true;

  btQueue = xQueueCreate(BT_QUEUE_DEPTH, sizeof(BTDeviceRecord*));
  if (btQueue == nullptr) {
    Serial.println(F("ERRO: falha ao criar btQueue"));
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
  }

  SerialBT.begin("ESP32_GPSD_BT");

  xTaskCreatePinnedToCore(btConsumerTask,     "BT_Consumer",
                          BT_CONSUMER_STACK,     nullptr,
                          BT_CONSUMER_PRIORITY,  nullptr, 1);

  xTaskCreatePinnedToCore(btOrchestratorTask, "BT_Orch",
                          BT_ORCHESTRATOR_STACK,     nullptr,
                          BT_ORCHESTRATOR_PRIORITY,  nullptr, 0);
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

  // Atualiza a última posição/hora conhecida — consumida pelas consumer
  // tasks de BLE/BT (Core 0), que rodam fora deste fluxo.
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

  // Acumula no buffer circular de log (descarta a linha mais antiga se cheio)
  adicionarLinhaCircular(&logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount, logData);

  // Scan WiFi só roda na fase WiFi (nas fases BLE/BT o rádio WiFi está
  // desligado — ver RF Phase Sequencer)
  WifiStats wifiStats = {0, 0, 0};
  if (currentPhase == PHASE_WIFI) {
    wifiStats = varrerWiFi(timeStamp, lat, lon, kmh);
  }

  // Exibe dashboard no Serial Monitor
  exibirDashboard(timeStamp, lat, lon, sat, hdop, kmh, direcao,
                  umidade, tempDHT, mediaMPU, wifiStats, true);

  // Flush do log.txt: cadência normal (por contagem) em movimento; parado,
  // vira rajada única a cada WIFI_SLEEP_MS (mesma regra do sono do WiFi,
  // timer próprio pra não interferir no wifiSleepStart). Nas transições
  // (parar / voltar a mover) faz flush do que está em buffer, para não
  // perder dados de viagem enquanto o buffer circular espera a rajada.
  if (kmh > WIFI_SLEEP_KMH_THRESHOLD) {
    if (logParadoStart != 0) {
      flushBuffers();          // voltou a mover: grava o buffer da parada
      logParadoStart = 0;
    }
    if (logBufferCount >= LOG_BUFFER_MAX) {
      flushBuffers();          // cadência normal em movimento
    }
  } else {
    if (logParadoStart == 0) {
      flushBuffers();          // acabou de parar: grava dados da viagem
      logParadoStart = millis(); // inicia a janela de 5 min
    } else if (millis() - logParadoStart >= WIFI_SLEEP_MS) {
      flushBuffers();
      logParadoStart = millis(); // reinicia o ciclo de 5 min
    }
    // senão: parado e ainda dentro da janela de 5 min, não grava
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial2.begin(GPS_Serial_Baud, SERIAL_8N1, GPS_RX, GPS_TX);

  sdMutex = xSemaphoreCreateMutex();
  if (sdMutex == nullptr) {
    Serial.println(F("ERRO CRITICO: falha ao criar sdMutex. Travando."));
    while (1) delay(1000);
  }

  // Inicialização do DHT22
  dht.begin();

  Serial.println(F("\n--- ESP32 GPS Logger com SD, DHT22, MPU6050, WiFi, BT e BLE ---"));
  Serial.printf("Buffer log:%d wifi:%d ble:%d bt:%d | Cache ssid:%d ble:%d bt:%d\n",
                LOG_BUFFER_MAX, WIFI_BUFFER_MAX, BLE_BUFFER_MAX, BT_BUFFER_MAX,
                SSID_CACHE_MAX, BLE_CACHE_MAX, BT_CACHE_MAX);

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
  Serial.printf("Arquivo %s: %s\n", btFileName,   sd.exists(btFileName)   ? "encontrado" : "sera criado na primeira gravacao");

  // Watchdog: se loop() travar por mais de WDT_TIMEOUT_S sem "alimentar"
  // o watchdog, o ESP32 reseta sozinho. Cobre travamentos de qualquer
  // origem (SD, I2C do MPU, WiFi scan, etc.) sem intervenção manual.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdtConfig = {
    .timeout_ms = WDT_TIMEOUT_S * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true
  };
  esp_task_wdt_init(&wdtConfig);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);
  Serial.printf("Watchdog ativado: %ds\n", WDT_TIMEOUT_S);

  // RF Phase Sequencer: roteia pra fase correta após cada restart.
  // Fallback de segurança caso a memória RTC corrompa.
  if (currentPhase > PHASE_BT) currentPhase = PHASE_WIFI;
  phaseCount++;

  Serial.printf("Fase atual: %s | Ciclo: %lu | Reset reason: %d\n",
                faseNome(currentPhase), (unsigned long)phaseCount, (int)esp_reset_reason());

  switch (currentPhase) {
    case PHASE_WIFI:
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      wifiPhaseStart = millis();
      break;
    case PHASE_BLE:
      initBLEPhase();
      break;
    case PHASE_BT:
      initBTPhase();
      break;
  }

  Serial.println(F("Aguardando fix do GPS...\n"));
}

// ─────────────────────────────────────────────────────────────────────────────
void loop() {
  esp_task_wdt_reset(); // alimenta o watchdog a cada volta do loop

  // Fase WiFi esgotou seu tempo: cede o rádio pra excursão BLE/BT. Só
  // avança de fase depois de flush confirmado (não perde dados por causa
  // só do timing do restart) — se falhar, tenta de novo no próximo loop.
  if (currentPhase == PHASE_WIFI && (millis() - wifiPhaseStart >= WIFI_SLEEP_MS)) {
    WiFi.mode(WIFI_OFF);
    scanEmAndamento = false;
    if (flushBuffers()) {
      currentPhase = PHASE_BLE;
      triggerRestart();
    }
  }

  bool newData = false;

  // Acumuladores para calcular a média do MPU6050 durante o ciclo GPS
  DadosMPU somaMPU = {0, 0, 0, 0, 0, 0};
  int contadorMPU = 0;
  unsigned long ultimaLeituraMPU = millis();

  // Analisa dados do GPS por 1 segundo, amostrando o MPU a cada 100ms
  for (unsigned long start = millis(); millis() - start < 1000;) {
    while (Serial2.available()) {
      char c = Serial2.read();
      if (gps.encode(c)) {
        newData = true;
      }
    }

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
    WifiStats semWifi = {0, 0, 0};
    exibirDashboard("---", 0, 0, 0, 0, 0, "---",
                    umidade, tempDHT, mediaMPU, semWifi, false);
  }
}
