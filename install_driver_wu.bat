@echo off
set "LOG=C:\Users\ahmadsy\Desktop\wasd\driver_install_log.txt"

echo ============================================================ > "%LOG%"
echo   Realtek Driver Install via pnputil and Windows Update >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo [1] Current driver... >> "%LOG%"
powershell -Command "Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'Realtek PCIe GbE' } | Select-Object DeviceName,DriverVersion,DriverDate,DriverProviderName | Format-List" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [2] Scanning devices for hardware changes... >> "%LOG%"
pnputil /scan-devices >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [3] Trying Windows Update driver search (ServerSelection=2 = Windows Update)... >> "%LOG%"
powershell -Command "$session = New-Object -ComObject Microsoft.Update.Session; $searcher = $session.CreateUpdateSearcher(); $searcher.ServerSelection = 2; Write-Output 'Searching Windows Update for drivers...'; $result = $searcher.Search('IsInstalled=0 AND Type=''Driver'''); Write-Output ('Found ' + $result.Updates.Count + ' updates'); $realtek = @(); foreach ($u in $result.Updates) { Write-Output ('  - ' + $u.Title); if ($u.Title -match 'Realtek') { $realtek += $u } }; if ($realtek.Count -gt 0) { Write-Output ('Found ' + $realtek.Count + ' Realtek updates'); foreach ($u in $realtek) { $u.AcceptEula(); $coll = New-Object -ComObject Microsoft.Update.UpdateColl; $coll.Add($u) | Out-Null; $dl = $session.CreateUpdateDownloader(); $dl.Updates = $coll; Write-Output ('Downloading: ' + $u.Title); $dl.Download(); $inst = $session.CreateUpdateInstaller(); $inst.Updates = $coll; Write-Output ('Installing: ' + $u.Title); $r = $inst.Install(); Write-Output ('ResultCode: ' + $r.ResultCode); Write-Output ('RebootRequired: ' + $r.RebootRequired) } } else { Write-Output 'No Realtek updates found' }" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [4] Also trying Device Manager update (Search Windows Update)... >> "%LOG%"
powershell -Command "Start-Process -FilePath 'rundll32.exe' -ArgumentList 'devmgr.dll,DeviceManager_Execute' -Wait" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [5] Driver after update attempt... >> "%LOG%"
powershell -Command "Get-CimInstance Win32_PnPSignedDriver | Where-Object { $_.DeviceName -match 'Realtek PCIe GbE' } | Select-Object DeviceName,DriverVersion,DriverDate,DriverProviderName | Format-List" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Done. >> "%LOG%"
echo ============================================================ >> "%LOG%"
