#define DISABLE_FS_H_WARNING  // Disable warning for type File not defined.
#include "SdFat.h"
#include "SPI.h"

// CS Pin. Padrão SPI no ESP32 é GPIO 5
const uint8_t SD_CS_PIN = 5;

// Velocidade do clock SPI. Se houver falhas, tente reduzir para SD_SCK_MHZ(10) ou SD_SCK_MHZ(4)
#define SPI_CLOCK SD_SCK_MHZ(16)

// Configuração SPI do SdFat
#define SD_CONFIG SdSpiConfig(SD_CS_PIN, SHARED_SPI, SPI_CLOCK)

SdFs sd;
FsFile file;

void setup() {
  Serial.begin(115200);
  while (!Serial) {
    yield();
  }
  
  delay(1000);
  Serial.println(F("\n--- Teste de Leitura SdFat no ESP32 ---"));

  // Inicializa o cartão SD
  Serial.print(F("Inicializando o cartao SD... "));
  if (!sd.begin(SD_CONFIG)) {
    Serial.println(F("FALHA!"));
    sd.initErrorHalt(&Serial);
    return;
  }
  Serial.println(F("SUCESSO!"));

  // Exibe informações do cartão e sistema de arquivos
  Serial.print(F("Tipo de FAT: FAT"));
  Serial.println(int(sd.vol()->fatType()));
  Serial.print(F("Tamanho do Volume: "));
  uint32_t sizeMB = sd.vol()->sectorsPerCluster() * sd.vol()->clusterCount();
  sizeMB /= 2048; // Setores de 512 bytes -> MB
  Serial.print(sizeMB);
  Serial.println(F(" MB"));

  // Lista os arquivos no diretório raiz
  Serial.println(F("\n--- Lista de Arquivos no Raiz ---"));
  sd.ls(&Serial, LS_SIZE | LS_R);
  Serial.println(F("--------------------------------"));

  // Tenta abrir o arquivo log.txt para leitura
  const char* targetFile = "log.txt";
  if (sd.exists(targetFile)) {
    Serial.printf("\nLendo o arquivo: %s\n", targetFile);
    Serial.println(F("--- Comeco do Arquivo ---"));
    
    if (file.open(targetFile, O_RDONLY)) {
      char line[128];
      int lineCount = 0;
      
      // Lê o arquivo linha por linha
      while (file.available()) {
        int n = file.fgets(line, sizeof(line));
        if (n <= 0) {
          Serial.println(F("\n[Erro ou Fim de Arquivo ao ler linha]"));
          break;
        }
        // Imprime a linha
        Serial.print(line);
        lineCount++;
        
        // Limita a exibição inicial para não inundar o serial
        if (lineCount >= 50) {
          Serial.println(F("\n... [Mais linhas disponiveis, limitando a 50 no teste] ..."));
          break;
        }
      }
      file.close();
      Serial.println(F("\n--- Fim do Arquivo ---"));
    } else {
      Serial.println(F("Erro ao abrir arquivo para leitura!"));
    }
  } else {
    Serial.printf("\nArquivo %s nao encontrado no cartao SD.\n", targetFile);
  }

  // Tenta abrir o arquivo wifi.txt para leitura se log.txt não existir ou depois de ler
  targetFile = "wifi.txt";
  if (sd.exists(targetFile)) {
    Serial.printf("\nLendo o arquivo: %s\n", targetFile);
    Serial.println(F("--- Comeco do Arquivo ---"));
    
    if (file.open(targetFile, O_RDONLY)) {
      char line[128];
      int lineCount = 0;
      
      while (file.available()) {
        int n = file.fgets(line, sizeof(line));
        if (n <= 0) {
          Serial.println(F("\n[Erro ou Fim de Arquivo ao ler linha]"));
          break;
        }
        Serial.print(line);
        lineCount++;
        
        if (lineCount >= 50) {
          Serial.println(F("\n... [Mais linhas disponiveis, limitando a 50 no teste] ..."));
          break;
        }
      }
      file.close();
      Serial.println(F("\n--- Fim do Arquivo ---"));
    } else {
      Serial.println(F("Erro ao abrir arquivo para leitura!"));
    }
  }

  Serial.println(F("\nTeste concluido."));
}

void loop() {
  // Nada a fazer no loop
}
