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

// Configuração de buffer
#define LOG_BUFFER_MAX    150    // Linhas de log GPS acumuladas antes de flush
#define WIFI_BUFFER_MAX   100    // Linhas de log WiFi acumuladas antes de flush
#define SSID_CACHE_MAX    500    // Máx SSIDs rastreadas para deduplicação (guarda hash, não string)

// Watchdog: timeout de 15 segundos
#define WDT_TIMEOUT_S 15

// Se o remount do SD falhar essa quantidade de vezes seguidas, reinicia
#define SD_REMOUNT_MAX_FALHAS 10

// Sono do WiFi
#define WIFI_SLEEP_KMH_THRESHOLD 2.0
#define WIFI_SLEEP_MS            (5UL * 60UL * 1000UL)

TinyGPS gps;
DHT dht(DHTPIN, DHTTYPE);
Adafruit_MPU6050 mpu;
SdFs sd;

bool mpuDisponivel = false;
int  falhasRemountSD = 0;  // Falhas consecutivas de remount do SD

// Estrutura para armazenar dados médios do MPU6050
struct DadosMPU {
  float acX, acY, acZ;
  float gyX, gyY, gyZ;
};

// Estatísticas do scan WiFi
struct WifiStats {
  int total;
  int novas;
  int dup;
};

// Nomes dos arquivos de log
const char* logFileName  = "/log.txt";
const char* wifiFileName = "/wifi.txt";

// ─────────────────────────────────────────────────────────────────────────────
// Mutexes de Sincronização
SemaphoreHandle_t logMutex = NULL;
SemaphoreHandle_t wifiMutex = NULL;
SemaphoreHandle_t serialMutex = NULL;
SemaphoreHandle_t sensorMutex = NULL;

#define LOCK_MUTEX(m)   xSemaphoreTake(m, portMAX_DELAY)
#define UNLOCK_MUTEX(m) xSemaphoreGive(m)

// ─────────────────────────────────────────────────────────────────────────────
// Buffers de log em RAM — circulares por linha (Globais, protegidos por Mutex)
char (*logBuffer)[160] = NULL;
int  logBufferHead  = 0;
int  logBufferCount = 0;

char (*wifiBuffer)[256] = NULL;
int  wifiBufferHead  = 0;
int  wifiBufferCount = 0;

// Buffers de Escrita Locais (exclusivos da tarefa do Core 0, não precisam de Mutex)
char (*logWriteBuffer)[160] = NULL;
int  logWriteCount = 0;

char (*wifiWriteBuffer)[256] = NULL;
int  wifiWriteCount = 0;

// Cache de SSIDs já gravadas (acessado apenas no Core 0, sem necessidade de mutex)
uint32_t ssidCacheHash[SSID_CACHE_MAX];
int  ssidCacheCount = 0;
bool ssidCacheCheioAvisado = false;

// Estado do sono do WiFi (exclusivo do Core 0)
bool wifiDormindo = false;
unsigned long wifiSleepStart = 0;

// Timer próprio do log.txt (exclusivo do Core 0)
unsigned long logParadoStart = 0;

// Estado do scan WiFi (exclusivo do Core 0)
bool scanEmAndamento = false;

// Dados compartilhados de sensores (atualizados pelo Core 1, lidos pelo Core 0 para o dashboard)
DadosMPU globalMediaMPU = {0, 0, 0, 0, 0, 0};
float globalUmidade = NAN;
float globalTempDHT = NAN;
bool globalTemFix = false;
char globalTimeStamp[80] = "---";
long globalLat = 0;
long globalLon = 0;
int globalSat = 0;
float globalHdop = 0;
float globalKmh = 0;
char globalDirecao[6] = "---";

// ─────────────────────────────────────────────────────────────────────────────
// Hash FNV-1a de 32 bits
uint32_t hashSSID(const char* ssid) {
  uint32_t hash = 2166136261u;
  for (const char* p = ssid; *p; p++) {
    hash ^= (uint8_t)*p;
    hash *= 16777619u;
  }
  return hash;
}

// Verifica se uma SSID já está no cache (só roda no Core 0)
bool ssidJaVista(const char* ssid) {
  uint32_t h = hashSSID(ssid);
  for (int i = 0; i < ssidCacheCount; i++) {
    if (ssidCacheHash[i] == h) {
      return true;
    }
  }
  return false;
}

