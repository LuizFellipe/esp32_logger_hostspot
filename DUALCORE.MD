# Guia Técnico Avançado: Concorrência, Mutex e Dual-Core no ESP32

Este guia foi elaborado como uma referência definitiva de programação concorrente e de baixo nível para a plataforma ESP32 (especificamente o ESP32 Dev Kit V1) [631]. Ele aborda as nuances do sistema operacional de tempo real (FreeRTOS) integrado ao ESP32 e como a Arduino IDE interage com a arquitetura dual-core Xtensa LX6 [262, 264, 631].

---

## 1. Introdução Arquitetural Avançada do ESP32

O ESP32 é alimentado por dois microprocessadores idênticos Xtensa de 32 bits LX6, denominados **Core 0 (PRO_CPU)** e **Core 1 (APP_CPU)** [262, 263, 340, 631]:
*   **PRO_CPU (Protocol CPU / Core 0):** Tradicionalmente reservado para gerenciar as pilhas de protocolos de rede sem fio de segundo plano, como Wi-Fi, Bluetooth Classic e Bluetooth Low Energy (BLE) [184, 210, 220, 571, 574, 631].
*   **APP_CPU (Application CPU / Core 1):** Geralmente dedicado a rodar o código principal da aplicação do desenvolvedor [184, 210, 220, 571, 574, 631].

### O Fluxo Nativo do Arduino no ESP32
Na Arduino IDE, as funções tradicionais `setup()` e `loop()` não rodam no "bare-metal". O framework Arduino encapsula o ESP-IDF e o FreeRTOS [245, 554, 555]. Quando o ESP32 inicializa, o ecossistema cria uma tarefa oculta do FreeRTOS (geralmente chamada de `loopTask`) [281] e a fixa por padrão no **Core 1 (APP_CPU)** [262, 268, 272, 450]. 

---

## 2. Mecânica de Criação de Tarefas no FreeRTOS (Arduino IDE)

Para executar código de forma concorrente em ambos os núcleos, devemos instanciar tarefas explicitamente por meio das APIs do FreeRTOS [262, 449]. A principal função para obter controle multicore é:

```cpp
BaseType_t xTaskCreatePinnedToCore(
    TaskFunction_t pxTaskCode,   // Ponteiro para a função da tarefa
    const char * const pcName,   // Nome descritivo para debug (máx. 16 caracteres)
    const uint32_t usStackDepth, // Tamanho da pilha (Stack Size) em BYTES
    void * const pvParameters,   // Parâmetros passados para a tarefa (ou NULL)
    UBaseType_t uxPriority,      // Prioridade da tarefa (0 é a menor prioridade)
    TaskHandle_t * const pxCreatedTask, // Ponteiro para o manipulador da tarefa
    const BaseType_t xCoreID     // ID do Core (0, 1 ou tskNO_AFFINITY)
);
``` [265, 269, 466, 467]

### O Pitfall Crítico de Memória (Bytes vs. Words)
No FreeRTOS "Vanilla" clássico, o parâmetro `usStackDepth` define o tamanho da pilha em **Words** (palavras de 32 bits em sistemas ARM/Xtensa). 
*   **No ESP32 FreeRTOS (Xtensa), o tamanho é medido estritamente em BYTES** [223, 235, 280, 467, 468, 471, 473].
*   *Exemplo:* Se você definir `usStackDepth = 1000` esperando 1000 words (4000 bytes) como faria em outros sistemas, o ESP32 alocará apenas **1000 bytes**. Isso frequentemente resulta em travamentos imediatos por estouro de pilha (*Stack Overflow*) [223, 235].
*   Cada tarefa exige um mínimo absoluto de **768 bytes** apenas para manter o seu contexto básico de execução ativa, sem contar as variáveis locais [280]. Recomenda-se alocar uma margem de segurança (geralmente de `2048` a `4096` bytes para tarefas básicas de I/O).

---

## 3. O Escalonador SMP do ESP32 e Comportamento Multicore

