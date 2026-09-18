# Compila e abre no PPSSPP.
#
# ATENCAO: o PPSSPP nao substitui teste em hardware. Ele e mais tolerante com
# caminho relativo, timing de sceIo e estado sujo do GU. O veredito e sempre o
# PSP fisico - o emulador serve para iterar rapido em logica de UI.
$ppsspp = "C:\Program Files\PPSSPP\PPSSPPWindows64.exe"

& (Join-Path $PSScriptRoot "build.ps1")
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$eboot = Join-Path $PSScriptRoot "EBOOT.PBP"
if (-not (Test-Path $eboot)) { Write-Host "EBOOT.PBP nao encontrado" -ForegroundColor Red; exit 1 }
& $ppsspp $eboot
