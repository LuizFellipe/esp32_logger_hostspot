# Guia Detalhado: Uso de Dual Core no ESP32 com Arduino IDE

O ESP32 possui dois núcleos de processamento (Core 0 e Core 1). A Arduino IDE executa nativamente o código (`setup()` e `loop()`) no **Core 1**, enquanto o **Core 0** geralmente é dedicado para tarefas de rede, como Wi-Fi e Bluetooth.

Para tirar proveito de ambos os núcleos, utilizamos o **FreeRTOS** (Real-Time Operating System), que já vem integrado ao pacote ESP32 para Arduino.

## 1. Identificando o Core Atual

Para descobrir em qual core uma tarefa está rodando, utilizamos a função:
```cpp
xPortGetCoreID();
```
Exemplo no `setup()`:
```cpp
void setup() {
  Serial.begin(115200);
  Serial.print("setup() está rodando no Core: ");
  Serial.println(xPortGetCoreID());
}
```

## 2. Criando Tarefas (Tasks) no FreeRTOS

Para rodar código em um core específico, precisamos criar uma Task utilizando a função `xTaskCreatePinnedToCore()`.

### Estrutura da Tarefa (Função)

Primeiro, você deve criar a função que será executada. Essa função precisa ter um loop infinito (como o `loop()`) e deve conter um `delay()` do FreeRTOS ou yield para permitir que outras tarefas rodem e evitar acionar o Watchdog Timer (WDT).

```cpp
void nomeDaTarefa(void * parameter) {
  for(;;) { // Loop infinito
    Serial.print("Rodando no Core: ");
    Serial.println(xPortGetCoreID());
    
    // Seu código aqui
    
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Delay de 1 segundo
  }
}
```

### Inicializando a Tarefa no Core Desejado

No `setup()`, você deve chamar a função `xTaskCreatePinnedToCore` para alocar a tarefa.

**Sintaxe:**
```cpp
TaskHandle_t Task1; // Variável para controlar a tarefa

void setup() {
  xTaskCreatePinnedToCore(
    nomeDaTarefa,   /* 1. Função da tarefa */
    "Tarefa_1",     /* 2. Nome da tarefa (string) */
    10000,          /* 3. Tamanho da pilha (Stack) em bytes */
    NULL,           /* 4. Parâmetros a serem passados */
    1,              /* 5. Prioridade da tarefa (0 a N, onde números maiores = maior prioridade) */
    &Task1,         /* 6. Handle da tarefa para acompanhamento (opcional) */
    0);             /* 7. Core onde irá rodar (0 ou 1) */
}
```

## 3. Resumo dos Parâmetros

| Parâmetro | Descrição |
|-----------|-------------|
| **TaskFunction_t** | Nome da função que contém o código da tarefa. |
| **const char * const pcName** | Nome amigável da tarefa. Útil para debug. |
| **const uint32_t usStackDepth** | Memória (stack) alocada para a tarefa (ex: 10000 bytes). |
| **void * const pvParameters** | Ponteiro para parâmetros. `NULL` se nenhum. |
| **UBaseType_t uxPriority** | Prioridade. `1` é uma boa prioridade base. O RTOS decide quem roda. |
| **TaskHandle_t * const pvCreatedTask** | Endereço do handle (`&Task1`). Permite pausar/deletar a tarefa. |
| **const BaseType_t xCoreID** | ID do Core: `0` ou `1`. |

## 4. Dicas e Boas Práticas

- **Watchdog Timer (WDT):** As tarefas do FreeRTOS não devem bloquear o core indefinidamente. Sempre use `vTaskDelay()` dentro dos loops infinitos das suas tarefas para permitir a execução de tarefas internas do sistema.
- **Variáveis Compartilhadas:** Ao acessar as mesmas variáveis (ou hardware, como Serial, I2C, SPI) de ambos os núcleos simultaneamente, podem ocorrer colisões de dados. Use **Semáforos** (Semaphores/Mutex) ou **Filas** (Queues) do FreeRTOS para coordenar o acesso.
- **Divisão de Carga:** Deixe o Core 0 livre para funções lentas ou em background e o Core 1 para processamento crítico, mas lembre-se que se usar Wi-Fi, o Core 0 lidará com a pilha TCP/IP.

## 5. Comunicação Segura Entre Cores

Para evitar corrupção de dados ao acessar a mesma variável de dois núcleos diferentes simultaneamente, utilize **Filas (Queues)** ou **Semáforos (Mutexes)**.

### Filas (Queues)
Filas são a melhor forma de passar dados entre tarefas com segurança.

```cpp
QueueHandle_t filaDados;

void setup() {
  // Cria fila para 10 inteiros
  filaDados = xQueueCreate(10, sizeof(int));
}

// Para enviar dados (ex: Core 0)
int valorSensor = 42;
xQueueSend(filaDados, &valorSensor, portMAX_DELAY);

// Para receber dados (ex: Core 1)
int valorRecebido;
if (xQueueReceive(filaDados, &valorRecebido, portMAX_DELAY)) {
  Serial.println(valorRecebido);
}
```

### Semáforos Mutex
Ideais para proteger um recurso compartilhado (como a porta Serial ou uma variável global) que ambas as tarefas precisam acessar.

```cpp
SemaphoreHandle_t mutexSerial;

void setup() {
  mutexSerial = xSemaphoreCreateMutex();
}

void tarefaSegura() {
  // Pega o mutex (aguarda até estar disponível)
  if (xSemaphoreTake(mutexSerial, portMAX_DELAY)) {
    Serial.println("Acesso exclusivo à Serial!");
    // Libera o mutex
    xSemaphoreGive(mutexSerial);
  }
}
```

## 6. Exemplo Completo

Neste exemplo, o **Core 0** lê um valor fictício de um sensor a cada 2 segundos e envia por uma Fila. O **Core 1** (no loop principal) recebe o valor e imprime na Serial protegida por um Mutex.

```cpp
#include <Arduino.h>

TaskHandle_t TaskSensor;
QueueHandle_t filaSensores;
SemaphoreHandle_t mutexSerial;

// Tarefa que rodará no Core 0
void lerSensorCore0(void * parameter) {
  for(;;) {
    int valorLido = random(0, 100); // Simula leitura
    
    // Tenta enviar para a fila
    xQueueSend(filaSensores, &valorLido, portMAX_DELAY);
    
    vTaskDelay(2000 / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);
  
  // Cria recursos do RTOS
  filaSensores = xQueueCreate(10, sizeof(int));
  mutexSerial = xSemaphoreCreateMutex();

  // Cria a tarefa no Core 0
  xTaskCreatePinnedToCore(
    lerSensorCore0, 
    "TaskSensor", 
    2000, 
    NULL, 
    1, 
    &TaskSensor, 
    0 // Core 0
  );
}

// O loop nativo roda no Core 1
void loop() {
  int valorRecebido;
  
  // Verifica se há dados na fila
  if (xQueueReceive(filaSensores, &valorRecebido, 0) == pdTRUE) {
    
    // Protege acesso à Serial com Mutex
    if (xSemaphoreTake(mutexSerial, portMAX_DELAY)) {
      Serial.print("Core ");
      Serial.print(xPortGetCoreID());
      Serial.print(" recebeu o valor: ");
      Serial.println(valorRecebido);
      xSemaphoreGive(mutexSerial);
    }
  }
  
  vTaskDelay(10 / portTICK_PERIOD_MS); // Pequeno delay
}
```