O escalonador clássico do FreeRTOS é um sistema preemptivo de prioridade fixa com divisão de tempo (Time Slicing) [186, 199, 212, 458]. No entanto, no ESP32, o escalonador foi modificado para suportar **Multiprocessamento Simétrico (SMP)** [181, 221, 231].

### Best Effort Round-Robin e o Problema de Task Skipping
Diferente do FreeRTOS clássico que executa tarefas de mesma prioridade em uma fila circular (*Round-Robin*) perfeita, o escalonador SMP do ESP32 adota uma política de **Best Effort Round-Robin** [188, 201, 214, 224, 236, 460].

A lista de tarefas prontas para execução (`pxReadyTasksList`) é compartilhada entre os núcleos, mas possui apenas um ponteiro de índice global (`pxIndex`) para rastrear o progresso da rotação [237, 238]. Isso gera a seguinte anomalia concorrente [238]:

1. **O Core 0** chama o escalonador e seleciona a *Tarefa A*. Ele move o `pxIndex` global para apontar para a *Tarefa A* [239].
2. **O Core 1** chama o escalonador imediatamente a seguir e inicia a busca a partir da tarefa subsequente, que é a *Tarefa B*. No entanto, a *Tarefa B* está fixada exclusivamente ao Core 0. O Core 1 é obrigado a pulá-la e seleciona a *Tarefa C*, apontando o `pxIndex` para a *Tarefa C* [239].
3. Na próxima iteração, **o Core 0** retoma a busca a partir da tarefa após o `pxIndex` atual (ou seja, após a *Tarefa C*). Com isso, a *Tarefa B* foi completamente ignorada em um ciclo de Time Slicing sem ter a chance de executar [239].

> **Regra de Ouro Multicore:** Para garantir um escalonamento Round-Robin perfeitamente distribuído e evitar o "task skipping", **todas as tarefas que compartilham a mesma prioridade devem ser fixadas (pinned) no mesmo núcleo** [226].

---

## 4. Controle de Concorrência e Primitivas de Sincronização

A comunicação inter-tarefas (ITC) e a exclusão mútua são fundamentais em sistemas dual-core onde os núcleos compartilham o mesmo espaço de memória física [220, 316, 631].

### Semáforo Binário vs. Mutex: Diferenças de Propriedade e Sincronização
Embora conceitualmente parecidos, as suas aplicações práticas são totalmente distintas [106, 632]:

| Característica | Semáforo Binário | Mutex (Mutual Exclusion) |
| :--- | :--- | :--- |
| **Caso de Uso Primário** | Sinalização e Sincronização (ex: ISR para Tarefa) [106, 117, 124, 133, 148, 632] | Proteção mútua de recursos compartilhados (ex: Serial) [106, 118, 125, 142, 632] |
| **Conceito de Propriedade** | **Não possui dono.** Qualquer tarefa ou ISR pode liberar (give) [117, 137, 145, 146, 632]. | **Possui dono.** Somente a tarefa que o obteve (take) pode liberá-lo (give) [116, 121, 125, 142, 146, 632]. |
| **Estado Inicial** | Inicializado em Zero (indisponível por padrão) [632]. | Inicializado em Um (disponível por padrão) [116, 621, 632]. |
| **Priority Inheritance** | **Não suportado.** Pode levar à Inversão de Prioridade [123, 620, 632]. | **Suportado nativamente.** Previne a Inversão de Prioridade [123, 326, 613, 620, 632]. |
| **Execução em ISR** | Seguro para ser liberado a partir de uma ISR (`...FromISR`) [117, 124]. | **Proibido em ISR** (causa pânico no núcleo) [525]. |

#### O Mecanismo de Herança de Prioridade (Priority Inheritance)
A inversão de prioridade ocorre quando uma tarefa de alta prioridade fica bloqueada aguardando um recurso retido por uma tarefa de baixa prioridade, enquanto tarefas de prioridade média continuam rodando e impedindo a tarefa de baixa prioridade de terminar o seu trabalho.

