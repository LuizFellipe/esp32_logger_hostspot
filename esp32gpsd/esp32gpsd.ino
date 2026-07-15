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

// Configuração de buffer (buffers circulares por linha: se o SD ficar
// indisponível e o buffer encher, as linhas mais antigas são descartadas
// para abrir espaço às mais recentes)
#define LOG_BUFFER_MAX    150    // Linhas de log GPS acumuladas antes de flush
#define WIFI_BUFFER_MAX   100    // Linhas de log WiFi acumuladas antes de flush
#define SSID_CACHE_MAX    64     // Máx SSIDs rastreadas para deduplicação

// Watchdog: se o loop() não "alimentar" o watchdog nesse tempo, o ESP32
// assume que travou (SD/I2C pendurado etc.) e reseta sozinho.
#define WDT_TIMEOUT_S 15

// Se o remount do SD falhar essa quantidade de vezes seguidas, reinicia
// o ESP32 inteiro (na esperança de que um boot limpo destrave o hardware).
#define SD_REMOUNT_MAX_FALHAS 10

// Sono do WiFi: parado (abaixo desse km/h) e sem SSID novo -> desliga o
// rádio WiFi por WIFI_SLEEP_MS pra evitar scan (pico de corrente) coincidindo
// com escrita no SD, que já causou brownout/travamento em campo.
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

// ─────────────────────────────────────────────────────────────────────────────
// Buffer de log GPS em RAM — circular por linha.
// Só é esvaziado quando a escrita no SD é confirmada. Se o SD ficar
// indisponível e o buffer encher, a linha mais antiga é descartada para
// abrir espaço para a mais nova (sempre preserva o trecho mais recente).
char logBuffer[LOG_BUFFER_MAX][160];   // 160 bytes/linha com margem
int  logBufferHead  = 0;               // Índice da linha mais antiga
int  logBufferCount = 0;               // Linhas acumuladas

// Buffer WiFi em RAM — mesma lógica circular por linha
char wifiBuffer[WIFI_BUFFER_MAX][256];
int  wifiBufferHead  = 0;              // Índice da linha mais antiga
int  wifiBufferCount = 0;              // Linhas acumuladas

// Cache de SSIDs já gravadas — nunca reseta (nem no flush, nem quando o
// WiFi acorda do sono). Uma rede só é gravada em wifi.txt 1 vez até
// SSID_CACHE_MAX encher ou o ESP32 reiniciar; evita duplicar nome de rede
// no log mesmo depois de vários ciclos de sono/flush.
char ssidCache[SSID_CACHE_MAX][33];    // 32 chars max SSID + null
int  ssidCacheCount = 0;

// Estado do sono do WiFi (ver WIFI_SLEEP_KMH_THRESHOLD / WIFI_SLEEP_MS)
bool wifiDormindo = false;
unsigned long wifiSleepStart = 0;

// Timer próprio do log.txt (desacoplado do timer do WiFi acima, pra não
// interferir na lógica de despertar dele) — enquanto parado, log.txt só
// grava em rajada a cada WIFI_SLEEP_MS; 0 = não está no modo "parado".
unsigned long logParadoStart = 0;

// true enquanto um WiFi.scanNetworks() assíncrono está em andamento —
// global (não static local) pra flushBuffers() poder checar antes de gravar
// no SD e evitar coincidir escrita física com o pico de corrente do scan.
bool scanEmAndamento = false;

// ─────────────────────────────────────────────────────────────────────────────
// Verifica se uma SSID já está no cache
// Retorna true se já foi vista (duplicata)
bool ssidJaVista(const char* ssid) {
  for (int i = 0; i < ssidCacheCount; i++) {
    if (strcmp(ssidCache[i], ssid) == 0) {
      return true;
    }
  }
  return false;
}

