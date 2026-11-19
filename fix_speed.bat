@echo off
set "LOG=C:\Users\ahmadsy\Desktop\wasd\fix_speed_log.txt"

echo ============================================================ > "%LOG%"
echo   Speed and Duplex Fix >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo [1] Current Speed and Duplex setting... >> "%LOG%"
powershell -Command "Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' | Format-List DisplayName,DisplayValue,ValidDisplayValues" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [2] Available values... >> "%LOG%"
powershell -Command "(Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex').ValidDisplayValues" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [3] Setting to 1.0 Gbps Full Duplex... >> "%LOG%"
powershell -Command "Set-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' -DisplayValue '1.0 Gbps Full Duplex'" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [4] If above failed, trying Registry approach... >> "%LOG%"
powershell -Command "$regBase = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}'; Get-ChildItem $regBase -ErrorAction SilentlyContinue | ForEach-Object { $p = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue; if ($p.DriverDesc -match 'Realtek PCIe GbE') { Set-ItemProperty -Path $_.PSPath -Name 'SpeedDuplex' -Value 6 -Type String; echo ('Set SpeedDuplex=6 in ' + $_.PSChildName) } }" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [5] Verifying new setting... >> "%LOG%"
powershell -Command "Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' | Format-List DisplayName,DisplayValue" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [6] Current Link Speed... >> "%LOG%"
powershell -Command "(Get-NetAdapter -Name 'Ethernet').LinkSpeed" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Done. You may need to replug the cable or reboot. >> "%LOG%"
echo ============================================================ >> "%LOG%"
