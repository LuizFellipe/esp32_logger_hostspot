---
name: sync-docs
description: Reconcilia documentação do projeto ESP32 com mudanças de firmware, bibliotecas e organização desde o último commit. Atualiza READMEs existentes, sinaliza novos conceitos e prepara proposta de commit e push mediante aprovação.
---

# /sync-docs

Sincronize documentação com o código atual deste repositório, preservando
conteúdo correto, mudanças locais e decisões já autorizadas na sessão.

## Fontes deste projeto

| Área | Código ou configuração | Documentação |
|---|---|---|
| Firmware principal | `esp32gpsd/esp32gpsd.ino` | `esp32gpsd/README.md` |
| Firmware experimental | `esp32gpsd_v2/esp32gpsd_v2.ino` | README e notas de análise dessa pasta |
| Bibliotecas externas | `libraries/manifest.json`, `libraries/sync.py` e pacotes locais | `libraries/README.md` |
| Visão integrada | Firmwares e estrutura efetiva | `README.md`, `docs/README.md`, `docs/wiki/`, `docs/GRAPHIFY.md` |
| Conceitos e regras | Fluxos e termos implementados | `CONTEXT.md` |
| Histórico | `archive/` e submódulo GSM | `archive/README.md` e docs de cada variante |
| Ferramentas | `.agents/`, `.claude/`, `.serena/`, `.tokensave/` | Configurações e skills correspondentes |
| Grafo gerado | `graphify-out/` | `graphify-out/GRAPH_REPORT.md` |

Referências ao histórico ESP32 continuam válidas. Preserve documentação de
experimentos antigos e READMEs de bibliotecas vendorizadas; compare-os com sua
própria versão, sem substituir suas descrições pelo firmware principal atual.
O projeto de hotspot documentado na v2 não comprova implementação: confira o
sketch antes de apresentá-lo como funcionalidade disponível.

## 1. Detectar mudanças

Execute e analise resultados completos:

```sh
git status --short
git diff HEAD --stat
git log --oneline -5
```

Inclua staged, unstaged, novos, removidos e renomeados. Para pastas untracked,
enumere arquivos com `git ls-files --others --exclude-standard`. Separe firmware,
ferramentas/configuração, documentação, bibliotecas, histórico e Graphify.
Movimentos de arquivos sem alteração de conteúdo não são novas funcionalidades.

Se apenas Graphify mudou, informe que não há documentação funcional a sincronizar.

## 2. Comparar documentação e implementação

Antes de abrir código, consulte `graphify-out/GRAPH_REPORT.md` e use `graphify query`,
`graphify path` e `graphify explain` para localizar fontes. O extrator atual tem
cobertura parcial de `.ino`; use os sketches indicados pelos READMEs indexados.

Em cada pasta afetada que já tenha README, leia o README e o diff específico
(`git diff HEAD -- <arquivo>`). Para arquivo novo não rastreado, leia seu conteúdo.
Não crie README onde não existe. Edite somente discrepâncias comprovadas.

Confirme versão por versão: GPIO/UART, sensores, cadência de scans/logs, modos e
limiares, proprietário dos buffers, consumo da fila BLE, flush SD e watchdog.
Não atribua uma melhoria exclusiva da v2 à v1. Mantenha planos, diagnóstico
histórico e comportamento implementado claramente identificados.

Quando mudanças afetarem visão compartilhada, identifique trechos específicos
de `README.md` ou `docs/` que precisam mudar. Aproveite autorização existente na
sessão; se atualização global ainda não foi autorizada, apresente trechos e peça
confirmação antes de alterá-los. Não procure documentos de outros projetos.

Antes de uma compilação necessária, siga a regra de bibliotecas em `CONTEXT.md`:
sincronize projeto → Arduino IDE e confira igualdade usando `libraries/sync.py`.
Não recompile apenas por ajustes de texto; não faça upload de firmware durante
sincronização de docs. Preserve restrições de hardware dadas pelo usuário.

## 3. Sinalizar conceitos novos

Compare modos, fluxos de rádio, estruturas de registro e regras de armazenamento
novos com `CONTEXT.md`. Informe conceitos ausentes e seus arquivos de origem.
Nunca escreva no CONTEXT.md automaticamente. Atualize-o apenas quando o usuário
autorizar os conceitos concretos; não trate termos de um plano como implementados.

## 4. Validar e preparar revisão

Confira links relativos, nomes e caminhos após movimentos. Verifique que firmware
e bibliotecas não foram alterados por uma tarefa de documentação. Aponte diferenças
preexistentes sem misturá-las às edições feitas pela skill.

Liste todos os arquivos candidatos ao commit por categoria. Em conjuntos grandes,
forneça inventário completo em arquivo acessível e resumo com contagens. Liste
Graphify separadamente; respeite a decisão da sessão sobre sua inclusão. Neste
projeto o grafo é versionado: não o exclua nem peça a mesma confirmação novamente
quando sua inclusão já estiver autorizada. Logs locais, DBs e caches ignorados
permanecem fora do commit.

Proponha mensagem em português, imperativo, descrevendo mudanças reais de firmware,
organização e documentação, conforme estilo de `git log --oneline -5`.
Peça aprovação da mensagem e do conjunto de arquivos antes de commitar.

Depois da aprovação, adicione somente arquivos aprovados, mostre `git status` e
peça confirmação final para commit e push. Execute-os somente após essa confirmação.
Se houver recusa, mantenha alterações no working tree para revisão manual.

Ao pedir aprovação exigida pela skill, cite este arquivo e a instrução aplicável.
Não envie mensagens externas nem publique outros artefatos como parte deste fluxo.
