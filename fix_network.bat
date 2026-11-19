@echo off
set "LOG=C:\Users\ahmadsy\Desktop\wasd\fix_network_log.txt"

echo ============================================================ > "%LOG%"
echo   Network Fix Script >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo [1/5] Changing DNS to Cloudflare... >> "%LOG%"
netsh interface ip set dns name="Ethernet" static 1.1.1.1 primary >> "%LOG%" 2>&1
netsh interface ip add dns name="Ethernet" 1.0.0.1 index=2 >> "%LOG%" 2>&1
echo [OK] DNS changed >> "%LOG%"
echo. >> "%LOG%"

echo [2/5] Stopping ProtonVPN Service... >> "%LOG%"
sc stop "ProtonVPN Service" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [3/5] Stopping ProtonVPN WireGuard... >> "%LOG%"
sc stop "ProtonVPN WireGuard" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [4/5] Disabling auto-start... >> "%LOG%"
sc config "ProtonVPN Service" start= demand >> "%LOG%" 2>&1
sc config "ProtonVPN WireGuard" start= demand >> "%LOG%" 2>&1
echo [OK] Auto-start disabled >> "%LOG%"
echo. >> "%LOG%"

echo [5/5] Killing ProtonVPN processes... >> "%LOG%"
taskkill /F /IM ProtonVPN.Client.exe >> "%LOG%" 2>&1
taskkill /F /IM ProtonVPNService.exe >> "%LOG%" 2>&1
taskkill /F /IM ProtonVPN.NrptWatchdog.exe >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo [Extra] Flushing DNS... >> "%LOG%"
ipconfig /flushdns >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Verification >> "%LOG%"
echo ============================================================ >> "%LOG%"
echo. >> "%LOG%"

echo --- DNS Servers --- >> "%LOG%"
netsh interface ip show dnsservers name="Ethernet" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo --- ProtonVPN Services --- >> "%LOG%"
sc query "ProtonVPN Service" >> "%LOG%" 2>&1
echo. >> "%LOG%"
sc query "ProtonVPN WireGuard" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo --- ProtonVPN Processes --- >> "%LOG%"
tasklist /FI "IMAGENAME eq ProtonVPN*" >> "%LOG%" 2>&1
echo. >> "%LOG%"

echo ============================================================ >> "%LOG%"
echo   Done. >> "%LOG%"
echo ============================================================ >> "%LOG%"