// Adiciona SSID ao cache (só roda no Core 0)
void adicionarSSIDCache(const char* ssid) {
  if (ssidCacheCount < SSID_CACHE_MAX) {
    ssidCacheHash[ssidCacheCount] = hashSSID(ssid);
    ssidCacheCount++;
  } else if (!ssidCacheCheioAvisado) {
    LOCK_MUTEX(serialMutex);
    Serial.println("Aviso: cache de SSID cheio (SSID_CACHE_MAX). Deduplicacao desativada.");
    UNLOCK_MUTEX(serialMutex);
    ssidCacheCheioAvisado = true;
  }
}

// Grava bloco de dados no cartão SD
bool appendFile(const char *path, const char *message) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOCK_MUTEX(serialMutex);
    Serial.print("Aviso: ");
    Serial.print(path);
    Serial.println(" nao disponivel para escrita.");
    UNLOCK_MUTEX(serialMutex);
    return false;
  }

  bool ok = file.print(message) > 0;
  if (!ok) {
    LOCK_MUTEX(serialMutex);
    Serial.print("Erro ao gravar em: ");
    Serial.println(path);
    UNLOCK_MUTEX(serialMutex);
  }
  file.close();
  return ok;
}

// Tenta remontar o cartão SD
bool remontarSD() {
  sd.end();
  vTaskDelay(pdMS_TO_TICKS(50));
  bool ok = sd.begin(SD_CONFIG);

  if (ok) {
    falhasRemountSD = 0;
  } else {
    falhasRemountSD++;
    if (falhasRemountSD >= SD_REMOUNT_MAX_FALHAS) {
      LOCK_MUTEX(serialMutex);
      Serial.println("SD nao remontou apos multiplas tentativas. Reiniciando ESP32...");
      UNLOCK_MUTEX(serialMutex);
      esp_restart();
    }
  }
  return ok;
}

// Grava as linhas copiadas no SD (rodando no Core 0, sem travar o Core 1)
int appendLinhasWriteBuffer(const char* path, char* buf, int rows, int lineLen, int count) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    LOCK_MUTEX(serialMutex);
    Serial.print("Aviso: ");
    Serial.print(path);
    Serial.println(" nao disponivel para escrita.");
    UNLOCK_MUTEX(serialMutex);
    return 0;
  }

  int escritas = 0;
  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    const char* linha = buf + ((size_t)i * lineLen);
    ok = file.print(linha) > 0;
    if (ok) escritas++;
  }
  if (!ok) {
    LOCK_MUTEX(serialMutex);
    Serial.print("Erro ao gravar em: ");
    Serial.println(path);
    UNLOCK_MUTEX(serialMutex);
  }
  file.close();
  return escritas;
}

// Adiciona uma linha a um buffer circular
void adicionarLinhaCircular(char* buf, int rows, int lineLen, int &head, int &count, const char* linha) {
  int idx;
  if (count < rows) {
    idx = (head + count) % rows;
    count++;
  } else {
    idx = head;
    head = (head + 1) % rows;
  }
  char* destino = buf + ((size_t)idx * lineLen);
  strncpy(destino, linha, lineLen - 1);
  destino[lineLen - 1] = '\0';
}

