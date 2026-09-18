# Compila dentro do container pspdev. Uso:
#   .\build.ps1            -> make
#   .\build.ps1 clean      -> make clean
#   .\build.ps1 dist       -> gera dist/EREADER/
param([Parameter(ValueFromRemainingArguments=$true)][string[]]$MakeArgs)

$root  = $PSScriptRoot
$image = "pspdev/pspdev:latest"

# $mk precisa ser array tipado: splat de string escalar vira splat de CARACTERES.
# E nao use o nome $args: e variavel automatica do PowerShell.
[string[]]$mk = @()
if ($MakeArgs) { $mk = @($MakeArgs) }

docker run --rm -v "${root}:/src" -w /src $image make @mk
if ($LASTEXITCODE -ne 0) { Write-Host "BUILD FALHOU" -ForegroundColor Red; exit $LASTEXITCODE }
Write-Host "BUILD OK -> EBOOT.PBP" -ForegroundColor Green
