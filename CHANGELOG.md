# Histórico

Formato baseado em [Keep a Changelog](https://keepachangelog.com/pt-BR/1.1.0/).
Este projeto usa [versionamento semântico](https://semver.org/lang/pt-BR/).

O desenvolvimento foi conduzido em etapas numeradas, e o histórico abaixo as
reflete. Cada uma foi validada antes da seguinte — as de parser e reflow no
host, com testes automatizados; as de interface no PSP físico.

---

## [1.0.0] — 2026-08-30

Primeira versão pública. Lê EPUB, PDF e TXT com reflow, guarda a posição de
leitura e tem três temas. Validada em PSP-2000 com CFW.

### Formatos

- **EPUB 2 e 3** — leitor de ZIP próprio (diretório central, membros
  armazenados e deflate, leitura em fluxo), `META-INF/container.xml` → OPF →
  manifesto e espinha, XHTML convertido em parágrafos. Ordem de leitura é a da
  espinha; `href` relativo resolvido, inclusive com `../`; prefixo de namespace
  ignorado (`<opf:item>` é o mesmo que `<item>`).
- **PDF com camada de texto** — parser próprio, sem dependência de MuPDF ou
  Bookr (ver [NOTICE](NOTICE) para o porquê). xref clássico e em stream, cadeia
  `/Prev`, object streams, reconstrução por varredura quando a tabela está
  corrompida. Filtros Flate, LZW, ASCII85, ASCIIHex e RunLength, com predictors
  PNG e TIFF. Fontes WinAnsi, MacRoman, StandardEncoding, `/Differences`,
  `/ToUnicode` e Type0/CID.
- **TXT** — codificação detectada (BOM, UTF-8 validado por redundância, senão
  CP1252) e quebra de linha dura desfeita.

### Leitura

- **Reflow** — o texto é remontado em parágrafos lógicos e repaginado para
  480x272. A unidade de leitura é a **tela**, não a página do arquivo, e a
  virada atravessa a fronteira da página sozinha.
- **Hifenização** — palavra partida no fim da linha é remontada, mantendo o
  hífen quando é de composto (`Não-Violenta`) e removendo quando é de quebra.
- **Cabeçalho e rodapé** — título corrente e número de página descartados por
  isolamento vertical, sem comer o título do capítulo.
- **Colunas** — coluna dupla detectada por coincidência de calha e desfeita.
- **Blocos** — epígrafe, citação e verso têm margem própria e sobrevivem
  inteiros; título e bloco centralizado são reconhecidos.
- **Quebra de linha por largura real de glifo**, não por contagem de caracteres.

### Interface

- Biblioteca de `ms0:/PSP/BOOKS/`, com a porcentagem já lida de cada livro.
- Progresso de leitura por livro (posição e corpo de fonte), em arquivo de texto
  em `ms0:/PSP/BOOKS/.ereader/progress.txt`.
- Três temas — Papel, Noite e Claro — em SELECT, com a escolha persistida.
- Corpo de fonte ajustável de 12 a 34 px, repaginando sem perder o parágrafo.
- Painel de diagnóstico do reflow no triângulo.
- Tipografia FreeType com UTF-8 puro de ponta a ponta — sem conversão com perda
  em nenhum ponto do caminho.

### Limitações declaradas

Consequências da arquitetura, não defeitos. Ver a seção correspondente no
[README](README.md#limitações-conhecidas).

- PDF escaneado não é lido: é imagem, não há camada de texto. Sem OCR.
- Tabelas saem embaralhadas: reflow reconstrói fluxo de leitura, e tabela não é
  um fluxo.
- Imagens e figuras são ignoradas nos três formatos.
- EPUB com DRM e PDF criptografado com senha não abrem.
- Parágrafo cortado na virada de unidade ainda não emenda.

---

## Etapas do desenvolvimento

Registro de como se chegou à 1.0. Não são versões publicadas.

### Etapa 4 — Formatos, progresso e tema

Camada `doc`: uma unidade de carga e uma saída em parágrafos lógicos, iguais
para todo formato. É o que fez o terceiro formato custar menos que o segundo, e
o que deixou `main.c` sem nenhuma condicional de formato no caminho de leitura.

Acrescentados TXT e EPUB, progresso de leitura e temas.

Dois defeitos encontrados pelos testes e corrigidos:

- Laço infinito ao abrir EPUB pequeno — a busca do diretório central varre o
  arquivo de trás para frente com sobreposição de 3 bytes, e no último bloco a
  sobreposição era maior que o bloco, devolvendo a posição ao valor anterior.
- Parágrafo desaparecendo entre trechos de um capítulo grande — o corte era por
  bytes, calculado por aritmética dos dois lados, enquanto o limite real também
  era o de parágrafos. Uma unidade densa parava antes do fim da sua faixa e a
  seguinte começava onde a conta mandava. A unidade passou a ser uma faixa de
  bytes explícita, planejada por uma varredura única.

Duas coisas previstas no plano foram descartadas com registro: o índice de
paginação persistido (`.pri`), desnecessário depois que a paginação passou a ser
por unidade, e o modo de página fixa, que exigiria zoom e pan — exatamente o que
o projeto existe para não ser.

### Etapa 3 — Parser de PDF e reflow

- **3a** — tokenizer, modelo de objetos, xref e filtros: abrir o PDF e listar
  páginas.
- **3b** — fontes e extração de texto: o texto do PDF aparecendo na tela, ainda
  com as linhas do papel.
- **3c** — reflow e paginação. As duas correções mais importantes vieram do
  corpus real, não dos testes sintéticos: o título do capítulo sendo comido pela
  detecção de cabeçalho, e a epígrafe quebrada linha a linha porque o recuo era
  medido contra a margem da página em vez da linha anterior.

### Etapa 2 — Ambiente e esqueleto

Makefile PSPSDK em container Docker, scripts de build, deploy com verificação de
hash, e o harness de teste de host que virou a ferramenta central do projeto.
Tipografia FreeType validada no hardware — 18 px aprovado como corpo padrão.

### Etapa 1 — Planejamento

Decisões registradas em [PLAN.md](PLAN.md), com o porquê de cada uma. A que
definiu o projeto: a licença MIT elimina MuPDF (AGPL), Bookr (GPLv2) e intraFont
(CC BY-SA), o que obrigou a escrever o parser do zero — e empurrou o projeto
para uma arquitetura melhor, com UTF-8 puro e o parser inteiro testável no host.
