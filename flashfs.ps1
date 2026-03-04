$pio      = "C:\Users\Jarrod\.platformio\penv\Scripts\pio.exe"
$env_name = "seeed_xiao_esp32s3"

function Run($label, $args_list) {
    Write-Host "`n==> $label" -ForegroundColor Cyan
    & $pio run -e $env_name @args_list
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAILED: $label (exit $LASTEXITCODE)" -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "OK: $label" -ForegroundColor Green
}

Run "Build LittleFS image"  @("-t", "buildfs")
Run "Upload LittleFS"       @("-t", "uploadfs")

Write-Host "`nFilesystem flashed!" -ForegroundColor Green