O Mutex resolve isso herdando temporariamente a prioridade da tarefa bloqueada [326, 613, 620]:
*   Quando a tarefa de alta prioridade solicita o Mutex, o FreeRTOS eleva a prioridade da tarefa de baixa prioridade (que detém o Mutex) ao nível da tarefa de alta prioridade [326, 613, 620].
*   Isso impede que tarefas de prioridade média preemptem a tarefa de baixa prioridade, permitindo que ela conclua a seção crítica rapidamente e libere o Mutex [326].

### Mutexes Recursivos
Se o seu código executa chamadas de funções aninhadas que precisam adquirir repetidamente o mesmo Mutex dentro da mesma tarefa, utilize um **Mutex Recursivo** [522, 621, 632]. 
*   Criado via: `xSemaphoreCreateRecursiveMutex()` [522, 621].
*   Manipulado via: `xSemaphoreTakeRecursive()` e `xSemaphoreGiveRecursive()` [522, 524].
*   *Comportamento:* Ele rastreia internamente a quantidade de aquisições do lock feitas pela tarefa [622, 632]. O recurso só é liberado para outras tarefas quando a tarefa proprietária executar exatamente o mesmo número de liberações (`Give`) em relação às aquisições (`Take`) [522, 622, 632].

### Spinlocks e Seções Críticas com `portMUX_Type`
Em microcontroladores de núcleo único, as seções críticas são implementadas desativando as interrupções locais ou suspendendo o escalonador (`vTaskSuspendAll()`) [189, 232]. 
*   **No ESP32 (SMP), desativar interrupções locais ou suspender o escalonador em um núcleo NÃO impede que o outro núcleo acesse a mesma memória física simultaneamente [232]!**
*   Para exclusão mútua multicore real de baixíssimo nível, o ESP-IDF utiliza **Spinlocks** [463, 634].

```cpp
// Declaração estática de um spinlock
static portMUX_Type meuSpinlock = portMUX_INITIALIZER_UNLOCKED;

// Entrada na Seção Crítica (Desativa interrupções locais e gira o spinlock)
taskENTER_CRITICAL(&meuSpinlock);

/* --- SEÇÃO CRÍTICA DE BAIXO NÍVEL: ACESSO DIRETO A HARDWARE / REGISTRADORES --- */

// Saída da Seção Crítica (Libera o spinlock e reativa as interrupções)
taskEXIT_CRITICAL(&meuSpinlock);
``` [462, 463]

#### Regras Restritas para Seções Críticas (Spinlocks)
1. **Duração mínima absoluta:** Eles bloqueiam interrupções e suspendem a execução do outro núcleo se ele tentar acessar a mesma seção crítica [191, 204, 217, 463, 464].
2. **Proibição de bloqueios/yields:** Nunca use funções de atraso (`delay()`, `vTaskDelay()`), chamadas de sincronização do FreeRTOS ou operações lentas (como Serial ou I2C) dentro de uma seção crítica [191, 204, 217, 464]. Isso causará imediatamente uma falha do sistema ou acionamento do Watchdog [276, 573].

### Comparação de Performance: Atomics vs. Spinlocks vs. Mutexes
Cada mecanismo de sincronização possui custos de hardware e software distintos. Veja os resultados típicos de benchmarks de eficiência para operações repetitivas (tempo menor indica maior velocidade) [11]:

1. **Sem bloqueio (Acesso Inseguro):** `0.24s` (Rápido, mas corrompe os dados) [11].
2. **Operações Atômicas (Atomic):** `1.08s` (Executadas diretamente pelo hardware de barramento Xtensa) [11].
3. **Spinlocks (`portMUX_Type`):** `2.90s` (Rápido, mas consome ciclos de CPU ativamente se houver contenção) [11, 163].
4. **Mutex clássico:** `5.82s` (Mais lento, devido ao custo de colocar a tarefa bloqueada para dormir e gerenciar o escalonador) [11, 132, 659, 674].

> **Recomendação de Arquitetura:** Use **Atomics** para variáveis numéricas simples (contadores, flags) [2, 10]. Use **Mutexes** para proteger seções de código lentas ou de alto nível [164]. Use **Spinlocks** somente para acessos ultrarrápidos de hardware a nível de registrador ou interrupção [163, 634].

