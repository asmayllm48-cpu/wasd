# خطة التنفيذ المرحلية

> المخرج 8 (مراحل صغيرة قابلة للاختبار)، والمخرج 9 (الملفات في كل مرحلة)، والمخرج 10 (التعقيد والاعتماديات).
> مقياس التعقيد: S (أيام)، M (أسبوع تقريباً)، L (2–3 أسابيع)، XL (أكثر من ذلك)، لمطور واحد متفرغ. التقدير يُراجع بعد PoC.

## الهيكل المقترح للمستودع
```
wasd/
  README.md  LICENSE  NOTICE  CMakeLists.txt  CMakePresets.json  vcpkg.json
  docs/                      (هذه الوثائق + poc-results/)
  android/
    agent/                   wasd-agent (Java, Gradle) — يُشغَّل عبر app_process
    latency-probe/           تطبيق قياس التأخير (اختبار فقط)
  windows/
    core/                    wasd-core (C++20): device/ session/ video/ input/ config/ diagnostics/ include/wasd_core.h
    gameview/                نافذة اللعب Win32 + D3D11 + طبقة المحرر
    app/                     واجهة Qt 6
  poc/                       تجارب مستقلة صغيرة (تُحذف أو تُجمّد بعد الحسم)
  tests/                     cpp/ (GoogleTest)  android/ (JUnit)  integration/
  tools/analysis/            سكربتات Python لتحليل CSV
  third_party/               إشعارات التراخيص
```

## المرحلة 1: الاستكشاف (الحالية)
| الخطوة | المخرج | التعقيد |
|---|---|---|
| 1.1 | `docs/*.md` الحالية (المتطلبات والقرارات والبنية والخطط) | S (منجز كمسودة) |
| 1.2 | تحديد أجهزة الاختبار الفعلية في `testing.md` | S، ويعتمد على المستخدم |
| 1.3 | مراجعة المستخدم للقرارات المبدئية | — |

## المرحلة 2: إثبات الجدوى
| الخطوة | الملفات | التعقيد | الاعتماديات |
|---|---|---|---|
| 2.0 PoC-0 الأدوات | `CMakeLists.txt`, `CMakePresets.json`, `vcpkg.json`, `android/settings.gradle`, `docs/build-and-release.md` (تحديث) | S | VS2022 وCMake وvcpkg وJDK17 وAndroid SDK |
| 2.1 PoC-1 الاتصال | `poc/adb-track/main.cpp`, `poc/adb-track/adb_client.{h,cpp}`, `docs/poc-results/poc1.md` | S–M | adb على الجهاز وهاتف |
| 2.2 PoC-2 الترميز | `poc/stream-baseline/` (عميل يستقبل من scrcpy-server المثبت الإصدار ويسجل الرؤوس والتوقيت)، `poc/stream-baseline/encoders_probe.md` | M | scrcpy-server بإصدار مثبت (Apache-2.0) |
| 2.3 PoC-3 فك الترميز والعرض | `poc/decode-present/ffmpeg_d3d11va.cpp`, `poc/decode-present/mf_decoder.cpp`, `poc/decode-present/swapchain.cpp`, `poc/decode-present/nv12_to_rgb.hlsl` | L | FFmpeg (LGPL) وWindows SDK |
| 2.4 Latency Probe | `android/latency-probe/` (Activity واحدة، وتغيير لون عند اللمس، وعرض موقع اللمسة) | S | Android SDK |
| 2.5 PoC-4a/b الحقن | `android/agent/` (أول نسخة: handshake وControlReceiver وPointersState وWatchdog وClockSync)، `poc/input-inject/main.cpp` | L | wrappers مشتقة من scrcpy (ترخيص) |
| 2.6 PoC-5 الحلقة الكاملة | `poc/e2e-loop/`, `tools/analysis/latency_report.py`, `docs/poc-results/baseline.md` | M | كاميرا سريعة (اختياري للتحقق الخارجي) |
| 2.7 PoC-6 الواجهة | `poc/ui-overhead/` (Qt shell بسيط مع نافذة اللعب) | S–M | Qt 6 |
| 2.8 تحديث ADRs | `docs/architecture-decisions.md`: تغيير الحالة من "مبدئي" إلى "مقبول"، مع الأرقام | S | نتائج كل التجارب |

**بوابة الخروج:** كل ADR الحرجة (001 إلى 007) "مقبولة" بدليل، وخط أساس منشور.