// Flush: Transfere dados de buffers globais para locais (rápido) e grava no SD (Core 0)
void flushBuffers() {
  if (scanEmAndamento) {
    LOCK_MUTEX(serialMutex);
    Serial.println(F("Flush adiado: scan WiFi em andamento."));
    UNLOCK_MUTEX(serialMutex);
    return;
  }

  LOCK_MUTEX(serialMutex);
  Serial.println(F(">> GRAVANDO SD... NAO DESLIGAR! <<"));
  UNLOCK_MUTEX(serialMutex);
  
  bool falhaAlgum = false;

  // 1. Processa Log GPS
  logWriteCount = 0;
  LOCK_MUTEX(logMutex);
  if (logBufferCount > 0) {
    logWriteCount = logBufferCount;
    // Copia na ordem linear para o buffer de escrita
    for (int i = 0; i < logWriteCount; i++) {
      int idx = (logBufferHead + i) % LOG_BUFFER_MAX;
      memcpy(logWriteBuffer[i], logBuffer[idx], 160);
    }
    // Esvazia buffer global
    logBufferHead = 0;
    logBufferCount = 0;
  }
  UNLOCK_MUTEX(logMutex);

  if (logWriteCount > 0) {
    LOCK_MUTEX(serialMutex);
    Serial.printf("  log.txt: %d linhas... ", logWriteCount);
    UNLOCK_MUTEX(serialMutex);

    int escritas = appendLinhasWriteBuffer(logFileName, &logWriteBuffer[0][0], LOG_BUFFER_MAX, 160, logWriteCount);
    
    if (escritas == logWriteCount) {
      LOCK_MUTEX(serialMutex);
      Serial.println("OK");
      UNLOCK_MUTEX(serialMutex);
    } else {
      LOCK_MUTEX(serialMutex);
      Serial.printf("FALHOU (%d/%d gravadas)\n", escritas, logWriteCount);
      UNLOCK_MUTEX(serialMutex);
      
      // Devolve não escritas ao buffer global
      LOCK_MUTEX(logMutex);
      for (int i = escritas; i < logWriteCount; i++) {
        adicionarLinhaCircular(&logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount, logWriteBuffer[i]);
      }
      UNLOCK_MUTEX(logMutex);
      falhaAlgum = true;
    }
  }

  // 2. Processa Log WiFi
  wifiWriteCount = 0;
  LOCK_MUTEX(wifiMutex);
  if (wifiBufferCount > 0) {
    wifiWriteCount = wifiBufferCount;
    for (int i = 0; i < wifiWriteCount; i++) {
      int idx = (wifiBufferHead + i) % WIFI_BUFFER_MAX;
      memcpy(wifiWriteBuffer[i], wifiBuffer[idx], 256);
    }
    wifiBufferHead = 0;
    wifiBufferCount = 0;
  }
  UNLOCK_MUTEX(wifiMutex);

  if (wifiWriteCount > 0) {
    LOCK_MUTEX(serialMutex);
    Serial.printf("  wifi.txt: %d linhas... ", wifiWriteCount);
    UNLOCK_MUTEX(serialMutex);

    int escritas = appendLinhasWriteBuffer(wifiFileName, &wifiWriteBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiWriteCount);
    
    if (escritas == wifiWriteCount) {
      LOCK_MUTEX(serialMutex);
      Serial.println("OK");
      UNLOCK_MUTEX(serialMutex);
    } else {
      LOCK_MUTEX(serialMutex);
      Serial.printf("FALHOU (%d/%d gravadas)\n", escritas, wifiWriteCount);
      UNLOCK_MUTEX(serialMutex);

      LOCK_MUTEX(wifiMutex);
      for (int i = escritas; i < wifiWriteCount; i++) {
        adicionarLinhaCircular(&wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount, wifiWriteBuffer[i]);
      }
      UNLOCK_MUTEX(wifiMutex);
      falhaAlgum = true;
    }
  }

  if (falhaAlgum) {
    remontarSD();
    return;
  }

  LOCK_MUTEX(serialMutex);
  Serial.println(F(">> GRAVACAO CONCLUIDA. SEGURO DESLIGAR. <<"));
  UNLOCK_MUTEX(serialMutex);
}

