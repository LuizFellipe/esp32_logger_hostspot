# Diagnóstico de Travamento de Fase (BLE Loop Bug)

Este documento centraliza todas as tentativas realizadas para corrigir o bug onde o `esp_restart()` não estava preservando a variável `currentPhase` na memória RTC do ESP32, fazendo com que o sequenciador de fases ficasse preso num loop infinito executando apenas a fase 0 (BLE).

## Problema Raiz Identificado
No momento em que `esp_restart()` era chamado para alternar do Scan BLE para o Bluetooth Clássico, a variável global de controle de fase, que deveria sobreviver ao reinício da placa via `RTC_DATA_ATTR`, estava sendo zerada. Consequentemente, o `setup()` lia o valor `0` (PHASE_BLE) e reiniciava o ciclo eternamente.

## Cronologia das Tentativas

### 1. Fix da Race Condition no NimBLE (Sucesso Parcial)
- **Hipótese:** A task de orquestração estava chamando `esp_restart()` enquanto o callback `onScanEnd` do NimBLE tentava religar o scanner, causando uma corrupção.
- **O que foi feito:** Adicionado um flag `blePhaseEnding` para impedir que o callback agisse durante o desligamento, e a stack do orquestrador foi aumentada.
- **Resultado:** O loop persistiu. Concluiu-se que o hardware ainda não retinha o valor RTC.

### 2. Uso de NVS (Preferences) (Sucesso, porém inadequado)
- **Hipótese:** Se a memória RTC está volátil ou sendo apagada pelo bootloader/startup do compilador C, a gravação física na memória flash (NVS) resolveria definitivamente a persistência. Além disso, adicionou-se a função `NimBLEDevice::deinit(true)` para derrubar a pilha de rede.
- **O que foi feito:** Substituição total de `RTC_DATA_ATTR` por `Preferences.h`.
- **Resultado:** A troca de fases funcionou, porém a abordagem foi descartada a pedido do usuário. Gravar a cada 10-30 segundos na memória flash esgota rapidamente a vida útil do chip (Flash wear). Revertido.

### 3. Tentativa com RTC_NOINIT_ATTR e Magic Number (Falha Crítica)
- **Hipótese:** O script de inicialização do C (C-startup) do core do Arduino ESP32 estava sobrescrevendo as variáveis RTC que possuíam inicialização no código fonte (ex: `= PHASE_BLE`).
- **O que foi feito:** Alterado para `RTC_NOINIT_ATTR` para instruir o linker a não inicializar, acompanhado de um número mágico `rtcMagic` (0xCAFEBABF) para detecção de Cold Boot.
- **Resultado:** A placa ficou completamente muda na serial a 115200bps. Esse atributo, no core Arduino, tentou alocar memória numa região que conflitou com o script do linker e corrompeu a carga do bootloader, causando Kernel Panic contínuo e silencioso. Revertido.

### 4. Uso da seção `.rtc.bss` (RTC_DATA_ATTR sem valor) (Falha de Isolamento)
- **Hipótese:** Removendo o `= PHASE_BLE` na declaração da variável `RTC_DATA_ATTR`, ela cairia na seção `.rtc.bss` que o ESP-IDF e o Arduino prometem limpar apenas durante o Power-On (Hard Reset), mantendo no Soft Reset.
- **O que foi feito:** Variáveis instanciadas sem valor inicial para cair no `.rtc.bss`.
- **Resultado:** O framework específico do ESP32/Arduino em uso continuou forçando a inicialização desse segmento ao passar por um `esp_restart()`.

### 5. Substituição do Reset por Deep Sleep (Solução Final adotada)
- **Hipótese:** A única forma garantida por *hardware* de manter a memória RTC intacta em qualquer framework do ESP32 é utilizando o estado de energia Deep Sleep, pois o wake stub do bootloader o reconhece explicitamente e isola as áreas RTC.
- **O que foi feito:** A função `esp_restart()` foi completamente substituída por um helper chamado `triggerRestart()`, que executa um Deep Sleep de exatos 10 milissegundos.
- **Resultado:** Ao acordar do Deep Sleep curtíssimo, a placa passa pelo mesmo ciclo de limpeza de RAM principal e recarregamento (exigência inicial), mas a variável RTC é intocada e persistida adequadamente, eliminando a dependência do NVS e preservando a vida útil da flash.
