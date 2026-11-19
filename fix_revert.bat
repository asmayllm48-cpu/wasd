@echo off
set "LOG=C:\Users\ahmadsy\Desktop\wasd\fix_revert_log.txt"

echo ============================================================ > "%LOG%"
echo   Revert Speed and Duplex to Auto Negotiation >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo [1] Current setting... >> "%LOG%"
powershell -Command "Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' | Format-List DisplayName,DisplayValue" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [2] Reverting to Auto Negotiation... >> "%LOG%"
powershell -Command "Set-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' -DisplayValue 'Auto Negotiation'" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [3] Also reverting Registry SpeedDuplex to 0 (Auto)... >> "%LOG%"
powershell -Command "$regBase = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}'; Get-ChildItem $regBase -ErrorAction SilentlyContinue | ForEach-Object { $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue; if ($p.DriverDesc -match 'Realtek PCIe GbE') { Set-ItemProperty -Path $_.PSPath -Name 'SpeedDuplex' -Value 0 -Type String; echo ('Set SpeedDuplex=0 (Auto) in ' + $_.PSChildName) } }" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [4] Verifying... >> "%LOG%"
powershell -Command "Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' | Format-List DisplayName,DisplayValue" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [5] Current Link Speed... >> "%LOG%"
powershell -Command "(Get-NetAdapter -Name 'Ethernet').LinkSpeed" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Done. Replug cable to apply. >> "%LOG%"
echo ============================================================ >> "%LOG%"
