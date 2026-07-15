// Teste standalone (sem Arduino) da lógica de buffer circular usada em
// esp32gpsd.ino (adicionarLinhaCircular / montarBloco).
// Compilar e rodar: g++ -std=c++11 test_circular_buffer.cpp -o /tmp/t && /tmp/t
#include <cassert>
#include <cstring>
#include <cstdio>

size_t montarBloco(char* buf, int rows, int lineLen, int head, int count, char* out, size_t outSize) {
  size_t pos = 0;
  for (int i = 0; i < count; i++) {
    const char* linha = buf + ((size_t)((head + i) % rows) * lineLen);
    size_t len = strlen(linha);
    if (pos + len >= outSize) break;
    memcpy(out + pos, linha, len);
    pos += len;
  }
  out[pos] = '\0';
  return pos;
}

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

int main() {
  const int ROWS = 3, LINE_LEN = 16;
  char buf[ROWS][LINE_LEN];
  int head = 0, count = 0;

  adicionarLinhaCircular(&buf[0][0], ROWS, LINE_LEN, head, count, "A\n");
  adicionarLinhaCircular(&buf[0][0], ROWS, LINE_LEN, head, count, "B\n");
  assert(count == 2 && head == 0);

  char out[64];
  montarBloco(&buf[0][0], ROWS, LINE_LEN, head, count, out, sizeof(out));
  assert(strcmp(out, "A\nB\n") == 0);

  // Enche o buffer (capacidade 3)
  adicionarLinhaCircular(&buf[0][0], ROWS, LINE_LEN, head, count, "C\n");
  assert(count == 3 && head == 0);

  // Buffer cheio: próxima linha descarta a mais antiga ("A")
  adicionarLinhaCircular(&buf[0][0], ROWS, LINE_LEN, head, count, "D\n");
  assert(count == 3 && head == 1);

  montarBloco(&buf[0][0], ROWS, LINE_LEN, head, count, out, sizeof(out));
  assert(strcmp(out, "B\nC\nD\n") == 0);

  printf("OK: buffer circular preserva as linhas mais recentes.\n");
  return 0;
}
