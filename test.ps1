# Testa os modulos PORTAVEIS no HOST, sem PSP.
#
# Este script e a ferramenta central do projeto, nao um extra. A maior parte do
# codigo daqui e parser de PDF e reflow (src/pdf, src/read), que e C99 puro sem
# nenhuma dependencia do PSPSDK. Depurar um tokenizer de content stream pelo
# ciclo "compila -> copia pro Memory Stick -> desconecta USB -> abre no XMB ->
# olha a tela" seria insustentavel. Aqui o ciclo e de segundos.
#
# Uso:
#   .\test.ps1                 -> roda todos os testes
#   .\test.ps1 utf8            -> roda so tools/test_utf8.c
#   .\test.ps1 pdf "livro.pdf" -> roda tools/test_pdf.c com um argumento
param(
    [string]$Which = "",
    [Parameter(ValueFromRemainingArguments=$true)][string[]]$TestArgs
)

$root = $PSScriptRoot
$img  = "ereader-tools"

# Imagem derivada com gcc nativo; criada na primeira execucao.
if (-not (docker images -q $img)) {
    Write-Host "criando imagem $img (uma vez)..." -ForegroundColor Yellow
    docker build -q -t $img -f "$root\tools\Dockerfile.hosttest" $root | Out-Null
    if ($LASTEXITCODE -ne 0) { Write-Host "falha ao criar imagem" -ForegroundColor Red; exit 1 }
}

# Cada teste declara os fontes portaveis de que precisa e as bibliotecas extras.
# Manter isso explicito evita arrastar src/psp para o build de host, que nao
# compilaria - e e o que garante que a camada PDF continue sem dependencia de
# PSPSDK.
$pdfSrcs = "src/pdf/pdf_arena.c src/pdf/pdf_io.c src/pdf/pdf_obj.c " +
           "src/pdf/pdf_lex.c src/pdf/pdf_filt.c src/pdf/pdf_doc.c " +
           "src/pdf/pdf_font.c src/pdf/pdf_text.c src/read/utf8.c"

# A camada de leitura. reflow e layout nao dependem do parser para compilar, mas
# textlines.h inclui pdf_text.h pelo tipo do run, entao o conjunto anda junto.
$readSrcs = "src/read/textlines.c src/read/reflow.c src/read/layout.c " +
            "src/read/txt.c src/read/zip.c src/read/epub.c src/read/doc.c"

$suites = [ordered]@{
    "utf8"      = @{ srcs = "src/read/utf8.c";                        libs = "" }
    "pdfcore"   = @{ srcs = $pdfSrcs;                                 libs = "-lz" }
    "reflow"    = @{ srcs = "$pdfSrcs $readSrcs";                     libs = "-lz" }
    "txt"       = @{ srcs = "$pdfSrcs $readSrcs tools/host_io.c";     libs = "-lz" }
    "epub"      = @{ srcs = "$pdfSrcs $readSrcs tools/host_io.c";     libs = "-lz" }
    "epubdump"  = @{ srcs = "$pdfSrcs $readSrcs tools/host_io.c";     libs = "-lz" }
    "progress"  = @{ srcs = "src/read/progress.c tools/host_fs.c";    libs = "" }
    "pdf"       = @{ srcs = "$pdfSrcs tools/host_io.c";               libs = "-lz" }
    "pdftext"   = @{ srcs = "$pdfSrcs tools/host_io.c";               libs = "-lz" }
    "pdfreflow" = @{ srcs = "$pdfSrcs $readSrcs tools/host_io.c";     libs = "-lz" }
}

if ($Which) {
    if (-not $suites.Contains($Which)) {
        Write-Host "teste desconhecido: $Which" -ForegroundColor Red
        Write-Host "disponiveis: $($suites.Keys -join ', ')" -ForegroundColor Yellow
        exit 1
    }
    $run = @($Which)
} else {
    # As suites que precisam de um arquivo como argumento ficam fora da execucao
    # completa; rode com  .\test.ps1 pdf "caminho.pdf"
    $needFile = @("pdf", "pdftext", "pdfreflow", "epubdump")
    $run = @($suites.Keys | Where-Object { $needFile -notcontains $_ })
}

# Argumentos que sao caminhos de arquivo existente precisam estar visiveis DENTRO
# do container. Montamos a pasta de cada um como /corpus0, /corpus1... e
# reescrevemos o argumento. Sem isso, passar um PDF de F:\ falha com "nao abriu"
# apontando para um caminho que existe no Windows mas nao no container - erro que
# parece bug do parser e nao de montagem.
#
# Montado somente-leitura: um teste nao tem por que poder escrever no corpus.
[string[]]$mounts = @()
$rewritten = @()
$ci = 0

foreach ($arg in $TestArgs) {
    if ($arg -and (Test-Path -LiteralPath $arg -PathType Leaf)) {
        $item = Get-Item -LiteralPath $arg
        $dir  = $item.DirectoryName
        $mnt  = "/corpus$ci"
        $mounts += @("-v", "${dir}:${mnt}:ro")
        $rewritten += "'$mnt/$($item.Name)'"
        $ci++
    } elseif ($arg) {
        # .Replace() de string, nao -replace: o operador -replace usa regex, e
        # uma barra invertida sozinha nao e um padrao valido.
        $rewritten += "'" + $arg.Replace('\','/') + "'"
    }
}
$extra = $rewritten -join " "

$failed = @()
foreach ($name in $run) {
    $s = $suites[$name]
    Write-Host "== $name ==" -ForegroundColor Cyan

    $cmd = "gcc -Wall -Wextra -std=c99 -g -fsanitize=address,undefined " +
           "-Iinclude -Itools -o /tmp/t_$name tools/test_$name.c $($s.srcs) " +
           "$($s.libs) -lm && /tmp/t_$name $extra"

    docker run --rm -v "${root}:/src" @mounts -w /src $img sh -c $cmd
    if ($LASTEXITCODE -ne 0) { $failed += $name }
}

if ($failed.Count -gt 0) {
    Write-Host "FALHOU: $($failed -join ', ')" -ForegroundColor Red
    exit 1
}
Write-Host "TODOS OS TESTES OK" -ForegroundColor Green