---

## 5. Armadilhas Comuns (Troubleshooting e Boas Práticas)

### Watchdog Timer (WDT) Resets no Core 0
O Watchdog do sistema está ativo em ambos os núcleos [380]. Como o Core 0 lida nativamente com o Wi-Fi e Bluetooth [184, 571], ele executa a tarefa IDLE com prioridade zero para realizar tarefas domésticas internas do sistema e limpar a memória de tarefas deletadas [185, 211, 457].
*   Se você criar uma tarefa de prioridade igual ou superior a **1** e fixá-la no **Core 0**, contendo um loop infinito (`while(1)`) sem nenhuma chamada bloqueante, **a tarefa IDLE nunca terá a chance de rodar no Core 0** [276, 573].
*   Isso resultará no erro clássico: `task_wdt: Task watchdog got triggered` [276], seguido por uma reinicialização forçada do ESP32 [276, 573].
*   *Solução:* Insira sempre `vTaskDelay(pdMS_TO_TICKS(ms))` ou a função `delay()` do Arduino (que encapsula internamente a chamada de `vTaskDelay` no ESP32 [282]) para ceder tempo ao escalonador e permitir o reset do watchdog [276].

### Observação sobre Recursos de Ponto Flutuante (FPU)
O microprocessador Xtensa LX6 do ESP32 integra hardware de Unidade de Ponto Flutuante (FPU) [374]. Embora as fontes apontem suporte a operações de ponto flutuante [219, 374], detalhes finos de otimização de baixo nível como o "Lazy Context Switching" da FPU não são explicitados nos manuais fornecidos. Contudo, evite realizar cálculos matemáticos densos com `float` ou `double` dentro de Seções Críticas baseadas em Spinlock para não atrasar as interrupções do sistema.

---

## 6. Exemplos Práticos Higienizados e Seguros

Abaixo estão dois exemplos completos escritos no padrão de linguagem do Arduino que aplicam rigorosamente todas as regras de segurança concorrente.

### Exemplo 1: Processamento Paralelo Seguro e Yield para o Watchdog (Core 0 e Core 1)
Este exemplo cria duas tarefas que operam de forma totalmente concorrente em núcleos distintos, garantindo que o núcleo PRO_CPU (Core 0) nunca sofra com o acionamento do Watchdog.

```cpp
#include <Arduino.h>

// Manipuladores de tarefas
TaskHandle_t tarefaCore0_Handle = NULL;
TaskHandle_t tarefaCore1_Handle = NULL;

// Protótipos de funções de tarefas
void tarefaCore0(void *pvParameters);
void tarefaCore1(void *pvParameters);

void setup() {
    Serial.begin(115200);
    delay(1000); // Aguarda estabilização da UART
    Serial.println("Inicializando Sistema Dual-Core...");

    // Criação de tarefa no Core 0 (PRO_CPU) - Cuidado redobrado com Watchdog
    xTaskCreatePinnedToCore(
        tarefaCore0,              // Função que implementa a tarefa
        "Tarefa_Core0",           // Nome para depuração
        4096,                     // Stack size em BYTES
        NULL,                     // Parâmetro de entrada
        1,                        // Prioridade
        &tarefaCore0_Handle,      // Manipulador da tarefa
        0                         // Fixado no Core 0
    );

    // Criação de tarefa no Core 1 (APP_CPU)
    xTaskCreatePinnedToCore(
        tarefaCore1,              // Função que implementa a tarefa
        "Tarefa_Core1",           // Nome para depuração
        4096,                     // Stack size em BYTES
        NULL,                     // Parâmetro de entrada
        1,                        // Prioridade (mesma prioridade)
        &tarefaCore1_Handle,      // Manipulador da tarefa
        1                         // Fixado no Core 1
    );
}

void loop() {
    // loop() roda por padrão no Core 1. 
    // Podemos deixá-lo vazio ou deletá-lo. Aqui aplicamos vTaskDelay para não consumir CPU.
    vTaskDelay(pdMS_TO_TICKS(10000));
}

// Tarefa dedicada ao Core 0
void tarefaCore0(void *pvParameters) {
    (void) pvParameters;
    
    Serial.print("Tarefa 0 iniciada no núcleo: ");
    Serial.println(xPortGetCoreID()); // Deve retornar 0

    for (;;) {
        // Simula processamento de rádio ou rede de segundo plano
        // ...

        // ESSENCIAL: vTaskDelay garante que a IDLE task execute no Core 0,
        // evitando que o Watchdog de hardware reinicie o chip.
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
}

// Tarefa dedicada ao Core 1
void tarefaCore1(void *pvParameters) {
    (void) pvParameters;

    Serial.print("Tarefa 1 iniciada no núcleo: ");
    Serial.println(xPortGetCoreID()); // Deve retornar 1

    for (;;) {
        // Simula coleta local de sensores analógicos de alta velocidade
        // ...

        vTaskDelay(pdMS_TO_TICKS(20)); // Cede tempo amigável de execução
    }
}
``` [265, 269, 270, 276, 451, 452]