// Adiciona SSID ao cache (se couber)
void adicionarSSIDCache(const char* ssid) {
  if (ssidCacheCount < SSID_CACHE_MAX) {
    strncpy(ssidCache[ssidCacheCount], ssid, 32);
    ssidCache[ssidCacheCount][32] = '\0';
    ssidCacheCount++;
  }
  // Cache cheio → ignora silenciosamente (redes novas passam direto)
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
// bloco inteiro em RAM antes — economiza rows*lineLen bytes de RAM estática
// por buffer (relevante com LOG_BUFFER_MAX/WIFI_BUFFER_MAX grandes).
// Recebe o buffer como ponteiro plano (base + stride) em vez de template
// com referência a array — o pré-processador do Arduino IDE (ctags) não
// gera corretamente o protótipo automático para esse tipo de assinatura.
bool appendLinhasCirculares(const char* path, char* buf, int rows, int lineLen, int head, int count) {
  FsFile file = sd.open(path, O_WRONLY | O_CREAT | O_APPEND);
  if (!file) {
    Serial.print("Aviso: ");
    Serial.print(path);
    Serial.println(" nao disponivel para escrita.");
    return false;
  }

  bool ok = true;
  for (int i = 0; i < count && ok; i++) {
    const char* linha = buf + ((size_t)((head + i) % rows) * lineLen);
    ok = file.print(linha) > 0;
  }
  if (!ok) {
    Serial.print("Erro ao gravar em: ");
    Serial.println(path);
  }
  file.close();
  return ok;
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

// Flush: grava ambos buffers no SD.
// Só esvazia cada buffer se a escrita correspondente foi confirmada; em
// caso de falha, os dados ficam retidos no buffer circular e o SD é
// remontado, para o próximo ciclo tentar gravar de novo sem perder nada.
void flushBuffers() {
  // Não grava no SD com scan WiFi em andamento: os dois picos de corrente
  // juntos (rádio + escrita física) já causaram brownout/travamento em
  // campo. Adia pro próximo ciclo — buffer circular tolera o atraso.
  if (scanEmAndamento) {
    Serial.println(F("Flush adiado: scan WiFi em andamento."));
    return;
  }

  Serial.println(F(">> GRAVANDO SD... NAO DESLIGAR! <<"));
  bool falhaAlgum = false;

  // Flush buffer de log GPS
  if (logBufferCount > 0) {
    Serial.printf("  log.txt: %d linhas... ", logBufferCount);
    if (appendLinhasCirculares(logFileName, &logBuffer[0][0], LOG_BUFFER_MAX, 160, logBufferHead, logBufferCount)) {
      Serial.println("OK");
      logBufferHead  = 0;
      logBufferCount = 0;
    } else {
      Serial.println("FALHOU (dados mantidos em buffer)");
      falhaAlgum = true;
    }
  }

  // Flush buffer WiFi
  if (wifiBufferCount > 0) {
    Serial.printf("  wifi.txt: %d linhas... ", wifiBufferCount);
    if (appendLinhasCirculares(wifiFileName, &wifiBuffer[0][0], WIFI_BUFFER_MAX, 256, wifiBufferHead, wifiBufferCount)) {
      Serial.println("OK");
      wifiBufferHead  = 0;
      wifiBufferCount = 0;
    } else {
      Serial.println("FALHOU (dados mantidos em buffer)");
      falhaAlgum = true;
    }
  }

  if (falhaAlgum) {
    remontarSD();  // tenta recuperar pro próximo ciclo
    return;
  }

  Serial.println(F(">> GRAVACAO CONCLUIDA. SEGURO DESLIGAR. <<"));
}

// Escreve cabeçalho CSV em log.txt caso o arquivo ainda não exista
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

  // Linha WiFi
  if (temFix) {
    Serial.printf("  WiFi  %d redes | %d novas | %d dup\n",
                  wifi.total, wifi.novas, wifi.dup);
  } else {
    Serial.println(F("  WiFi  --- (sem fix GPS) ---"));
  }

  // Linha estado do sono do WiFi (ON/OFF + contagem regressiva)
  if (wifiDormindo) {
    long remSeg = (long)(WIFI_SLEEP_MS - (millis() - wifiSleepStart)) / 1000;
    if (remSeg < 0) remSeg = 0;
    Serial.printf("  RF    WiFi:OFF (dormindo)  acorda em %lds ou ao mover\n", remSeg);
  } else {
    Serial.println(F("  RF    WiFi:ON"));
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
    const char* ssid = WiFi.SSID(i).c_str();

    // Deduplicação: ignora SSIDs já gravadas (cache nunca reseta)
    if (ssidJaVista(ssid)) {
      stats.dup++;
      continue;
    }
    adicionarSSIDCache(ssid);

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

  // Scan WiFi acumula no wifiBuffer (dorme sozinho se parado e sem redes novas)
  WifiStats wifiStats = varrerWiFi(timeStamp, lat, lon, kmh);

  // Exibe dashboard no Serial Monitor
  exibirDashboard(timeStamp, lat, lon, sat, hdop, kmh, direcao,
                  umidade, tempDHT, mediaMPU, wifiStats, true);

  // Flush do log.txt: cadência normal (por contagem) em movimento; parado,
  // vira rajada única a cada WIFI_SLEEP_MS (mesma regra do sono do WiFi,
  // timer próprio pra não interferir no wifiSleepStart).
  if (kmh > WIFI_SLEEP_KMH_THRESHOLD) {
    logParadoStart = 0;
    if (logBufferCount >= LOG_BUFFER_MAX) {
      flushBuffers();
    }
  } else {
    if (logParadoStart == 0) {
      logParadoStart = millis(); // acabou de parar, começa a espera
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

  // Inicialização do WiFi no modo Estação
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  // Inicialização do DHT22
  dht.begin();

  Serial.println(F("\n--- ESP32 GPS Logger com SD, DHT22, MPU6050 e WiFi ---"));
  Serial.printf("Buffer log: %d linhas | Buffer WiFi: %d linhas | Cache SSID: %d entradas\n",
                LOG_BUFFER_MAX, WIFI_BUFFER_MAX, SSID_CACHE_MAX);

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
  // ciclos de leitura do cartão; vida útil do SD importa mais que a
  // contagem de linhas no boot, que era só curiosidade no Serial).
  Serial.printf("Arquivo %s: %s\n", logFileName, sd.exists(logFileName) ? "encontrado" : "sera criado na primeira gravacao");
  Serial.printf("Arquivo %s: %s\n", wifiFileName, sd.exists(wifiFileName) ? "encontrado" : "sera criado na primeira gravacao");

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
