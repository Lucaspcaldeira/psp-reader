# Etapa 1 — Planejamento técnico

Leitor de livros digitais para PSP (homebrew, PSPSDK/C). Documento de decisões
da Etapa 1. Cada decisão registra o **porquê**, para não ser reaberta sem motivo.

Alvo de hardware: **PSP-2000/3000** (64 MB de RAM), CFW, teste em hardware real.

---

> **Nota da Etapa 4.** Este documento registra as decisões tomadas na Etapa 1,
> quando o alvo era só PDF. O projeto lê hoje **EPUB, PDF e TXT**. O que mudou em
> relação ao previsto aqui está na seção 13, no fim — as seções 1 a 12 ficam como
> estavam, porque o valor delas é registrar o raciocínio no momento em que ele
> foi feito.

---

## 1. Formato inicial: PDF, com reflow

Decidido: **PDF de texto**, lido por parser próprio, exibido com **reflow**.

### Por que reflow e não página fixa

PDF é um formato de **layout fixo**: a página tem tamanho físico definido
(A4 = 595x842 pt) e o texto tem coordenadas absolutas. A tela do PSP tem
480x272. Exibir a página inteira reduzida deixa o corpo do texto em ~4 px —
ilegível. A saída óbvia é zoom + pan em duas dimensões, que foi o que o Bookr
fez em 2007, e não é leitura de livro: é navegar num mapa.

**Reflow** extrai o texto com suas posições, reconstrói os parágrafos lógicos e
repagina para 480x272 com word-wrap e corpo de fonte ajustável. É o que um
Kindle faz. É o objetivo declarado do projeto.

O custo é que reflow exige entender o conteúdo, não só desenhá-lo — daí o
parser de content stream na seção 4.

---

## 2. Licença MIT: o que ela proíbe

Requisito do projeto: **MIT**. Isso não é um detalhe da Etapa 6, é uma
restrição de arquitetura, porque elimina as três bibliotecas que seriam a
escolha natural:

| Biblioteca | Licença | Veredito |
|---|---|---|
| **MuPDF** (Artifex) | AGPL-3.0 ou comercial paga | **Fora.** "If you link MuPDF into your own software, the entirety of that software must be licensed under the GNU AGPL." |
| **Bookr** / **PSPPDF** / **MuPDF-PSP** | GPLv2 | **Fora.** Copyleft; nem o código nem trechos dele. |
| **intraFont** (BenHur) | CC BY-SA 3.0 | **Fora.** Share-alike viral. (Usado no projeto `midnight`; aqui não dá.) |

O que sobra é escrever o parser de PDF do zero. Foi a decisão tomada com pleno
conhecimento do custo — ver seção 9 (riscos).

### Dependências permitidas

| Dependência | Licença | Uso | Disponível? |
|---|---|---|---|
| PSPSDK | BSD-like | `sceGu`, `sceCtrl`, `sceIo` | sim |
| **zlib** 1.3.1 | zlib (permissiva) | `FlateDecode` — o filtro de stream de 95% dos PDFs | `psp-pacman` |
| **FreeType 2** 2.11.0 | FTL (BSD-like, exige atribuição) | rasterização de glifos | `psp-pacman` |
| Fonte TTF sob **OFL 1.1** | OFL 1.1 | tipografia de leitura (ex. Noto Serif, EB Garamond) | embutir em `data/` |

FTL e OFL são permissivas e não contaminam nosso código: ele permanece MIT, e
elas ficam declaradas no `NOTICE`. Mesma relação que qualquer projeto MIT tem
com zlib.

Alternativa de reserva ao FreeType: **stb_truetype** (domínio público, pacote
`stb` no pspdev, header único). Qualidade de rasterização um pouco inferior,
zero atrito de licença. Trocável — o resto do código não muda.

### O ganho colateral de perder o intraFont

O intraFont não tem modo UTF-8: sua flag `INTRAFONT_STRING_ASCII` é na verdade
Latin-1, 1 byte por glifo. O `midnight` contorna isso com uma conversão
UTF-8 para Latin-1 **com perda**, que mapeia travessão para `-`, aspas curvas
para `"` e reticências para três pontos.

Num jogo com diálogos curtos isso passa. Num **leitor de livros** é inaceitável:
é exatamente a pontuação tipográfica que aparece em texto editorado, em todas as
páginas.

FreeType rasteriza qualquer codepoint Unicode. Então o pipeline passa a ser
UTF-8 puro de ponta a ponta, sem conversão com perda, e ainda ganha controle
real de corpo de fonte (essencial: mudar o corpo é uma função do leitor).

**A restrição MIT empurrou o projeto para a arquitetura melhor.**

---