### Exemplo 2: Proteção de Recurso de Hardware Compartilhado (Serial Monitor) Usando Mutex
Escrever simultaneamente no Monitor Serial a partir de núcleos diferentes de forma desordenada corrompe a saída de caracteres. Este exemplo protege a UART garantindo acesso sequencial exclusivo por meio de um Mutex do FreeRTOS com tratamento de limite de tempo (Timeout).

```cpp
#include <Arduino.h>

// Manipulador do Mutex de exclusão mútua do Serial
SemaphoreHandle_t serialMutex = NULL;

// Tempo limite máximo que uma tarefa aceita esperar pelo Mutex (em Ticks)
const TickType_t MUTEX_TIMEOUT = pdMS_TO_TICKS(500); 

void tarefaLeituraSensor(void *pvParameters);
void tarefaControleStatus(void *pvParameters);

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Criação do Mutex de forma segura
    serialMutex = xSemaphoreCreateMutex();

    if (serialMutex == NULL) {
        Serial.println("Erro crítico: Falha ao criar o Mutex da UART!");
        while (1); // Trava execução para evitar falhas graves
    }

    // Criação de duas tarefas com prioridades diferentes no Core 1 para demonstrar Priority Inheritance
    xTaskCreatePinnedToCore(
        tarefaLeituraSensor,
        "Sensor_Task",
        3072,
        NULL,
        1,  // Prioridade menor
        NULL,
        1
    );

    xTaskCreatePinnedToCore(
        tarefaControleStatus,
        "Status_Task",
        3072,
        NULL,
        2,  // Prioridade maior (pode causar herança de prioridade se Sensor detiver o Mutex)
        NULL,
        1
    );
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(10000));
}

void tarefaLeituraSensor(void *pvParameters) {
    (void) pvParameters;
    for (;;) {
        // Tenta adquirir o acesso exclusivo do Serial Monitor
        if (xSemaphoreTake(serialMutex, MUTEX_TIMEOUT) == pdTRUE) {
            // SEÇÃO CRÍTICA PROTEGIDA: Acesso exclusivo à UART
            Serial.println("=== INÍCIO LOG SENSOR ===");
            Serial.print("Sensor Umidade: ");
            Serial.println(random(40, 80));
            Serial.println("=== FIM LOG SENSOR ===");
            
            // Sempre liberar imediatamente o recurso após o término da seção crítica
            xSemaphoreGive(serialMutex);
        } else {
            // Tratamento de falha se o mutex não puder ser adquirido dentro do timeout
            // Impede bloqueios infinitos indesejados
        }

        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

void tarefaControleStatus(void *pvParameters) {
    (void) pvParameters;
    for (;;) {
        if (xSemaphoreTake(serialMutex, MUTEX_TIMEOUT) == pdTRUE) {
            // SEÇÃO CRÍTICA PROTEGIDA: Acesso exclusivo à UART
            Serial.println(">>> ALERTA DE STATUS: Sistema Operacional Estável <<<");
            
            xSemaphoreGive(serialMutex);
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
``` [297, 300, 301]