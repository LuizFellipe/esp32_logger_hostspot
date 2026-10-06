# Atlas técnico ESP32 GPS Logger v3

15 diagramas do hardware e software de `esp32gpsd_v3.ino`. Snapshot: **06/10/2026**, firmware `serial-2026-10-06-painel2`. Imagens inseridas nas seções correspondentes do [README do sistema](../../README.md).

[Atlas editável DrawIO](esp32gpsd-v3.drawio) contém todas as páginas. Cada PNG, exportado pelo DrawIO Desktop, também possui seu diagrama individual embutido e pode ser aberto no DrawIO para edição. Resolução: aproximadamente 2400 × 1650 px; o enquadramento do DrawIO pode variar alguns pixels entre páginas.

| Página | Conteúdo | Imagem |
|---|---|---|
| 01 | Arquitetura integrada | [Visão geral](01-visao-geral.png) |
| 02 | Pinos, interfaces e alimentação | [Hardware](02-hardware.png) |
| 03 | Inicialização e falhas de boot | [Inicialização](03-inicializacao.png) |
| 04 | Aquisição, modos, retry e foco HTTP | [Loop](04-loop.png) |
| 05 | Histerese e estados do logger | [Máquina de estados](05-maquina-estados.png) |
| 06 | GPS RMC/GGA, DHT e IMU | [Aquisição](06-aquisicao.png) |
| 07 | Scans WiFi/BLE e fechamento | [Ciclo de rádio](07-ciclo-radio.png) |
| 08 | Fila BLE e caches de hash | [Deduplicação](08-deduplicacao.png) |
| 09 | Buffers, confirmação SD e recuperação | [Armazenamento](09-armazenamento.png) |
| 10 | Tarefas, ownership e sdMutex | [Concorrência](10-concorrencia.png) |
| 11 | AP, página, inatividade e pausa da aquisição | [Hotspot](11-hotspot.png) |
| 12 | Allowlist, streaming e abortos | [Download](12-download.png) |
| 13 | Colunas, unidades e memória | [CSV](13-dados-csv.png) |
| 14 | Bibliotecas, sincronização e no_ota | [Build](14-build.png) |
| 15 | Console, recuperação e validação | [Diagnóstico](15-diagnostico.png) |

## Leitura visual

Cores codificam responsabilidades, não ordem de execução:

- Azul `#2864A0`: sensores e aquisição.
- Violeta `#7157A0`: rádio e BLE.
- Verde `#287452`: armazenamento SD.
- Petróleo `#167580`: HTTP e hotspot.
- Âmbar `#A36A15`: condição, falha ou limite.
- Azul aço `#354D6B`: aplicação e controle.

Setas indicam fluxo/transição; tracejado indica relação auxiliar ou caminho condicional. Só o diagrama de inicialização segue ordem única. Ramo WiFi e ramo BLE têm tempos próprios; junção requer ambos finalizados. Títulos: Ubuntu Sans; corpo: Noto Sans; legendas técnicas: DejaVu Sans Mono. Sem recursos visuais externos.

## Fontes e limites

Comportamento descrito a partir do [firmware v3](../../esp32gpsd_v3.ino), [README](../../README.md) e [serial.md](../../serial.md). Cada imagem traz referências do sketch no rodapé. Pinagem padrão conferida em [Dados e hardware](../../../docs/wiki/dados-hardware.md). Dependências vêm do [manifesto](../../../libraries/manifest.json); fluxo de sincronização segue [CONTEXT.md](../../../CONTEXT.md) e documentação de build.

Documentação geral contém versões anteriores; o atlas não incorpora consumidor BLE separado da v1, divisão de tarefas da variante dual-core, GSM, API remota, OLED, Bluetooth clássico ou deep sleep. Não há esquema elétrico/BOM completo disponível: diagrama de hardware representa interligações e marca itens não documentados, sem inventar alimentação ou valores de componentes.

Limites do firmware representados explicitamente:

- Foco HTTP descarta UART GPS e pausa aquisição/serviço do modo. Movimento não é detectado enquanto o foco estiver ativo.
- BLE é desinicializado antes do AP; próximo scan o reinicializa.
- Hash é inserido antes da confirmação no SD. Colisões e descarte de linhas podem afetar deduplicação.
- Confirmação SD é por lote/arquivo; retry pode duplicar linhas previamente escritas.
- Dados em RAM são voláteis. Retenção RTC depende do tipo de reset; não é armazenamento permanente.
- Prazos/cadências não garantem tempo real. Verificação lógica do firmware não substitui testes em placa.

## Edição e reprodução

Abrir `esp32gpsd-v3.drawio` no DrawIO Desktop. Para editar individualmente, também é possível abrir qualquer PNG com fonte embutida. Salvar mudanças no atlas e exportar página correspondente como PNG, escala **1,5**, opção **Include a copy of my diagram**, mantendo nome usado pelo README.

O gerador [gerar_diagramas.py](gerar_diagramas.py) mantém layout e conteúdo versionáveis. Executá-lo sobrescreve o atlas; alterações manuais no DrawIO precisam ser preservadas ou transferidas ao gerador antes de regenerar.

A partir de `esp32gpsd_v3/`:

```sh
python3 docs/diagramas/gerar_diagramas.py
while IFS="$(printf '\t')" read -r pagina nome; do
  xvfb-run -a drawio --no-sandbox --export --format png \
    --scale 1.5 --embed-diagram --page-index "$pagina" \
    --output "docs/diagramas/$nome.png" \
    docs/diagramas/esp32gpsd-v3.drawio
done < docs/diagramas/paginas.tsv
```

`xvfb-run` serve à exportação sem display no Linux; numa sessão gráfica pode ser omitido. `--no-sandbox` corresponde ao ambiente de exportação isolado utilizado aqui. O gerador é documentação explícita, não extração automática: após mudar firmware, revisar textos e conexões antes de regenerar.
