# Compila e copia o build para o PSP.
#
# Detecta o drive automaticamente procurando por PSP\GAME, em vez de confiar
# numa letra fixa: as letras mudam conforme o que esta conectado.
#
#   .\deploy.ps1              -> detecta o PSP
#   .\deploy.ps1 -Drive F:    -> forca um drive
param([string]$Drive = "")

$root = $PSScriptRoot

& (Join-Path $root "build.ps1") dist
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not $Drive) {
    $found = Get-CimInstance Win32_LogicalDisk | Where-Object {
        $_.DeviceID -and (Test-Path (Join-Path $_.DeviceID "PSP\GAME"))
    }
    if (-not $found) {
        Write-Host "PSP nao encontrado. Conecte o USB (Configuracoes > Conexao USB)" -ForegroundColor Red
        Write-Host "ou passe o drive: .\deploy.ps1 -Drive F:" -ForegroundColor Red
        exit 1
    }
    if ($found.Count -gt 1) {
        Write-Host "mais de um candidato encontrado:" -ForegroundColor Yellow
        $found | ForEach-Object { Write-Host "  $($_.DeviceID)  $($_.VolumeName)" }
        Write-Host "escolha com -Drive" -ForegroundColor Yellow
        exit 1
    }
    $Drive = $found.DeviceID
    Write-Host "PSP detectado em $Drive" -ForegroundColor Cyan
}

$dest = Join-Path $Drive "PSP\GAME\EREADER"
New-Item -ItemType Directory -Force -Path $dest | Out-Null
Copy-Item -Recurse -Force (Join-Path $root "dist\EREADER\*") $dest

# A pasta de livros fica FORA da pasta do app, para sobreviver a uma
# reinstalacao do homebrew. Criada aqui para o usuario ter onde por os PDFs.
$books = Join-Path $Drive "PSP\BOOKS"
if (-not (Test-Path $books)) {
    New-Item -ItemType Directory -Force -Path $books | Out-Null
    Write-Host "criada $books - ponha seus PDFs ai" -ForegroundColor Cyan
}

# Confere o que chegou: copia truncada para Memory Stick produz sintomas
# esquisitos e dificeis de diagnosticar depois.
$src = Get-Item (Join-Path $root "dist\EREADER\EBOOT.PBP")
$dst = Get-Item (Join-Path $dest "EBOOT.PBP")
$srcHash = (Get-FileHash $src.FullName -Algorithm SHA1).Hash
$dstHash = (Get-FileHash $dst.FullName -Algorithm SHA1).Hash

if ($srcHash -ne $dstHash) {
    Write-Host "ATENCAO: hash do EBOOT copiado NAO confere" -ForegroundColor Red
    Write-Host "  local $srcHash"
    Write-Host "  PSP   $dstHash"
    exit 1
}
Write-Host "copiado para $dest" -ForegroundColor Green
Write-Host "EBOOT verificado: $srcHash ($($src.Length) bytes)" -ForegroundColor Green

# A fonte tem ~1 MB e e o unico asset critico: sem ela nada aparece na tela.
$fontDst = Join-Path $dest "data\Literata.ttf"
if (Test-Path $fontDst) {
    $fSrc = (Get-FileHash (Join-Path $root "data\Literata.ttf") -Algorithm SHA1).Hash
    $fDst = (Get-FileHash $fontDst -Algorithm SHA1).Hash
    if ($fSrc -ne $fDst) {
        Write-Host "ATENCAO: hash da fonte copiada NAO confere" -ForegroundColor Red
        exit 1
    }
    Write-Host "fonte verificada: Literata.ttf" -ForegroundColor Green
} else {
    Write-Host "ATENCAO: data\Literata.ttf nao chegou ao PSP" -ForegroundColor Red
    exit 1
}

Write-Host "Use 'Desconectar USB' no PSP antes de tirar o cabo." -ForegroundColor Cyan
