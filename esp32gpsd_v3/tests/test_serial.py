"""Host regressions using real TinyGPS and functions extracted from the sketch.
Run: python3 esp32gpsd_v3/tests/test_serial.py
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SKETCH = ROOT / 'esp32gpsd_v3/esp32gpsd_v3.ino'


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class SerialRegression(unittest.TestCase):
    def test_console_download_history_and_timers(self):
        source = SKETCH.read_text()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            harness = r'''#include <cassert>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cmath>
using std::isfinite;
#include <cstring>
#include <string>
uint32_t now = 5000;
uint32_t millis() { return now; }
struct SerialMock {
  std::string output;
  template<class... A> void printf(const char* format, A... args) {
    char buffer[512];
    if constexpr (sizeof...(args) == 0) snprintf(buffer, sizeof(buffer), "%s", format);
    else snprintf(buffer, sizeof(buffer), format, args...);
    output += buffer;
  }
} Serial;
enum DownloadEstado { DOWNLOAD_OCIOSO, DOWNLOAD_INICIADO, DOWNLOAD_CONCLUIDO, DOWNLOAD_ERRO };
'''
            harness += source[source.index('std::atomic<int> downloadEstado'):source.index('std::atomic<uint32_t> perdidos')]
            harness += r'''
struct DadosMPU { float acX, acY, acZ, gyX, gyY, gyZ; };
#define MODO_MOVIMENTO 0
#define MODO_PARADO_SONO 2
#define MODO_PARADO_CHECK 3
#define MODO_PARADO_HOTSPOT 4
#define PARKED_SLEEP_MS 300000U
#define RADIO_SCAN_INTERVAL_MS 30000U
#define HOTSPOT_IDLE_MS 300000U
#define HOTSPOT_SSID "ESP32GPS-Logs"
#define LOG_BUFFER_MAX 150
#define WIFI_BUFFER_MAX 100
#define BLE_BUFFER_MAX 100
#define LOG_ADD_MOVING_MS 10000U
#define LOG_ADD_PARKED_MS 30000U
#define SSID_CACHE_MAX 500
#define BLE_CACHE_MAX 500
uint32_t bootCount = 1, modoInicio = 0, cicloNumero = 0, radioCicloInicio = 0, ultimoCicloFim = 0;
uint32_t ultimaVelocidadeMs = 0, ultimaPosicaoMs = 0, ultimoDHTValido = 0, lastLogAddMs = 0;
uint8_t modoAtual = MODO_MOVIMENTO;
bool radioCicloAtivo = false, radioCicloWifiDone = false, radioCicloBleDone = false;
bool temVelocidade = false, temPosicao = false, hotspotDisponivel = true;
bool dhtValido = false, mpuDisponivel = false, sdConfirmado = true, flushPendente = false;
bool scanEmAndamento = false, bleScanAtivo = false;
std::atomic<bool> hotspotAberto{false}, downloadAtivo{false};
std::atomic<uint32_t> ultimaAtividade{0}, perdidos{0};
float ultimaVelocidade = 0, dashTemp = 24.5, dashUmid = 51;
long lastLat = -23550520, lastLon = -46633308;
char lastTimeStamp[25] = "06/10/2026 00:00:00";
struct { int total = 12, novas = 10, dup = 2; } ultimoWifiStats;
int dashBle = 30, bleBufferizados = 25, bleDuplicados = 5;
int logBufferCount = 0, wifiBufferCount = 10, bleBufferCount = 25;
int falhasRemountSD = 0, ssidCacheCount = 12, bleCacheCount = 30;
struct { unsigned int softAPgetStationNum() { return 1; } } WiFi;
struct { unsigned int getFreeHeap() { return 182*1024; } unsigned int getMaxAllocHeap() { return 100*1024; } } ESP;
'''
            for signature in ('const char* modoNome(', 'void tempoTexto(', 'uint32_t tempoRestante(', 'void eventoSerial(',
                              'void linhaPainel(', 'const char* nomeDownload(', 'void finalizarDownload(',
                              'void imprimirAcessosWeb(', 'void imprimirDownload(', 'void exibirDashboard('):
                harness += '\n' + function(source, signature) + '\n'
            harness += r'''
int main() {
  char time[20]; tempoTexto(3661000, time, sizeof(time));
  assert(std::string(time) == "01:01:01");
  assert(tempoRestante(2000, 1000, 5000) == 4000);
  assert(tempoRestante(9000, 1000, 5000) == 0);
  assert(tempoRestante(50, 0xfffffff0U, 100) == 34); // millis rollover
  linhaPainel("%s", std::string(100, 'x').c_str());
  assert(Serial.output.size() == 81 && Serial.output.substr(78) == " |\n");
  Serial.output.clear(); loopNumero = 42; faseLoop = "WEB";
  webPaginas = 2; webDownloads = 1;
  imprimirAcessosWeb();
  assert(Serial.output.find("[00:00:05] [EVT] loop=42 WEB") != std::string::npos);
  assert(Serial.output.find("pagina +2 | download +1") != std::string::npos);
  auto before = Serial.output; imprimirAcessosWeb(); assert(before == Serial.output);
  downloadArquivo = 1; downloadTotal = 512; downloadEnviado = 512; downloadInicioMs = 4900;
  downloadEstado = DOWNLOAD_INICIADO;
  finalizarDownload(true); // download finishes before loop sees start
  imprimirDownload();
  assert(Serial.output.find("Download iniciado: log.txt") != std::string::npos);
  assert(Serial.output.find("Download concluido: log.txt | 512/512 bytes em 100ms") != std::string::npos);
  assert(downloadsOk == 1 && downloadEstado == DOWNLOAD_OCIOSO);
  assert(ultimoDownloadEstado == DOWNLOAD_CONCLUIDO && ultimoDownloadBytes == 512);
  before = Serial.output; imprimirDownload(); assert(before == Serial.output);
  now = 6000; downloadArquivo = 3; downloadInicioMs = 5500; downloadTotal = 1000; downloadEnviado = 20;
  downloadEstado = DOWNLOAD_INICIADO; imprimirDownload();
  finalizarDownload(false); imprimirDownload();
  assert(downloadsErro == 1 && ultimoDownloadEstado == DOWNLOAD_ERRO);
  assert(ultimoDownloadArquivo == 3 && ultimoDownloadBytes == 20 && ultimoDownloadDuracaoMs == 500);
  assert(Serial.output.find("Download erro: ble.txt | 20/1000 bytes em 500ms") != std::string::npos);
  Serial.output.clear(); faseLoop = "RESUMO"; loopInicioMs = 5000;
  exibirDashboard({});
  assert(Serial.output.find("MOVIMENTO") != std::string::npos);
  assert(Serial.output.find("heap=182KB max=100KB") != std::string::npos);
  assert(Serial.output.find("WEB ") == std::string::npos);  // hotspot fechado: sem linha WEB
  now = 40000; modoAtual = MODO_PARADO_HOTSPOT; hotspotAberto = true; temPosicao = true;
  Serial.output.clear(); exibirDashboard({});
  assert(Serial.output.find("PARADO (hotspot)") != std::string::npos);
  assert(Serial.output.find("clientes=1") != std::string::npos);
  assert(Serial.output.find("erro=1") != std::string::npos);
  now = 80000; downloadEstado = DOWNLOAD_INICIADO;  // painel mudo durante download
  Serial.output.clear(); exibirDashboard({});
  assert(Serial.output.empty());
  downloadEstado = DOWNLOAD_OCIOSO; now = 60000;  // hotspot aberto: intervalo de 30 s
  Serial.output.clear(); exibirDashboard({});
  assert(Serial.output.empty());
}
'''
            (tmp / 'main.cpp').write_text(harness)
            subprocess.run(['g++', '-std=c++17', str(tmp / 'main.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)

    def test_gps_sd_and_losses(self):
        source = SKETCH.read_text()
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            (tmp / 'Arduino.h').write_text('''#pragma once
#include <cmath>
#include <cstdint>
using byte = unsigned char;
using std::isfinite;
using std::isnan;
#define TWO_PI (2*M_PI)
unsigned long millis();
#define radians(v) ((v)*M_PI/180.0)
#define degrees(v) ((v)*180.0/M_PI)
#define sq(v) ((v)*(v))
''')
            harness = r'''
#include <cassert>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <string>
#include <ctime>
#include <vector>
#include "TinyGPS.h"
unsigned long now = 100;
unsigned long millis() { return now; }
TinyGPS gps;
struct LeituraGPS {
  long lat = TinyGPS::GPS_INVALID_ANGLE, lon = TinyGPS::GPS_INVALID_ANGLE;
  unsigned long date = TinyGPS::GPS_INVALID_DATE, time = TinyGPS::GPS_INVALID_TIME;
  unsigned long posMs = 0;
  unsigned short sat = TinyGPS::GPS_INVALID_SATELLITES;
  float hdop = NAN;
  const char* direcao = "";
} leituraGPS;
float ultimaVelocidade = NAN;
unsigned long ultimaVelocidadeMs = 0;
bool temVelocidade = false;
int decisoes = 0;
void atualizarModo(float) { decisoes++; }
struct { template<class... A> void printf(const char*, A...) {} } Serial;
template<class... A> void eventoSerial(const char*, const char*, A...) {}
std::atomic<uint32_t> perdidos{0};
bool sdConfirmado = false;
struct FsFile {
  bool opened = true, syncOk = true, closeOk = true;
  bool shortWrite = false;
  int writes = 0, failAfter = -1;
  int syncCalls = 0, closeCalls = 0;
  operator bool() const { return opened; }
  size_t write(const char*, size_t n) {
    bool fail = shortWrite || (failAfter >= 0 && writes >= failAfter);
    writes++; return n - (fail && n > 0);
  }
  bool sync() { syncCalls++; return syncOk; }
  bool close() { closeCalls++; return closeOk; }
};
struct { FsFile next; FsFile open(const char*, int) { return next; } } sd;
const int O_WRONLY = 1, O_CREAT = 2, O_APPEND = 4;
struct DadosMPU { float acX, acY, acZ, gyX, gyY, gyZ; };
float dashTemp = NAN, dashUmid = NAN;
bool mpuDisponivel = false, temPosicao = false;
long lastLat = 0, lastLon = 0;
unsigned long ultimaPosicaoMs = 0, lastLogAddMs = 0;
char lastTimeStamp[25] = "";
char logRows[150][160];
char (*logBuffer)[160] = logRows;
int logBufferHead = 0, logBufferCount = 0;
int modoAtual = 0;
#define MODO_MOVIMENTO 0
#define LOG_ADD_MOVING_MS 10000UL
#define LOG_ADD_PARKED_MS 30000UL
#define LOG_BUFFER_MAX 150
'''
            for signature in ('bool numeroNMEA(', 'bool receberGPS(', 'bool confirmarArquivo(', 'bool appendFile(',
                              'int appendLinhasCirculares(', 'void adicionarLinhaCircular(',
                              'void campoFloat(', 'void processarDadosGPS(', 'uint32_t hashString(', 'bool hashJaVisto(', 'bool adicionarHashCache('):
                harness += '\n' + function(source, signature) + '\n'
            harness += r'''
bool feed(const std::string& body, bool corrupt = false) {
  unsigned char checksum = 0;
  for (char c : body) checksum ^= c;
  if (corrupt) checksum ^= 1;
  char tail[8]; snprintf(tail, sizeof(tail), "*%02X\r\n", checksum);
  bool valid = false;
  for (char c : "$" + body + tail) valid = receberGPS(c) || valid;
  return valid;
}
std::vector<std::string> csv(const char* line) {
  std::vector<std::string> fields;
  const char* start = line;
  for (const char* p = line; ; ++p) {
    if (*p == ',' || *p == '\n' || !*p) {
      std::string field(start, p - start);
      auto first = field.find_first_not_of(' ');
      fields.push_back(first == std::string::npos ? "" : field.substr(first));
      if (*p != ',') return fields;
      start = p + 1;
    }
  }
}
int main() {
  const std::string rmc = "GPRMC,120000.00,A,2355.0000,S,04638.0000,W,10.0,90.0,061026,,,A";
  assert(feed(rmc));
  assert(decisoes == 1 && temVelocidade && fabs(ultimaVelocidade - 18.52) < 0.01);
  now = 3500;
  assert(feed("GPGGA,120001.00,2355.0000,S,04638.0000,W,1,08,0.9,10.0,M,0,M,,"));
  assert(decisoes == 1 && ultimaVelocidadeMs == 100);
  assert(now - ultimaVelocidadeMs >= 3000);
  assert(feed("GPRMC,120002.00,A,2355.0000,S,04638.0000,W,,90.0,061026,,,A"));
  assert(decisoes == 1 && ultimaVelocidadeMs == 100);
  assert(!feed(rmc, true));
  assert(!feed("GPRMC,120003.00,V,2355.0000,S,04638.0000,W,0.0,90.0,061026,,,A"));
  assert(decisoes == 1);
  now = 3600;
  assert(feed("GPRMC,120004.00,A,2355.0000,S,04638.0000,W,0.0,90.0,061026,,,A"));
  assert(decisoes == 2 && ultimaVelocidade == 0 && ultimaVelocidadeMs == now);
  // Missing coordinates cannot renew speed or contaminate accepted snapshot.
  long acceptedLat = leituraGPS.lat;
  assert(!feed("GPRMC,120005.00,A,,S,04638.0000,W,100.0,90.0,061026,,,A"));
  assert(decisoes == 2 && leituraGPS.lat == acceptedLat);
  assert(!feed("GPRMC,120005.00,A,junk,S,04638.0000,W,100.0,90.0,061026,,,A"));
  assert(decisoes == 2 && leituraGPS.lat == acceptedLat);
  setenv("TZ", "UTC0", 1); tzset();
  now = 20000;
  assert(feed("GPRMC,010000.00,A,2355.0000,S,04638.0000,W,,,010126,,,A"));
  processarDadosGPS({});
  assert(logBufferCount == 1);
  auto fields = csv(logRows[0]);
  assert(fields.size() == 15);
  assert(fields[0] == "31/12/2025 22:00:00");
  assert(fields[1] == "-23916667" && fields[2] == "-46633333");
  for (int i = 5; i < 15; ++i) assert(fields[i].empty());
  assert(fields[3] == "8" && fields[4] == "0.90");
  now = 40000;
  assert(feed("GPRMC,010000.00,A,2355.0000,S,04638.0000,W,10.0,90.0,310226,,,A"));
  processarDadosGPS({});
  assert(logBufferCount == 1);  // invalid February date
  assert(feed("GPGGA,120001.00,2355.0000,S,04638.0000,W,1,,,10.0,M,0,M,,"));
  assert(leituraGPS.sat == TinyGPS::GPS_INVALID_SATELLITES && isnan(leituraGPS.hdop));
  FsFile f;
  assert(confirmarArquivo(f, true, "/test") && f.syncCalls == 1 && f.closeCalls == 1);
  f = FsFile(); f.syncOk = false;
  assert(!confirmarArquivo(f, true, "/test") && f.closeCalls == 1);
  f = FsFile(); f.closeOk = false;
  assert(!confirmarArquivo(f, true, "/test"));
  char rows[2][16] = {"first\n", "second\n"};
  for (int failure = 0; failure < 4; failure++) {
    sd.next = FsFile();
    if (failure == 0) sd.next.opened = false;
    if (failure == 1) sd.next.shortWrite = true;
    if (failure == 2) sd.next.syncOk = false;
    if (failure == 3) sd.next.closeOk = false;
    assert(appendLinhasCirculares("/test", &rows[0][0], 2, 16, 1, 2) == 0);
    assert(!sdConfirmado);
    assert(!appendFile("/test", "header\n"));
  }
  sd.next = FsFile(); sd.next.failAfter = 1;
  assert(appendLinhasCirculares("/test", &rows[0][0], 2, 16, 1, 2) == 0);
  sd.next = FsFile();
  assert(appendLinhasCirculares("/test", &rows[0][0], 2, 16, 1, 2) == 2);
  assert(appendFile("/test", "header\n"));
  int head = 0, count = 2;
  adicionarLinhaCircular(&rows[0][0], 2, 16, head, count, "new\n");
  assert(head == 1 && count == 2 && perdidos == 1);
  uint32_t cache[1]; int cacheCount = 0; bool warned = false;
  assert(adicionarHashCache(cache, cacheCount, 1, "known", warned, "test"));
  assert(!adicionarHashCache(cache, cacheCount, 1, "new", warned, "test"));
  assert(cacheCount == 1 && hashJaVisto(cache, cacheCount, "known"));
  assert(!hashJaVisto(cache, cacheCount, "new"));
}
'''
            (tmp / 'main.cpp').write_text(harness)
            subprocess.run(['g++', '-std=c++17', '-DARDUINO=100', '-I' + str(tmp),
                            '-I' + str(ROOT / 'libraries/TinyGPS/src'), str(tmp / 'main.cpp'),
                            str(ROOT / 'libraries/TinyGPS/src/TinyGPS.cpp'), '-o', str(tmp / 'test')], check=True)
            subprocess.run([str(tmp / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
