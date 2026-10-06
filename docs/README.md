# Documentação

- [Logger com hotspot (v3, em teste)](../esp32gpsd_v3/README.md)
- [Atlas técnico v3: 15 diagramas, PNGs e fonte editável](../esp32gpsd_v3/docs/diagramas/README.md)
- [Arquivo DrawIO completo](../esp32gpsd_v3/docs/diagramas/esp32gpsd-v3.drawio)
- [Wiki e inventário](wiki/README.md)
- [Mapa dos projetos](GRAPHIFY.md)
- [Pinout original](hardware/pinout.png)
- [Vocabulário e regras do projeto](../CONTEXT.md)
- [Bibliotecas, sincronização e builds](../libraries/README.md)
- [Histórico](../archive/README.md)

O README raiz apresenta o projeto; os READMEs dos sketches descrevem seu
comportamento. O grafo gerado permanece em `graphify-out/`, na raiz, e é versionado.

## Diagramas no contexto do projeto

As imagens são mantidas em `esp32gpsd_v3/docs/diagramas/` e referenciadas pelos documentos, evitando cópias divergentes. Snapshot: 06/10/2026. Diagramas representam exclusivamente a v3; outras variantes mantêm seus fluxos próprios.

| Tema | Documentação ilustrada |
|---|---|
| Visão integrada | [README do projeto](../README.md) e [visão geral](wiki/visao-geral.md) |
| Boot, loop, estados, rádio, buffers, tarefas, hotspot e download | [Arquitetura do logger](wiki/arquitetura-logger.md) |
| Hardware, aquisição, CSV e memória | [Dados e hardware](wiki/dados-hardware.md) |
| Bibliotecas, no_ota e diagnóstico | [Compilação e gravação](wiki/compilacao-arduino-cli.md) |
| Todos os 15 diagramas | [README v3](../esp32gpsd_v3/README.md) e [atlas](../esp32gpsd_v3/docs/diagramas/README.md) |

![Arquitetura integrada do logger v3](../esp32gpsd_v3/docs/diagramas/01-visao-geral.png)