## 3. A decisão que torna o projeto viável

**O parser de PDF e o motor de reflow não terão nenhuma dependência de PSP.**

São C99 puro sobre uma interface mínima de I/O (ler N bytes no offset X).
Compilam e rodam no host, com `gcc` nativo dentro do container, exercitados por
um harness de teste contra PDFs reais.

Isso importa porque a maior parte do código deste projeto é o parser e o reflow.
Depurar um tokenizer de content stream via ciclo "compila, copia pro Memory
Stick, desconecta USB, abre no XMB, olha a tela" seria insustentável. No host o
ciclo é de segundos, com `printf`, assert e diff de saída esperada.

Só o que é genuinamente do console — `gfx`, `input`, atlas de glifos, sistema de
arquivos — é código PSP. Segue o padrão do `test.ps1` do `midnight`.

---

## 4. Arquitetura

```
                   +----------------------------------+
                   |  UI (PSP)  library / reader /    |
                   |            settings / progress   |
                   +----------------------------------+
                                    |
                   +----------------------------------+
                   |  layout.c   paragrafos -> paginas|   <-- portavel
                   |  reflow.c   runs -> paragrafos   |   <-- portavel
                   +----------------------------------+
                                    |
                   +----------------------------------+
                   |  pdf_text  content stream -> runs|   <-- portavel
                   |  pdf_page  arvore de paginas     |
                   |  pdf_font  widths / ToUnicode    |
                   |  pdf_filt  Flate / A85 / LZW / RL|
                   |  pdf_xref  xref + xref streams   |
                   |  pdf_obj   modelo de objetos     |
                   |  pdf_lex   tokenizer             |
                   +----------------------------------+
                                    |
                   +----------------------------------+
                   |  io.h   ler N bytes no offset X  |
                   +----------------------------------+
                     host: fread     PSP: sceIoRead
```

### Camada PDF (portável)

| Módulo | Responsabilidade |
|---|---|
| `pdf_lex.c` | Tokenizer: números, nomes (`/Name`), strings literais e hexadecimais, delimitadores, comentários |
| `pdf_obj.c` | Modelo de objeto (null/bool/num/string/name/array/dict/stream/ref) e resolução de referência indireta `N G R` |
| `pdf_xref.c` | Tabela xref clássica, **xref streams** (PDF 1.5+), `/Prev` encadeado, trailer, **e reconstrução por varredura** quando o xref está corrompido |
| `pdf_filt.c` | `FlateDecode` (zlib), `ASCIIHexDecode`, `ASCII85Decode`, `LZWDecode`, `RunLengthDecode`, e `/DecodeParms` com predictor PNG |
| `pdf_font.c` | `/Font`: `/Widths`, `/Encoding` (WinAnsi, MacRoman, `/Differences`), `/ToUnicode` CMap, fontes CID e `Identity-H` |
| `pdf_page.c` | Árvore `/Pages` e `/Kids`, herança de `/MediaBox` e `/Resources`, `/Contents` (inclusive array de streams) |
| `pdf_text.c` | Máquina de estado do content stream. Operadores de texto: `BT`/`ET`, `Tf`, `Td`/`TD`/`Tm`/`T*`, `TL`, `Tc`/`Tw`/`Tz`/`Ts`/`Tr`, `Tj`/`TJ` e os dois operadores de aspas, mais `cm`/`q`/`Q` para a CTM. Saída: **runs de texto** com posição, corpo e fonte |

### Camada de leitura (portável)

| Módulo | Responsabilidade |
|---|---|
| `reflow.c` | Runs posicionados para parágrafos lógicos. Agrupa runs em linhas por proximidade de Y; ordena por X; detecta quebra de parágrafo por indentação e espaçamento vertical anômalo; junta palavras hifenizadas no fim de linha; descarta cabeçalho/rodapé recorrente; detecta múltiplas colunas por histograma de X |
| `layout.c` | Parágrafos + largura útil + corpo da fonte para páginas de leitura. Word-wrap por largura real de glifo, não por contagem de caracteres |
| `pageidx.c` | Índice de páginas persistido em sidecar `.pri`. Chave de invalidação: tamanho do arquivo + mtime + corpo da fonte + versão do layout |

### Camada PSP

| Módulo | Origem |
|---|---|
| `fs.c`, `input.c`, `gfx.c`, `texture.c`, `menu.c` | **portados do `midnight`** |
| `font.c` | **novo** — FreeType + atlas de glifos em textura |
| `library.c` | browser de `ms0:/PSP/BOOKS/` |
| `reader.c` | tela de leitura |
| `progress.c` | última página lida, por arquivo |
| `settings.c` | corpo da fonte, tema, margens |