// Escreve cabeçalho CSV em log.txt
void inicializarArquivoLog() {
  if (!sd.exists(logFileName)) {
    const char* cabecalho =
      "data_hora, lat, lon, sat, hdop, kmh, direcao, umidade, temp_dht,"
      " ac_x, ac_y, ac_z, gy_x, gy_y, gy_z\n";
    appendFile(logFileName, cabecalho);
    LOCK_MUTEX(serialMutex);
    Serial.println("Cabecalho CSV criado em log.txt");
    UNLOCK_MUTEX(serialMutex);
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

// Exibe dashboard ASCII no Serial Monitor (Seguro para multi-core usando local e mutex)
void exibirDashboard(const char* timeStamp, long lat, long lon,
                      int sat, float hdop, float kmh, const char* direcao,
                      float umidade, float tempDHT,
                      DadosMPU mediaMPU, WifiStats wifi, bool temFix) {
  int localLogCount = 0;
  int localWifiCount = 0;
  
  LOCK_MUTEX(logMutex);
  localLogCount = logBufferCount;
  UNLOCK_MUTEX(logMutex);
  
  LOCK_MUTEX(wifiMutex);
  localWifiCount = wifiBufferCount;
  UNLOCK_MUTEX(wifiMutex);

  char barra[LOG_BUFFER_MAX + 1];
  for (int i = 0; i < LOG_BUFFER_MAX; i++) {
    barra[i] = (i < localLogCount) ? '#' : '-';
  }
  barra[LOG_BUFFER_MAX] = '\0';

  LOCK_MUTEX(serialMutex);
  Serial.println(F("================================================"));

  if (temFix) {
    Serial.printf("  GPS   %s  Sat:%d  HDOP:%.1f\n", timeStamp, sat, hdop);
    Serial.printf("  Pos   Lat:%.6f  Lon:%.6f\n", lat / 1000000.0, lon / 1000000.0);
    Serial.printf("  Mov   Vel:%.1f km/h  Dir:%s\n", kmh, direcao);
  } else {
    Serial.println(F("  GPS   --- Aguardando fix ---"));
    Serial.println(F("  Pos   Lat:---  Lon:---"));
    Serial.println(F("  Mov   Vel:---  Dir:---"));
  }

  if (isnan(tempDHT) || isnan(umidade)) {
    Serial.println(F("  DHT   Temp:---  Umid:---"));
  } else {
    Serial.printf("  DHT   Temp:%.1f C  Umid:%.1f%%\n", tempDHT, umidade);
  }

  if (mpuDisponivel) {
    Serial.printf("  MPU   Ac(%.2f %.2f %.2f)  Gy(%.2f %.2f %.2f)\n",
                  mediaMPU.acX, mediaMPU.acY, mediaMPU.acZ,
                  mediaMPU.gyX, mediaMPU.gyY, mediaMPU.gyZ);
  } else {
    Serial.println(F("  MPU   --- Indisponivel ---"));
  }

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

  if (logParadoStart != 0) {
    long remSeg = (long)(WIFI_SLEEP_MS - (millis() - logParadoStart)) / 1000;
    if (remSeg < 0) remSeg = 0;
    Serial.printf("  LOG   Parado: gravacao em rajada em %lds\n", remSeg);
  } else {
    Serial.println(F("  LOG   Em movimento: gravacao normal (por buffer)"));
  }

  Serial.printf("  SD    [%s] %d/%d\n", barra, localLogCount, LOG_BUFFER_MAX);
  Serial.printf("  BUF   log:%d/%d  wifi:%d/%d  ssidCache:%d/%d\n",
                localLogCount, LOG_BUFFER_MAX,
                localWifiCount, WIFI_BUFFER_MAX,
                ssidCacheCount, SSID_CACHE_MAX);

  Serial.println(F("================================================"));
  UNLOCK_MUTEX(serialMutex);
}

// Varrer WiFi (Roda no Core 0)
WifiStats varrerWiFi(const char* timeStamp, long lat, long lon, float kmh) {
  static WifiStats lastStats = {0, 0, 0};

  if (wifiDormindo) {
    bool voltouAMover = kmh >= WIFI_SLEEP_KMH_THRESHOLD;
    bool tempoEsgotado = millis() - wifiSleepStart >= WIFI_SLEEP_MS;
    if (voltouAMover || tempoEsgotado) {
      WiFi.mode(WIFI_STA);
      WiFi.disconnect();
      wifiDormindo = false;
      LOCK_MUTEX(serialMutex);
      Serial.println(voltouAMover ? "WiFi acordado: veiculo em movimento."
                                   : "WiFi acordado: tempo de sono esgotado.");
      UNLOCK_MUTEX(serialMutex);
    } else {
      return lastStats;
    }
  }

  if (!scanEmAndamento) {
    WiFi.scanNetworks(true, true);
    scanEmAndamento = true;
    return lastStats;
  }

  int16_t n = WiFi.scanComplete();
  if (n == WIFI_SCAN_FAILED) {
    scanEmAndamento = false;
    return lastStats;
  }

  if (n == WIFI_SCAN_RUNNING) {
    return lastStats;
  }

  WifiStats stats = {n, 0, 0};

  for (int i = 0; i < n; ++i) {
    String ssidStr = WiFi.SSID(i);
    const char* ssid = ssidStr.c_str();

    if (ssidJaVista(ssid)) {
      stats.dup++;
      continue;
    }
    adicionarSSIDCache(ssid);

    char dadosWifi[256];
    snprintf(dadosWifi, sizeof(dadosWifi), "%s, %ld, %ld, %s, %d, %d, %s\n",
             timeStamp, lat, lon, ssid, (int)WiFi.RSSI(i),
             (int)WiFi.channel(i), obterTipoCriptografia(WiFi.encryptionType(i)));

    LOCK_MUTEX(wifiMutex);
    adicionarLinhaCircular(&wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount, dadosWifi);
    UNLOCK_MUTEX(wifiMutex);
    stats.novas++;
  }

  WiFi.scanDelete();
  scanEmAndamento = false;
  lastStats = stats;

  if (kmh < WIFI_SLEEP_KMH_THRESHOLD && stats.novas == 0) {
    WiFi.mode(WIFI_OFF);
    wifiDormindo = true;
    wifiSleepStart = millis();
    LOCK_MUTEX(serialMutex);
    Serial.println("WiFi dormindo: veiculo parado, sem redes novas.");
    UNLOCK_MUTEX(serialMutex);
  }

  return stats;
}

// ─────────────────────────────────────────────────────────────────────────────
// Tarefa 1: Sensores e GPS (Core 1 - APP_CPU)
void tarefaSensores(void *pvParameters) {
  (void) pvParameters;

  esp_task_wdt_add(NULL);

  LOCK_MUTEX(serialMutex);
  Serial.print("Tarefa de Sensores iniciada no nucleo: ");
  Serial.println(xPortGetCoreID());
  UNLOCK_MUTEX(serialMutex);

  unsigned long ultimaLeituraMPU = millis();
  DadosMPU somaMPU = {0, 0, 0, 0, 0, 0};
  int contadorMPU = 0;
  unsigned long startGPSCycle = millis();

  for (;;) {
    esp_task_wdt_reset();

    // Ingestão de dados GPS via Serial
    while (Serial2.available()) {
      char c = Serial2.read();
      gps.encode(c);
    }

    // Leitura rápida do MPU6050 (a cada 100ms)
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

    // A cada 1 segundo, processa o ciclo dos sensores
    if (millis() - startGPSCycle >= 1000) {
      long lat, lon;
      unsigned long fix_age, date, time;
      gps.get_position(&lat, &lon, &fix_age);
      gps.get_datetime(&date, &time, &fix_age);

      int sat = (gps.satellites() == TinyGPS::GPS_INVALID_SATELLITES) ? 0 : gps.satellites();
      float hdop = (gps.hdop() == TinyGPS::GPS_INVALID_HDOP) ? 0 : gps.hdop() / 100.0;
      float kmh = (gps.f_speed_kmph() == TinyGPS::GPS_INVALID_F_SPEED) ? 0.0 : gps.f_speed_kmph();
      const char* direcao = TinyGPS::cardinal(gps.f_course());

      float umidade = dht.readHumidity();
      float tempDHT = dht.readTemperature();

      // Média do MPU
      DadosMPU mediaMPU = {0, 0, 0, 0, 0, 0};
      if (contadorMPU > 0) {
        mediaMPU.acX = somaMPU.acX / contadorMPU;
        mediaMPU.acY = somaMPU.acY / contadorMPU;
        mediaMPU.acZ = somaMPU.acZ / contadorMPU;
        mediaMPU.gyX = somaMPU.gyX / contadorMPU;
        mediaMPU.gyY = somaMPU.gyY / contadorMPU;
        mediaMPU.gyZ = somaMPU.gyZ / contadorMPU;
      }
      
      // Reset acumuladores MPU
      somaMPU = {0, 0, 0, 0, 0, 0};
      contadorMPU = 0;

      bool temFix = (lat != TinyGPS::GPS_INVALID_ANGLE && lon != TinyGPS::GPS_INVALID_ANGLE && fix_age < 5000);

      char timeStamp[80] = "---";
      if (temFix) {
        int dia  = date / 10000;
        int mes  = (date / 100) % 100;
        int ano  = 2000 + (date % 100);
        int hora    = time / 1000000;
        int minuto  = (time / 10000) % 100;
        int segundo = (time / 100) % 100;

        struct tm t;
        t.tm_year  = ano - 1900;
        t.tm_mon   = mes - 1;
        t.tm_mday  = dia;
        t.tm_hour  = hora;
        t.tm_min   = minuto;
        t.tm_sec   = segundo;
        t.tm_isdst = -1;
        t.tm_hour -= 3; // Fuso UTC-3
        mktime(&t);

        snprintf(timeStamp, sizeof(timeStamp), "%02d/%02d/%04d %02d:%02d:%02d",
                 t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec);

        char logData[160];
        if (mpuDisponivel) {
          snprintf(logData, sizeof(logData),
                   "%s, %ld, %ld, %d, %.2f, %.2f, %s, %.1f, %.1f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f\n",
                   timeStamp, lat, lon, sat, hdop, kmh, direcao, umidade, tempDHT,
                   mediaMPU.acX, mediaMPU.acY, mediaMPU.acZ,
                   mediaMPU.gyX, mediaMPU.gyY, mediaMPU.gyZ);
        } else {
          snprintf(logData, sizeof(logData),
                   "%s, %ld, %ld, %d, %.2f, %.2f, %s, %.1f, %.1f, , , , , , \n",
                   timeStamp, lat, lon, sat, hdop, kmh, direcao, umidade, tempDHT);
        }

        LOCK_MUTEX(logMutex);
        adicionarLinhaCircular(&logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount, logData);
        UNLOCK_MUTEX(logMutex);
      }

      // Atualiza variáveis globais compartilhadas (para leitura no Core 0)
      LOCK_MUTEX(sensorMutex);
      globalMediaMPU = mediaMPU;
      globalUmidade = umidade;
      globalTempDHT = tempDHT;
      globalTemFix = temFix;
      strcpy(globalTimeStamp, timeStamp);
      globalLat = lat;
      globalLon = lon;
      globalSat = sat;
      globalHdop = hdop;
      globalKmh = kmh;
      strncpy(globalDirecao, direcao, sizeof(globalDirecao) - 1);
      globalDirecao[sizeof(globalDirecao) - 1] = '\0';
      UNLOCK_MUTEX(sensorMutex);

      startGPSCycle = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Tarefa 2: Armazenamento SD e WiFi (Core 0 - PRO_CPU)
void tarefaSDWifi(void *pvParameters) {
  (void) pvParameters;

  esp_task_wdt_add(NULL);

  LOCK_MUTEX(serialMutex);
  Serial.print("Tarefa de SD/WiFi iniciada no nucleo: ");
  Serial.println(xPortGetCoreID());
  UNLOCK_MUTEX(serialMutex);

  for (;;) {
    esp_task_wdt_reset();

    // Recupera variáveis globais dos sensores de forma segura
    char timeStamp[80];
    long lat, lon;
    int sat;
    float hdop, kmh;
    char direcao[6];
    DadosMPU mediaMPU;
    float umidade, tempDHT;
    bool temFix;

    // A leitura das variáveis globais compartilhadas é protegida por mutex
    LOCK_MUTEX(sensorMutex);
    temFix = globalTemFix;
    strcpy(timeStamp, globalTimeStamp);
    lat = globalLat;
    lon = globalLon;
    sat = globalSat;
    hdop = globalHdop;
    kmh = globalKmh;
    strcpy(direcao, globalDirecao);
    mediaMPU = globalMediaMPU;
    umidade = globalUmidade;
    tempDHT = globalTempDHT;
    UNLOCK_MUTEX(sensorMutex);

    WifiStats wifiStats = {0, 0, 0};
    if (temFix) {
      wifiStats = varrerWiFi(timeStamp, lat, lon, kmh);
    }

    // Exibe Dashboard ASCII
    exibirDashboard(timeStamp, lat, lon, sat, hdop, kmh, direcao,
                    umidade, tempDHT, mediaMPU, wifiStats, temFix);

    // Lógica de Flush e Gravação no SD
    int localLogCount = 0;
    LOCK_MUTEX(logMutex);
    localLogCount = logBufferCount;
    UNLOCK_MUTEX(logMutex);

    if (kmh > WIFI_SLEEP_KMH_THRESHOLD) {
      if (logParadoStart != 0) {
        flushBuffers();
        logParadoStart = 0;
      }
      if (localLogCount >= LOG_BUFFER_MAX) {
        flushBuffers();
      }
    } else {
      if (logParadoStart == 0) {
        flushBuffers();
        logParadoStart = millis();
      } else if (millis() - logParadoStart >= WIFI_SLEEP_MS) {
        flushBuffers();
        logParadoStart = millis();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial2.begin(GPS_Serial_Baud, SERIAL_8N1, GPS_RX, GPS_TX);

  // Inicialização de Mutexes
  logMutex = xSemaphoreCreateMutex();
  wifiMutex = xSemaphoreCreateMutex();
  serialMutex = xSemaphoreCreateMutex();
  sensorMutex = xSemaphoreCreateMutex();

  if (logMutex == NULL || wifiMutex == NULL || serialMutex == NULL || sensorMutex == NULL) {
    Serial.println("Erro critico: falha ao criar Mutexes.");
    while (1);
  }

  // Alocação dinâmica de buffers grandes no heap para economizar DRAM estática (BSS)
  logBuffer = (char (*)[160]) malloc(LOG_BUFFER_MAX * 160);
  wifiBuffer = (char (*)[256]) malloc(WIFI_BUFFER_MAX * 256);
  logWriteBuffer = (char (*)[160]) malloc(LOG_BUFFER_MAX * 160);
  wifiWriteBuffer = (char (*)[256]) malloc(WIFI_BUFFER_MAX * 256);

  if (logBuffer == NULL || wifiBuffer == NULL || logWriteBuffer == NULL || wifiWriteBuffer == NULL) {
    Serial.println("Erro critico: falha ao alocar buffers em heap.");
    while (1);
  }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  dht.begin();

  LOCK_MUTEX(serialMutex);
  Serial.println(F("\n--- ESP32 GPS Logger DUAL-CORE ---"));
  UNLOCK_MUTEX(serialMutex);

  if (!mpu.begin()) {
    LOCK_MUTEX(serialMutex);
    Serial.println("Aviso: MPU6050 nao encontrado. Dados de IMU desabilitados.");
    UNLOCK_MUTEX(serialMutex);
    mpuDisponivel = false;
  } else {
    mpu.setAccelerometerRange(MPU6050_RANGE_16_G);
    mpu.setGyroRange(MPU6050_RANGE_1000_DEG);
    mpu.setFilterBandwidth(MPU6050_BAND_44_HZ);
    mpuDisponivel = true;
    LOCK_MUTEX(serialMutex);
    Serial.println("MPU6050 inicializado.");
    UNLOCK_MUTEX(serialMutex);
  }

  // Inicialização do SD
  int tentativaSD = 0;
  while (!sd.begin(SD_CONFIG)) {
    tentativaSD++;
    LOCK_MUTEX(serialMutex);
    Serial.printf("Falha ao montar o cartao SD (tentativa %d). Tentando...\n", tentativaSD);
    UNLOCK_MUTEX(serialMutex);
    sd.end();
    delay(500);
  }

  LOCK_MUTEX(serialMutex);
  Serial.println("SD montado.");
  UNLOCK_MUTEX(serialMutex);

  inicializarArquivoLog();

  // Watchdog
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

  // Criação das tarefas
  xTaskCreatePinnedToCore(
      tarefaSensores,
      "Sensor_Task",
      8192,
      NULL,
      1,
      NULL,
      1 // Core 1 (APP)
  );

  xTaskCreatePinnedToCore(
      tarefaSDWifi,
      "SDWifi_Task",
      8192,
      NULL,
      1,
      NULL,
      0 // Core 0 (PRO)
  );

  LOCK_MUTEX(serialMutex);
  Serial.println("Tarefas dual-core criadas.");
  UNLOCK_MUTEX(serialMutex);
}

void loop() {
  // loopTask deleta a si mesma para liberar stack, pois usamos tarefas dedicadas
  vTaskDelete(NULL);
}
