# سكربت إصلاح الشبكة - يرفع نفسه كمدير تلقائياً
$logFile = "C:\Users\ahmadsy\Desktop\wasd\fix_network_log.txt"

# التحقق من صلاحيات المدير ورفع نفسه إن لزم
$isAdmin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $isAdmin) {
    Start-Process powershell -ArgumentList "-ExecutionPolicy Bypass -NoProfile -File `"$PSCommandPath`"" -Verb RunAs
    exit
}

Start-Transcript -Path $logFile -Force | Out-Null

Write-Output "============================================================"
Write-Output "  بدء إصلاح مشاكل الشبكة - يعمل بصلاحيات مدير"
Write-Output "============================================================"
Write-Output ""

# ----------------------------------------------------------
# 1. تغيير DNS إلى Cloudflare (1.1.1.1 / 1.0.0.1)
# ----------------------------------------------------------
Write-Output "[1/5] تغيير DNS إلى Cloudflare..."
try {
    # تعيين DNS أساسي
    netsh interface ip set dns name="Ethernet" static 1.1.1.1 primary 2>&1 | Out-Null
    # إضافة DNS ثانوي
    netsh interface ip add dns name="Ethernet" 1.0.0.1 index=2 2>&1 | Out-Null
    Write-Output "  [OK] تم تغيير DNS"

    # التحقق
    $dnsResult = netsh interface ip show dnsservers name="Ethernet" 2>&1
    Write-Output "  النتيجة:"
    Write-Output $dnsResult
} catch {
    Write-Output "  [ERROR] $($_.Exception.Message)"
}
Write-Output ""

# ----------------------------------------------------------
# 2. تغيير Speed & Duplex إلى 1.0 Gbps Full Duplex
# ----------------------------------------------------------
Write-Output "[2/5] تغيير Speed & Duplex إلى 1.0 Gbps Full Duplex..."
try {
    # محاولة عبر Set-NetAdapterAdvancedProperty
    $speedOptions = Get-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' -ErrorAction Stop
    Write-Output "  القيمة الحالية: $($speedOptions.DisplayValue)"
    Write-Output "  القيم المتاحة:"
    $speedOptions.ValidDisplayValues | ForEach-Object { Write-Output "    - $_" }

    # البحث عن 1.0 Gbps Full Duplex
    $targetValue = $speedOptions.ValidDisplayValues | Where-Object { $_ -match '1\.0 Gbps.*Full' -or $_ -match '1 Gbps.*Full' } | Select-Object -First 1

    if ($targetValue) {
        Write-Output "  المحاولة بتعيين: $targetValue"
        Set-NetAdapterAdvancedProperty -Name 'Ethernet' -DisplayName 'Speed & Duplex' -DisplayValue $targetValue -ErrorAction Stop
        Write-Output "  [OK] تم تغيير Speed & Duplex إلى: $targetValue"
    } else {
        Write-Output "  [WARN] لم يتم العثور على خيار 1.0 Gbps Full Duplex"
        Write-Output "  المحاولة عبر Registry..."
        # محاولة عبر Registry - SpeedDuplex value 6 = 1Gbps Full Duplex
        $regBase = "HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e972-e325-11ce-bfc1-08002be10318}"
        $found = $false
        Get-ChildItem $regBase -ErrorAction SilentlyContinue | ForEach-Object {
            $props = Get-ItemProperty $_.PSPath -ErrorAction SilentlyContinue
            if ($props.DriverDesc -match 'Realtek PCIe GbE') {
                Set-ItemProperty -Path $_.PSPath -Name 'SpeedDuplex' -Value 6 -Type String -ErrorAction SilentlyContinue
                Write-Output "  [OK] تم تعيين SpeedDuplex=6 عبر Registry في $($_.PSChildName)"
                $found = $true
            }
        }
        if (-not $found) {
            Write-Output "  [ERROR] لم يتم العثور على البطاقة في Registry"
        }
    }
} catch {
    Write-Output "  [ERROR] $($_.Exception.Message)"
}
Write-Output ""

# ----------------------------------------------------------
# 3. إيقاف خدمات ProtonVPN (بأسمائها الصحيحة)
# ----------------------------------------------------------
Write-Output "[3/5] إيقاف خدمات ProtonVPN..."
$services = @('ProtonVPN Service', 'ProtonVPN WireGuard')
foreach ($svc in $services) {
    try {
        $service = Get-Service -Name $svc -ErrorAction SilentlyContinue
        if ($service) {
            Write-Output "  خدمة: $svc - الحالة: $($service.Status) - النوع: $($service.StartType)"
            if ($service.Status -eq 'Running') {
                Write-Output "  إيقاف الخدمة..."
                Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
                Start-Sleep -Seconds 2
                $checkStatus = (Get-Service -Name $svc).Status
                Write-Output "  [OK] الحالة بعد الإيقاف: $checkStatus"
            }
            # تعطيل البدء التلقائي
            Set-Service -Name $svc -StartupType Manual -ErrorAction SilentlyContinue
            Write-Output "  [OK] تم تعيين نوع البدء: Manual"
        } else {
            Write-Output "  [SKIP] الخدمة غير موجودة: $svc"
        }
    } catch {
        Write-Output "  [ERROR] $svc : $($_.Exception.Message)"
    }
}
Write-Output ""

# ----------------------------------------------------------
# 4. إغلاق عمليات ProtonVPN النشطة
# ----------------------------------------------------------
Write-Output "[4/5] إغلاق عمليات ProtonVPN النشطة..."
$procs = Get-Process | Where-Object { $_.ProcessName -match 'ProtonVPN' }
if ($procs) {
    foreach ($p in $procs) {
        try {
            $name = $p.ProcessName
            $id = $p.Id
            $p.Kill()
            Write-Output "  [OK] تم إغلاق: $name (PID: $id)"
        } catch {
            Write-Output "  [ERROR] فشل إغلاق $($p.ProcessName): $($_.Exception.Message)"
        }
    }
} else {
    Write-Output "  [OK] لا توجد عمليات ProtonVPN نشطة"
}
Write-Output ""

# ----------------------------------------------------------
# 5. مسح DNS Cache
# ----------------------------------------------------------
Write-Output "[5/5] مسح DNS Cache..."
ipconfig /flushdns 2>&1 | Out-Null
Clear-DnsClientCache -ErrorAction SilentlyContinue
Write-Output "  [OK] تم مسح DNS Cache"
Write-Output ""

# ----------------------------------------------------------
# ملخص نهائي
# ----------------------------------------------------------
Write-Output "============================================================"
Write-Output "  ملخص الإعدادات بعد الإصلاح"
Write-Output "============================================================"

$dnsNow = Get-DnsClientServerAddress -InterfaceAlias 'Ethernet' -AddressFamily IPv4 -ErrorAction SilentlyContinue
Write-Output "DNS Servers: $($dnsNow.ServerAddresses -join ', ')"

$linkSpeed = (Get-NetAdapter -Name 'Ethernet' -ErrorAction SilentlyContinue).LinkSpeed
Write-Output "Link Speed: $linkSpeed"

$vpnServices = Get-Service | Where-Object { $_.Name -match 'Proton' } | Format-Table Name,Status,StartType -AutoSize | Out-String
Write-Output "ProtonVPN Services:"
Write-Output $vpnServices

$vpnProcs = Get-Process | Where-Object { $_.ProcessName -match 'ProtonVPN' } | Format-Table ProcessName,Id -AutoSize | Out-String
if ($vpnProcs) {
    Write-Output "ProtonVPN Processes:"
    Write-Output $vpnProcs
} else {
    Write-Output "ProtonVPN Processes: لا يوجد"
}

Write-Output ""
Write-Output "============================================================"
Write-Output "  ملاحظة: قد تحتاج لفصل وإعادة توصيل كبل الشبكة"
Write-Output "  أو إعادة تشغيل الكمبيوتر لتطبيق سرعة 1 Gbps"
Write-Output "============================================================"

Stop-Transcript | Out-Null

# إغلاق النافذة تلقائياً بعد 5 ثوان
Start-Sleep -Seconds 5