---

## 5. Estrutura de pastas

```
C:\Users\lscal\psp-homebrew\ereader\        <-- FONTE (no HD, nao no Memory Stick)
├── PLAN.md                  # este documento
├── README.md                # Etapa 6
├── LICENSE                  # MIT
├── NOTICE                   # atribuicao de zlib / FreeType / fonte OFL
├── Makefile                 # build PSPSDK (roda no container)
├── build.ps1                # docker pspdev -> make
├── run.ps1                  # build + PPSSPP
├── deploy.ps1               # build + copia pro PSP (detecta drive, verifica hash)
├── test.ps1                 # testa parser+reflow no HOST, sem PSP
├── include/                 # headers publicos
├── src/
│   ├── main.c
│   ├── pdf/                 # camada PDF, portavel
│   ├── read/                # reflow, layout, pageidx — portavel
│   └── psp/                 # gfx, input, font, ui — especifico do console
├── data/                    # fonte TTF (vai pro PSP)
├── tools/
│   ├── Dockerfile.hosttest  # gcc nativo p/ testes no host
│   ├── test_pdf.c           # dump de texto extraido de um PDF
│   └── test_reflow.c        # dump de paginas paginadas
└── dist/EREADER/            # gerado; conteudo copiado pro Memory Stick
```

No PSP:

```
ms0:/PSP/GAME/EREADER/EBOOT.PBP     # o homebrew
ms0:/PSP/GAME/EREADER/data/         # fonte TTF
ms0:/PSP/BOOKS/                     # os livros do usuario
ms0:/PSP/BOOKS/.ereader/            # indices .pri e progresso de leitura
```

Livros ficam **fora** da pasta do app, em `ms0:/PSP/BOOKS/`, para sobreviverem a
uma reinstalação do homebrew.

---

## 6. Controles

| Botão | Ação na leitura |
|---|---|
| esquerda / direita, L / R | página anterior / próxima |
| cima / baixo | corpo da fonte (dispara reindexação) |
| X | confirmar (na biblioteca: abrir livro) |
| O | voltar |
| Triângulo | menu: ir para página, marcadores, tema |
| Select | alterna reflow e página fixa |
| Start | fecha o livro, volta para a biblioteca |

---

## 7. Orçamento de memória (PSP-2000, 64 MB)

O maior PDF da biblioteca de teste tem **72 MB** — maior que a RAM do console.
Carregar arquivo inteiro está fora de questão em qualquer cenário.

| Item | Alvo |
|---|---|
| Arquivo PDF | **nunca residente.** `sceIoOpen` + seek/read sob demanda |
| Cache LRU de objetos PDF descomprimidos | ~2 MB, teto rígido |
| Índice de páginas | em disco (`.pri`); só a janela em torno da página atual em RAM |
| Página de leitura atual + próxima | poucos KB (texto reflowed) |
| Atlas de glifos (FreeType) | textura 256x256 CLUT8 por corpo de fonte, na VRAM |
| Framebuffers (2x 512x272x4) | ~1,1 MB de VRAM |
| **Total alvo** | **< 16 MB**, para caber com folga também num PSP-1000 |

---

## 8. Limitações conhecidas, declaradas desde já

Consequências diretas de "parser próprio + reflow". Nenhuma é bug; todas vão
para o README.

1. **PDF escaneado** (imagem pura, sem camada de texto) — não há texto para
   extrair. Detectar e informar: *"este PDF não tem camada de texto"*. Sem OCR.
2. **PDF criptografado** — suportar apenas senha vazia (RC4/AES padrão); o resto
   é recusado com mensagem clara.
3. **Tabelas e layout multi-coluna** — reflow embaralha. Mitigação: detecção de
   colunas por histograma de X. Não vai ser perfeito.
4. **Fontes CID sem `/ToUnicode`** — glifos sem mapeamento para Unicode ficam
   irrecuperáveis. Viram `?`.
5. **Imagens e figuras** — ignoradas no modo reflow.

---

## 9. Riscos, em ordem de probabilidade

1. **Robustez do parser em PDFs do mundo real.** O maior risco do projeto, com
   folga. PDFs gerados por ferramentas ruins têm xref com offset errado, objetos
   duplicados, `endstream` faltando, `/Length` mentindo. Mitigação: reconstrução
   de xref por varredura de `N G obj`, e o `/Length` tratado como dica, não como
   verdade.
2. **Qualidade do reflow.** Distinguir "nova linha do mesmo parágrafo" de "novo
   parágrafo" a partir só de coordenadas é heurística, não algoritmo.