## المرحلة 3: نموذج أولي قابل للاستخدام (هاتف واحد)
| الخطوة | الملفات | التعقيد |
|---|---|---|
| 3.1 DeviceManager + SessionManager | `windows/core/device/*`, `windows/core/session/*`, `tests/cpp/session_state_tests.cpp` | M |
| 3.2 VideoPipeline من PoC-3 | `windows/core/video/{demuxer,decoder_d3d11va,frame_slot,presenter}.{h,cpp}`, `tests/cpp/demuxer_tests.cpp`, `tests/cpp/frame_slot_tests.cpp` | L |
| 3.3 CoordinateMapper | `windows/core/input/coordinate_mapper.{h,cpp}`, `tests/cpp/coordinate_mapper_tests.cpp` | S–M |
| 3.4 InputEngine أساسي (Tap وHold والعصا والكاميرا) | `windows/core/input/{raw_input,key_state,pointer_allocator,bindings,coalescer}.{h,cpp}`, `tests/cpp/bindings_tests.cpp` | L |
| 3.5 GameView | `windows/gameview/{game_window,cursor_lock}.{h,cpp}` | M |
| 3.6 إعادة الاتصال الآمنة وتحرير الأصابع | تحديث session + agent watchdog، و`tests/integration/reconnect.md` | M |
| 3.7 قياسات داخلية حية | `windows/core/diagnostics/{metrics,histogram,csv_writer}.{h,cpp}` | M |

**بوابة الخروج:** G-01 وG-02 وG-03 وG-06 ناجحة، ولعبة واحدة على الأقل قابلة للعب بسلاسة.

## المرحلة 4: النظام الأساسي
| الخطوة | الملفات | التعقيد |
|---|---|---|
| 4.1 واجهة C للمحرك | `windows/core/include/wasd_core.h`, `windows/core/api.cpp` | M |
| 4.2 Config: المخطط والتحقق والترحيل والنسخ الاحتياطي | `windows/core/config/*`, `schemas/profile.v1.schema.json`, `tests/cpp/config_migration_tests.cpp` | M |
| 4.3 Swipe والمفاتيح المركبة ومناطق اللعبة | `windows/core/input/bindings_*.cpp` | M |
| 4.4 تحسين الفيديو (أوضاع الجودة وطلب IDR والتعافي) | `windows/core/video/*`, `android/agent/video/*` | M–L |
| 4.5 السجلات وحزمة التشخيص | `windows/core/diagnostics/{logger,bundle}.{h,cpp}` | S–M |

## المرحلة 5: الواجهة الاحترافية
| الخطوة | الملفات | التعقيد |
|---|---|---|
| 5.1 نظام التصميم (هوية خاصة: الألوان والخطوط والمكونات) | `windows/app/design/*`, `docs/ui-design.md` | M |
| 5.2 الشاشات: البداية، والرئيسية مع حالة الهاتف، والإعدادات، والمساعدة | `windows/app/screens/*` | L |
| 5.3 محرر المفاتيح (طبقة داخل GameView) | `windows/gameview/editor_overlay.{h,cpp}` | L |
| 5.4 إدارة ملفات الألعاب ومركز التشخيص | `windows/app/screens/{profiles,diagnostics}.*` | M |

## المرحلة 6: الاختبار والتحسين
| الخطوة | المخرج | التعقيد |
|---|---|---|
| 6.1 مصفوفة الأجهزة الكاملة | `docs/poc-results/matrix-*.csv` + تقرير | L، ويعتمد على توفر الأجهزة |
| 6.2 معالجة الاختناقات المقاسة فقط | تعديلات + CSV قبل وبعد | متغير |
| 6.3 تحمل لمدة ساعتين ومراجعة الأمان والتراخيص | تقرير | M |

## المرحلة 7: الإصدار
| الخطوة | الملفات | التعقيد |
|---|---|---|
| 7.1 Installer والتوقيع | `packaging/*` | M |
| 7.2 التحديثات الآمنة | `windows/app/update/*`, manifest موقع | M |
| 7.3 دليل المستخدم واستكشاف الأخطاء | `docs/user-guide.md` | S |

## الاعتماديات الخارجية والمخاطر على الجدول
- **أجهزة حقيقية:** لا يمكن إنهاء المرحلة 2 بدون هاتف Android متصل بكمبيوتر Windows. بيئتي السحابية لا تملك هاتفاً عبر USB، لذلك اختبارات الأجهزة تُنفذ على جهازك.
- **شهادة توقيع:** مطلوبة للمرحلة 7 فقط.
- **قرار ترخيص adb:** قبل المرحلة 7.
