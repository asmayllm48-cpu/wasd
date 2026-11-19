@echo off
set "LOG=C:\Users\ahmadsy\Desktop\wasd\driver_update_log.txt"

echo ============================================================ > "%LOG%"
echo   Driver Update Attempt >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo [1] Current driver info... >> "%LOG%"
powershell -Command "Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'Realtek PCIe GbE' } | Select-Object DeviceName,DriverVersion,DriverDate,DriverProviderName,InfName | Format-List" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [2] Scanning for updated drivers via pnputil... >> "%LOG%"
pnputil /scan-devices >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [3] Searching Windows Update for Realtek driver... >> "%LOG%"
powershell -Command "$UpdateSession = New-Object -ComObject Microsoft.Update.Session; $UpdateSearcher = $UpdateSession.CreateUpdateSearcher(); $UpdateSearcher.ServiceID = '7971f918-a847-4430-9279-4a52d1efe18d'; $UpdateSearcher.SearchScope = 1; $UpdateSearcher.ServerSelection = 3; $SearchResult = $UpdateSearcher.Search('IsInstalled=0 AND Type=''Driver'''); Write-Output ('Found ' + $SearchResult.Updates.Count + ' driver updates'); foreach ($Update in $SearchResult.Updates) { Write-Output ('---'); Write-Output ('Title: ' + $Update.Title); Write-Output ('Driver: ' + $Update.DriverClass); Write-Output ('HardwareID: ' + ($Update.DriverHardwareID -join ', ')); if ($Update.Title -match 'Realtek') { Write-Output '*** REALTEK UPDATE FOUND ***' } }" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [4] Listing all installed Realtek drivers... >> "%LOG%"
pnputil /enum-drivers >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Done. >> "%LOG%"
echo ============================================================ >> "%LOG%"