3. **Tempo da primeira indexação.** Paginar um PDF de 500 páginas no MIPS de
   333 MHz pode levar minutos. Mitigação: indexar em background com barra de
   progresso, gravando o `.pri` incrementalmente, e permitir começar a ler antes
   de terminar.

---

## 10. Corpus de teste

Da biblioteca Calibre local, três casos com propósitos distintos:

| Arquivo | Tamanho | Papel |
|---|---|---|
| `A Ilha do Tesouro Recortado.pdf` | 227 KB | caso pequeno, iteração rápida |
| `Comunicacao Nao-Violenta.pdf` | 1,1 MB | **caso principal** — livro de texto corrido |
| `Fogo & Sangue - Volume 1.pdf` | 72 MB | teste de estresse: prova que o streaming funciona |

---

## 11. Ambiente

Idêntico ao `midnight`, para não manter dois fluxos:

| Item | Escolha |
|---|---|
| Toolchain | `pspdev/pspdev:latest` via Docker — nada instalado no Windows |
| Emulador | PPSSPP, `C:\Program Files\PPSSPP\PPSSPPWindows64.exe` |
| Hardware | PSP-2000/3000 com CFW, deploy em `ms0:/PSP/GAME/EREADER/` |
| Render | `sceGu` nativo |
| Texto | FreeType 2 + TTF sob OFL, UTF-8 puro |
| Testes de parser | `gcc` nativo no container, sem PSP |

**Pendência de ambiente:** o daemon do Docker Desktop está parado. Precisa
subir antes do primeiro build.

---

## 12. Ordem de execução

| Etapa | Entrega | Testável em | Estado |
|---|---|---|---|
| 2 | Esqueleto: Makefile, `build.ps1`, `deploy.ps1`, `test.ps1`, `main.c` que sobe vídeo e escreve na tela com FreeType | hardware | feito |
| 3a | `pdf_lex` + `pdf_obj` + `pdf_xref` + `pdf_filt`: abrir PDF, listar páginas | host | feito |
| 3b | `pdf_font` + `pdf_text`: extrair texto de uma página | host | feito |
| 3c | `reflow` + `layout`: parágrafos e paginação | host | feito |
| 3d | Biblioteca de arquivos + tela de leitura + navegação | hardware | feito |
| 4 | `doc` + TXT + EPUB, progresso de leitura, tema, corpo de fonte | host + hardware | feito |
| 5 | Ciclo de correção sobre seu feedback de hardware | hardware | **aqui** |
| 6 | README, LICENSE, NOTICE, publicação — só com seu aval | — | — |

---

## 13. Decisões da Etapa 4

Três coisas mudaram em relação ao que esta seção previa na Etapa 1. Registradas
aqui para não serem confundidas com esquecimento.

### 13.1 A camada `doc` não estava no plano, e devia estar

O plano da seção 4 desenhava a pilha inteira em torno de PDF. Ao acrescentar o
segundo formato ficou claro que faltava um degrau: uma **unidade de carga** e uma
saída em **parágrafos lógicos**, iguais para todo formato. Sem ele, cada formato
novo se espalharia pelo `main.c` em condicionais e o terceiro custaria mais que o
segundo. Com ele, custou menos.

A unidade é página no PDF, bloco de ~3 KB no TXT, trecho de capítulo no EPUB. O
que ela precisa garantir é **estabilidade**: o mesmo índice devolve sempre o
mesmo texto, porque o progresso de leitura guarda um índice.

### 13.2 O `pageidx` (`.pri`) foi abandonado, não esquecido

A seção 4 previa um índice de paginação persistido em disco. Ele existia para
evitar repaginar o livro a cada abertura — um medo calibrado para "paginar 500
páginas leva minutos" (risco 3).

Esse risco não se concretizou, porque a paginação passou a ser **por unidade** e
não por livro: abrir custa paginar uma página, o que são milissegundos. Um índice
em disco só acrescentaria uma chave de invalidação (tamanho, mtime, corpo,
versão) para errar, e um arquivo a mais para corromper. O progresso de leitura,
que é o que o usuário realmente queria, é guardado à parte e cabe em algumas
linhas de texto.

### 13.3 O modo de página fixa foi descartado por coerência

A seção 6 reservava SELECT para alternar entre reflow e página fixa. Página fixa
útil exige zoom e pan em duas dimensões — que é exatamente o que a **seção 1
argumenta não ser leitura de livro**, e o motivo de o projeto existir. Mantê-lo
seria construir o Bookr dentro do leitor que foi feito para não ser o Bookr.

SELECT foi para o **tema**, que a seção 6 tinha posto num submenu do triângulo. A
troca é boa: a luz do ambiente muda mais vezes por sessão do que qualquer outra
preferência, e não deveria custar três botões.
