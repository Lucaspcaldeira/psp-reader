# PSP Reader

**Um leitor de livros para PlayStation Portable que trata a tela de 480x272 como
uma página, e não como uma janelinha sobre uma folha A4.**

Lê **EPUB**, **PDF** e **TXT**. O texto é remontado em parágrafos lógicos e
repaginado para a tela, então se lê virando telas inteiras — com palavra
hifenizada inteira de volta, cabeçalho e número de página fora do caminho, e o
corpo da fonte no tamanho que você quiser.

[![testes](https://github.com/Lucaspcaldeira/psp-reader/actions/workflows/tests.yml/badge.svg)](https://github.com/Lucaspcaldeira/psp-reader/actions/workflows/tests.yml)
[![licença: MIT](https://img.shields.io/badge/licen%C3%A7a-MIT-blue.svg)](LICENSE)
[![plataforma: PSP](https://img.shields.io/badge/plataforma-PSP--2000%2F3000-black.svg)](#como-instalar-usuário-final)
[![formatos](https://img.shields.io/badge/formatos-EPUB%20%C2%B7%20PDF%20%C2%B7%20TXT-green.svg)](#formatos)

---

## O problema

A tela do PSP tem 480x272. Uma página A4 tem 595x842 pontos, com o texto em
coordenadas absolutas. Reduzir a página inteira para caber deixa o corpo do
texto em cerca de **4 pixels** — ilegível. A saída óbvia é zoom e *pan* em duas
dimensões, que foi o que os leitores de PSP anteriores fizeram, e que não é ler
um livro: é navegar num mapa.

Este projeto faz o contrário. Extrai o texto com as posições, **reconstrói os
parágrafos** e repagina para 480x272 com quebra de linha por largura real de
glifo. É o que um Kindle faz.

Em EPUB o problema nem existe — o formato já traz `<p>` dizendo onde o parágrafo
termina. Em PDF não existe marca nenhuma: só há coordenadas, e a resposta sai de
heurística sobre elas. Essa diferença é a parte mais interessante do código, e
está documentada em [PLAN.md](PLAN.md).

---

## Como fica

Uma página de PDF diagramada para papel, depois do reflow e repaginada para a
tela do console. Saída real da ferramenta de teste (`.\test.ps1 pdfreflow`), não
uma maquete:

```
    +-----------------------------------------------------+ tela 2/6
    |    Ao estudar a questão do que nos afasta de nosso estado
    | natural de compaixão, identifiquei algumas formas
    | específicas de linguagem e comunicação que acredito
    | contribuírem para nosso comportamento violento em
    | relação aos outros e a nós mesmos. Para designar essas
    | formas de comunicação, utilizo a expressão
    | “comunicação alienante da vida”.
    +-----------------------------------------------------+
```

No papel, esse parágrafo eram seis linhas justificadas em 458 pontos de largura;
na tela, sete linhas em 452 pixels, quebradas onde a fonte de leitura manda.
Repare na pontuação tipográfica preservada — aspas curvas, acentuação — que é o
que se perde quando um leitor converte para Latin-1 pelo caminho.

> **Capturas de tela do console:** ainda não há. Se você rodar no seu PSP e
> quiser contribuir com uma, é bem-vinda.

---

## Ambiente

| Item | Escolha |
|---|---|
| Toolchain | `pspdev/pspdev:latest` via Docker — nada instalado no Windows |
| Emulador | PPSSPP (`C:\Program Files\PPSSPP\PPSSPPWindows64.exe`) |
| Hardware | PSP-2000/3000 com CFW, deploy em `ms0:/PSP/GAME/EREADER/` |
| Render | `sceGu` nativo |
| Texto | FreeType 2 + Literata (OFL), UTF-8 puro |

### Comandos

```powershell
.\build.ps1          # compila -> EBOOT.PBP
.\build.ps1 clean    # limpa objetos
.\build.ps1 dist     # empacota dist\EREADER\
.\run.ps1            # compila e abre no PPSSPP
.\deploy.ps1         # compila e copia pro PSP (detecta o drive, verifica hash)

.\test.ps1                        # todas as suites de host, sem PSP
.\test.ps1 utf8                   # decodificador UTF-8
.\test.ps1 pdfcore                # arena, lexer, parser, filtros, busca
.\test.ps1 reflow                 # paragrafos, hifenizacao, colunas, paginacao
.\test.ps1 txt                    # codificacao, quebra dura, indice de blocos
.\test.ps1 epub                   # ZIP, OPF, espinha, XHTML -> paragrafos
.\test.ps1 progress               # ida e volta do progresso de leitura

# as quatro abaixo recebem um arquivo e ficam fora da execucao completa
.\test.ps1 pdf       "testdata\x.pdf"           # o que o parser entendeu
.\test.ps1 pdftext   "testdata\x.pdf" 20        # runs e linhas de uma pagina
.\test.ps1 pdfreflow "testdata\x.pdf" 20 3      # paragrafos + telas repaginadas
.\test.ps1 pdfreflow "testdata\x.pdf" 20 1 18 linhas   # + coordenadas de origem
.\test.ps1 epubdump  "testdata\x.epub" 0 3 telas       # estrutura + repaginado
```

Ponha PDFs em `testdata/` (ignorada pelo git — os livros não são
redistribuíveis). Caminhos fora do projeto são montados automaticamente no
container, exceto em drives removíveis, que o Docker Desktop não compartilha.

A divisão entre suíte **sintética** e suíte **de corpus** é deliberada, e vale
para os três formatos. As sintéticas (`reflow`, `txt`, `epub`) constroem o
arquivo que exercitam — inclusive um EPUB completo, com ZIP e deflate de
verdade — porque a única forma de julgar uma heurística é dar a ela o caso que
precisa acertar **e o caso vizinho que não deve confundir com aquele**: não se
pede a um PDF real "agora a mesma página, mas alinhada à esquerda". As de corpus
(`pdfreflow`, `epubdump`) respondem a outra pergunta: as regras acertam juntas,
num arquivo que ninguém produziu pensando nelas. As correções de reflow mais
importantes até agora saíram delas.

O `deploy.ps1` detecta o drive procurando por `PSP\GAME` em vez de confiar numa
letra fixa, confere o SHA1 do EBOOT e da fonte depois de copiar (cópia truncada
para Memory Stick produz sintomas difíceis de diagnosticar depois) e cria
`ms0:/PSP/BOOKS/`.

---

## Estrutura

```
ereader/
├── PLAN.md               # decisoes tecnicas da Etapa 1 - leia primeiro
├── Makefile              # build PSPSDK (roda dentro do container)
├── build.ps1 / run.ps1 / deploy.ps1 / test.ps1
├── include/              # headers publicos
├── src/
│   ├── main.c            # a interface, e so ela
│   ├── pdf/              # parser de PDF                     - PORTAVEL
│   ├── read/             # doc, txt, zip, epub, reflow,
│   │                     # layout, progresso                 - PORTAVEL
│   └── psp/              # gfx, input, font, fs, tema - especifico do console
├── data/Literata.ttf     # fonte de leitura (OFL 1.1), vai pro PSP
├── tools/                # harness de teste de host
└── dist/EREADER/         # gerado; conteudo copiado pro Memory Stick
```

A separação **portável / PSP** é a decisão central do projeto. Os três parsers,
o motor de reflow e a paginação são C99 puro sobre uma interface de I/O de três
funções, e são desenvolvidos e depurados no host com `.\test.ps1` — ciclo de
segundos, com ASan e UBSan. Só `gfx`, `input`, `font`, `fs` e `theme` tocam o
hardware. Ver [PLAN.md](PLAN.md) seção 3.

A segunda decisão é a camada `doc`: uma **unidade de carga** e uma saída em
**parágrafos lógicos**, iguais para os três formatos. É o que faz `main.c` não
ter uma única condicional de formato no caminho de leitura.

---

## Como instalar (usuário final)

1. Copie a pasta `EREADER` para `ms0:/PSP/GAME/` do seu Memory Stick.
2. Ponha seus `.epub`, `.pdf` e `.txt` em `ms0:/PSP/BOOKS/`.
3. Abra pelo XMB em *Jogo → Memory Stick*.

Os livros ficam **fora** da pasta do app de propósito: apagar
`ms0:/PSP/GAME/EREADER/` para reinstalar não leva a biblioteca junto. O
progresso de leitura fica em `ms0:/PSP/BOOKS/.ereader/`, pelo mesmo motivo.

---

## Controles

A navegação é uma pilha de três níveis, e **O** sempre volta um:

```
BIBLIOTECA  --X-->  DOCUMENTO  --X-->  LEITURA
```

| Botão | BIBLIOTECA | DOCUMENTO | LEITURA | AMOSTRA / DIAGNOSTICO |
|---|---|---|---|---|
| cima / baixo | move o cursor | — | corpo da fonte | corpo da fonte |
| esquerda / direita | salta uma tela | — | tela anterior / próxima | rola o texto |
| L / R | alterna as três telas | — | tela anterior / próxima | alterna as telas |
| X | abre o documento | começa a ler | — | — |
| □ / △ | relê a pasta | — | △ diagnóstico do reflow | — |
| **SELECT** | **tema** | **tema** | **tema** | **tema** |
| O | — | volta | volta | — |
| START | sair | sair | sair | sair |

Na leitura a unidade é a **tela**, não a página do PDF: uma página A4 rende
cinco ou seis telas, e a virada atravessa a fronteira da página sozinha — quem
lê não deveria precisar saber que ela existe. Voltar da primeira tela de uma
página cai na **última** tela da anterior, e não na primeira.

Trocar o corpo da fonte repagina o livro e **devolve o leitor ao mesmo
parágrafo** — guardar o número da tela não serviria, porque com corpo maior há
mais telas e a tela 3 de antes não é a tela 3 de agora.

### Onde você parou

Cada livro guarda a posição e o corpo de fonte em que estava sendo lido, em
`ms0:/PSP/BOOKS/.ereader/progress.txt` — um arquivo de texto, legível e
editável num PC, porque um progresso perdido é um aborrecimento pequeno e poder
consertá-lo à mão vale mais que os bytes economizados.

A chave é **nome + tamanho** do arquivo: substituir um livro por outra edição de
mesmo nome faz o leitor tratá-lo como novo, em vez de abrir numa página
arbitrária. O que se guarda é o **parágrafo**, não o número da tela, pelo mesmo
motivo da troca de corpo.

A biblioteca mostra a **porcentagem lida** de cada livro já começado, antes de
abrir — é o que transforma a lista de arquivos numa estante.

A gravação acontece na virada de **unidade** (página do PDF, bloco do TXT,
trecho do EPUB), e não a cada tela: a cada tela seriam várias escritas por
minuto no Memory Stick, e só no fechamento se perderia a sessão inteira quando o
console é desligado no botão — que é como um PSP é desligado.

### Temas

**SELECT** alterna entre três, e a escolha persiste:

| Tema | Para quê |
|---|---|
| **Papel** | creme quente sobre tinta escura; leitura longa com luz normal |
| **Noite** | fundo escuro, sem preto puro — branco sobre preto produz halo na LCD do PSP |
| **Claro** | contraste máximo, para sol |

---

## Formatos

| Formato | Estado |
|---|---|
| **EPUB** (2 e 3) | lido pela espinha, capítulo a capítulo |
| **PDF** (com camada de texto) | lido, reflowed e repaginado |
| **TXT** (UTF-8, Latin-1, CP1252) | lido, com parágrafos remontados |
| PDF escaneado (imagem pura) | detectado e informado; sem OCR, não há texto para extrair |
| MOBI, AZW, DOC | não |

Os três formatos convergem para a mesma saída — **parágrafos lógicos** — e daí
para a mesma paginação. Nada acima dessa camada sabe de que formato o texto
veio, e é por isso que o terceiro formato custou menos que o segundo.

Cada um chega lá pelo caminho que lhe cabe, e o contraste é a coisa mais
interessante do projeto:

| Formato | Como sabe onde termina o parágrafo |
|---|---|
| **EPUB** | tem um `<p>` dizendo. Nenhuma heurística é necessária — a informação nunca foi perdida |
| **TXT** | linha em branco e recuo: sinais explícitos, mas pobres |
| **PDF** | **não sabe.** Só há coordenadas, e a resposta sai de heurística sobre elas |

### EPUB

- **ZIP** — diretório central lido do fim do arquivo, membros armazenados e
  deflate, leitura em fluxo (nunca carrega o livro na RAM)
- **Container e OPF** — `META-INF/container.xml` → OPF → manifesto e espinha,
  com `href` relativo resolvido (inclusive `../`) e **prefixo de namespace
  ignorado** (`<opf:item>` é o mesmo que `<item>`)
- **Ordem de leitura** — é a da espinha, não a do manifesto nem a do ZIP
- **XHTML** — tags de bloco viram fronteira de parágrafo, tags inline somem sem
  separar palavra, `<script>`/`<style>`/`<head>` não viram texto, entidades
  nomeadas / decimais / hexadecimais decodificadas, `<h1>`–`<h6>` marcados
- **Capítulo grande** — cortado em trechos por uma varredura única na abertura,
  respeitando ao mesmo tempo o limite de bytes e o de parágrafos

### TXT

- **Codificação adivinhada** — BOM, senão UTF-8 validado por redundância, senão
  CP1252. Assumir UTF-8 num arquivo Latin-1 põe um losango em cada acento, o que
  num livro em português é várias vezes por linha
- **Quebra dura desfeita** — o feitio de todo Project Gutenberg é quebra em ~70
  colunas; reexibi-la em 480 px daria o mesmo defeito do PDF sem reflow

### PDF

O que a camada de PDF já faz:

- **Estrutura** — xref clássico e xref stream, cadeia `/Prev`, object streams,
  reconstrução do xref por varredura quando a tabela está corrompida, árvore de
  páginas com salto por `/Count`
- **Filtros** — Flate, LZW, ASCII85, ASCIIHex, RunLength, com predictors PNG e
  TIFF
- **Fontes** — WinAnsi, MacRoman e StandardEncoding, `/Differences` com nomes de
  glifo (subconjunto da AGL mais `uniXXXX`), CMaps `/ToUnicode` com `bfchar` e
  `bfrange`, fontes Type0/CID com `/W` nas duas formas
- **Texto** — máquina de estado completa do content stream (`BT`/`ET`, `Tm`,
  `Td`, `TD`, `T*`, `TL`, `Tc`, `Tw`, `Tz`, `Ts`, `Tj`, `TJ`, `'`, `"`), matrizes
  `cm`/`q`/`Q`, recursão em Form XObjects, e blocos `BI..EI` de imagem embutida
  pulados com segurança
- **Linhas** — `/Rotate` desfeito e `/MediaBox` transladado (uma página girada
  sairia como uma coluna de sílabas), texto vertical de marginália descartado,
  agrupamento por Y, ordenação por X, inserção de espaço por lacuna e
  normalização de espaço em branco

E o que o reflow faz em cima disso:

- **Parágrafos** — junta as linhas do papel em parágrafos lógicos por lacuna
  vertical, mudança de margem e linha curta, com a regra de linha curta
  escolhida conforme o texto seja **justificado** ou **alinhado à esquerda** —
  errar isso quebraria cada linha num parágrafo, que é o defeito que o reflow
  existe para corrigir
- **Hifenização** — remonta a palavra partida no fim da linha, mantendo o hífen
  quando ele é de composto (`Não-Violenta`) e removendo quando é de quebra
  (`gene-` + `roso`)
- **Cabeçalho e rodapé** — descarta título corrente e número de página pelo
  isolamento vertical, sem comer o título do capítulo
- **Colunas** — detecta calha vertical por coincidência de X entre as linhas e
  desfaz a junção, sem confundir com sumário pontilhado ou tabela
- **Blocos** — epígrafe, citação e verso têm margem própria e sobrevivem
  inteiros; título e bloco centralizado são reconhecidos
- **Paginação** — quebra de linha por largura real de glifo (não por contagem de
  caracteres) e telas de 480x272 com recuo de primeira linha, sem deixar título
  sozinho no pé da tela

O que ficou de fora, e por quê:

- **Índice de paginação persistido** (`.pri`) — o [PLAN.md](PLAN.md) o previa
  para evitar repaginar a cada abertura. Acabou não sendo necessário: a
  paginação é por unidade, custa milissegundos, e um índice em disco só
  agregaria uma chave de invalidação para errar. Fica registrado como decisão,
  não como pendência.
- **Modo de página fixa** (SELECT no PLAN) — seria zoom e pan sobre a página do
  PDF, que é exatamente o que a [seção 1 do PLAN.md](PLAN.md) argumenta **não**
  ser leitura de livro. Com o reflow funcionando, ele deixou de ter propósito.
  SELECT foi para o tema.
- **Emenda de parágrafo entre unidades** — as marcas (`RF_OPEN` / `RF_CONT`) já
  são produzidas pelos três formatos, mas juntar exige duas unidades residentes
  ao mesmo tempo.

---

## Limitações conhecidas

Nenhuma destas é bug: são consequências diretas de "parser próprio + reflow", e
estavam declaradas desde a Etapa 1 ([PLAN.md](PLAN.md) seção 8).

| Limitação | Por quê |
|---|---|
| **PDF escaneado** não é lido | É imagem, não há camada de texto para extrair. O app detecta e informa; não há OCR. |
| **Tabelas** saem embaralhadas | Reflow reconstrói *fluxo de leitura*, e uma tabela não é um fluxo. As células viram texto corrido. |
| **Fontes CID sem `/ToUnicode`** viram `?` | O PDF não diz a que caractere o glifo corresponde. A informação não está no arquivo. |
| **Imagens e figuras** são ignoradas | Modo reflow é só texto, nos três formatos. |
| **PDF criptografado** só com senha vazia | O resto é recusado com mensagem clara. |
| Parágrafo cortado na **virada de unidade** ainda não emenda | Juntar exige duas unidades residentes ao mesmo tempo; as marcas já estão no lugar (`RF_OPEN` / `RF_CONT`). |
| **EPUB com DRM** não abre | É criptografia, não formato. Recusado com mensagem. |
| **ZIP64** não é lido | EPUB acima de 4 GB não existe na prática. |
| Unidade densa demais pode **truncar** | Teto interno de parágrafos. Quando acontece, a barra de topo mostra **`!`** — nunca em silêncio. |

O reflow é heurística, não algoritmo — não existe no arquivo nenhuma marca
dizendo "aqui termina o parágrafo", só coordenadas. Numa página que sair errada,
**△ na leitura** abre o painel de diagnóstico com o que o reflow viu: quantas
linhas de origem, quantas descartou, o corpo e o entrelinha que inferiu, as
margens, e se algo foi truncado. É isso que transforma "a página 84 ficou
estranha" num relato que aponta para uma regra.

---

## Compilar

Nada precisa ser instalado no Windows além do Docker: o toolchain roda no
container `pspdev/pspdev`, e o `build.ps1` cuida de baixá-lo na primeira vez.

```powershell
git clone https://github.com/Lucaspcaldeira/psp-reader.git
cd psp-reader
.\build.ps1          # -> EBOOT.PBP
.\build.ps1 dist     # -> dist\EREADER\ pronto para copiar
.\deploy.ps1         # -> copia pro PSP e verifica o hash
```

Para rodar os testes de host — que não precisam de PSP nem de emulador — veja
[Comandos](#comandos) acima. Eles são a forma mais rápida de verificar que a
árvore está sã depois de um `clone`.

Em Linux ou macOS não há scripts prontos, mas nada impede: o `Makefile` é
PSPSDK padrão e roda dentro do mesmo container.

---

## Créditos

Escrito por **Lucas Caldeira**.

O parser de PDF, o leitor de ZIP/EPUB, o reflow e a paginação são código
próprio, escritos do zero — não há fork nem trechos de outro leitor. Ver
[PLAN.md](PLAN.md) seção 2 para o motivo, que é de licença e acabou sendo também
de arquitetura.

Bibliotecas de terceiros: **FreeType 2**, **zlib**, **libpng**, **bzip2** e o
**PSPSDK**. Tipografia **Literata**, do Literata Project. As atribuições
completas estão em [NOTICE](NOTICE).

Agradecimento ao projeto **pspdev**, cujo toolchain em container é o que torna
razoável desenvolver para PSP hoje.

---

## Histórico

Ver [CHANGELOG.md](CHANGELOG.md).

---

## Licença

Código: **MIT** (ver [LICENSE](LICENSE)).

Dependências de terceiros, todas permissivas e nenhuma copyleft: FreeType (FTL),
zlib, libpng, bzip2, PSPSDK e a fonte Literata (OFL 1.1). Ver [NOTICE](NOTICE)
para as atribuições e para a lista do que foi deliberadamente **não** usado —
MuPDF (AGPL-3.0), Bookr (GPLv2) e intraFont (CC BY-SA 3.0) são incompatíveis
com MIT.

A restrição de licença não foi um obstáculo contornado: foi o que definiu a
arquitetura. Sem poder usar MuPDF nem intraFont, o projeto ganhou um parser
testável no host e UTF-8 puro de ponta a ponta.
