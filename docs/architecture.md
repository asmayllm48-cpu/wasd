# البنية المعمارية

> المخرج 5: الوحدات، ومخطط تدفق الفيديو والتحكم، ودورة حياة الجلسة.

## 1. الوحدات

```
┌──────────────────────── Windows ────────────────────────┐        ┌──────────── Android (shell uid) ────────────┐
│ UI Shell (Qt 6) ── C API / events ──► wasd-core (C++20) │        │ wasd-agent (app_process)                    │
│                                       ├ DeviceManager   │◄─adb──►│ ├ Session/Handshake                         │
│                                       ├ SessionManager  │ track- │ ├ VideoStreamer: Display→Surface→MediaCodec │
│                                       ├ VideoPipeline   │ devices│ ├ ControlReceiver → TouchInjector           │
│                                       ├ InputEngine     │        │ │    (multi-pointer state + watchdog)       │
│                                       ├ GameView (Win32+D3D11)   │ └ ClockSync / Metrics                       │
│                                       ├ Config          │        └─────────────────────────────────────────────┘
│                                       └ Diagnostics     │
└─────────────────────────────────────────────────────────┘
```

| الوحدة | المسؤولية | ما تملكه | الواجهة |
|---|---|---|---|
| **DeviceManager** | اكتشاف الأجهزة (`host:track-devices`)، والحالات (device/unauthorized/offline)، وفحص التوافق (API والمرمّزات والشركة) | اتصال adb host | أحداث `DeviceAdded/Removed/StateChanged` |
| **SessionManager** | آلة حالات الجلسة، ومعرف جلسة فريد (`scid`)، ومنع الجلسات المكررة، وإلغاء العمليات القديمة | sockets الجلسة وعملية agent | `Start(deviceId, profile)`, `Stop()`, أحداث الحالة |
| **VideoPipeline** | استقبال الحزم، والتحليل (demux)، وفك الترميز، وطابور الإطار الأحدث، والعرض، وقياس المراحل | decoder وD3D11 device وswap chain | `SetMode()`, إحصاءات |
| **InputEngine** | Raw Input، وتحويل الإحداثيات، وتنفيذ ملف التعيين، وإدارة الأصابع، وتحرير اللمسات العالقة | حالة المفاتيح والأصابع | `LoadKeymap()`, `Capture(on/off)` |
| **GameView** | نافذة اللعب على خيطها الخاص، وطبقة المحرر، وقفل المؤشر | HWND وخيط الرسائل | داخلية |
| **Config** | ملفات الألعاب، والتحقق، والترحيل، والاستيراد والتصدير، والنسخ الاحتياطي | ملفات `%APPDATA%\WASD\` | `Load/Save/Validate/Migrate` |
| **Diagnostics** | سجلات منظمة، وعدادات، ومدرّجات تكرار (histograms) للتأخير، وتصدير CSV وحزمة تشخيص | ring buffers | `Snapshot()`, `Export()` |

**قواعد الترابط:**
- الوحدات تتواصل عبر واجهات ورسائل (أحداث)، وليس عبر استدعاء تفاصيل بعضها.
- الواجهة لا تعرف شيئاً عن D3D أو adb، فقط عن واجهة C الخاصة بـ core.

## 2. نموذج الخيوط (Threads)

| الخيط | العمل | ملاحظات |
|---|---|---|
| UI (Qt) | الواجهة فقط | لا قرص ولا شبكة ثقيلة |
| GameView/Input | message loop لنافذة اللعب و`WM_INPUT`، وتحويل الإدخال وإرساله إلى socket التحكم (كتابة غير حاجبة) | أعلى أولوية، وبدون أقفال طويلة |
| Video RX + Decode | قراءة socket الفيديو، والتحليل، وإرسال الحزم إلى decoder، ووضع الإطار في "latest slot" | خيط واحد يكفي مبدئياً. يُقسَّم فقط إن أثبت القياس اختناقاً |
| Render/Present | ينتظر waitable object، ويأخذ أحدث إطار، ثم يرسم ويستدعي Present | قد يُدمج مع خيط GameView بعد القياس (PoC-3) |
| Device/Session | أحداث adb ودورة الحياة | حاجب على I/O فقط |
| Diagnostics writer | تفريغ السجلات وCSV إلى القرص | دفعات وليس لكل حدث |

**الإيقاف:** كل خيط يملك `stop_token` (C++20 `std::jthread`). إغلاق sockets يفك حجب القراءة. الترتيب: الإدخال ثم الفيديو ثم الجلسة ثم الجهاز.

## 3. تدفق الفيديو

```
Android                                                    Windows
Display ─► VirtualDisplay/Surface ─► MediaCodec(H.264)     socket RX ─► demux ─► decoder (D3D11VA)
   t_cap (≈ pts)            t_enc_out ─► header{pts,flags,  t_rx            t_dec_in   t_dec_out
                                         t_enc_out,size}                                 │
                              ─► localabstract socket ═USB═►                              ▼
                                                                         [latest-frame slot, cap=1]
                                                                          (overwrite = count drop)
                                                                                         │
                                                       waitable swapchain ◄─ render NV12→RGB + scale
                                                                 t_present ─► DWM/scanout ─► photon
```

- **طابور الحزم المضغوطة:** محدود السعة. لا تُسقط حزمة مضغوطة قبل decoder، لأن ذلك يكسر سلسلة الإطارات المرجعية. إذا امتلأ الطابور يُطلب IDR ويُعاد التزامن.
- **الإسقاط:** يحدث فقط في latest-frame slot بعد فك الترميز (إطار مفكوك قديم يُستبدل بأحدث). يُحسب كل إسقاط في الإحصاءات. هذا هو "الفصل بين إسقاط المعاينة الحية والمسار الذي يحتاج التسلسل" المطلوب في البرومبت.
- **التسجيل المستقبلي (إن أُضيف):** يأخذ الحزم المضغوطة قبل decoder، فلا يتأثر بالإسقاط.
- **تغيير الاتجاه أو الدقة:** الـ agent يعيد تشغيل المرمّز ويرسل رأساً جديداً بالأبعاد. VideoPipeline يعيد تهيئة decoder والسطوح، وInputEngine يحدّث التحويل.

## 4. تدفق التحكم

```
WM_INPUT / WM_KEY*  ─► InputEngine
   t_in (QPC)           ├ KeyState (scan code → binding)
                        ├ Binding executors: Tap | Hold | Joystick(WASD) | MouseCamera | Swipe
                        ├ PointerAllocator (ids 0..N-1, N ≤ device max)
                        ├ CoordinateMapper (window → video → device)
                        └ Coalescer (يدمج الحركات المتتالية لنفس الإصبع قبل الإرسال)
                                 │ TouchBatch{seq, t_send, [ {id, action, x, y, pressure} ... ]}
                                 ▼
                         control socket ═USB═► wasd-agent ControlReceiver
                                                  t_agent_rx
                                                  ├ PointersState (Android MotionEvent multi-pointer)
                                                  ├ inject (ACTION_DOWN / POINTER_DOWN / MOVE / POINTER_UP / UP)
                                                  │  t_injected
                                                  └ Watchdog: socket closed ⇒ release all (ACTION_CANCEL/UP)
```

**قواعد الصحة:**
- لكل إصبع حالة صريحة: `Idle`، ثم `Down`، ثم `Moving`، ثم `Up`. لا `MOVE` قبل `DOWN`، ولا `DOWN` مكرر.
- `seq` متزايد في كل دفعة. الـ agent يتجاهل المكرر وغير المرتب، فلا يتكرر أمر عند إعادة المحاولة.
- **الدمج (Coalescing):** الحركة فقط. أحداث `DOWN` و`UP` لا تُدمج ولا تُسقط أبداً.

### 4.1 تحويل الإحداثيات (CoordinateMapper)
أربع مساحات، وطبقة تحويل واحدة مختبرة:
1. **Window client:** بكسلات نافذة Windows، مع مراعاة DPI عبر Per-Monitor DPI Awareness v2.
2. **Viewport:** مستطيل الصورة داخل النافذة بعد letterbox أو pillarbox بنسبة الأبعاد.
3. **Video frame:** أبعاد الإطار المرمّز، وقد تكون مصغّرة عبر `max_size`.
4. **Device display:** الأبعاد الفعلية في الاتجاه الحالي.

بالإضافة إلى **Game area:** مستطيل فرعي اختياري إذا كانت اللعبة لا تملأ الشاشة (notch أو أشرطة).

- الملف يخزن نقاطاً مطبّعة (0..1) في مساحة Device display.
- يُرسل للهاتف `(x, y, screenW, screenH)`، والـ agent يتحقق من تطابق الأبعاد، ويرفض الحدث إذا تغيّر الاتجاه ولم يُحدَّث العميل بعد. نفس مبدأ scrcpy.

### 4.2 الكاميرا بالماوس
- عند القفل: إصبع مخصص يضغط في نقطة ارتكاز (anchor) داخل منطقة الكاميرا.
- كل إزاحة Raw Input تُضرب في `sensitivity` (منفصلة لـ X وY) وتُراكم كقيمة عشرية، ثم يُرسل `MOVE`.
- إذا ابتعد الإصبع عن الارتكاز أكثر من `recenterRadius`، أو اقترب من حافة المنطقة: يُرسل `UP` ثم `DOWN` في الارتكاز، ويُكمل الباقي. يُختبر أثر ذلك على اللعبة في PoC-4.
- لا يعتمد على FPS: الحركة تُرسل عند وصول الإدخال (مع دمج اختياري بحد زمني صغير قابل للضبط ومقاس)، وليس عند كل إطار.

### 4.3 العصا (WASD)
- إصبع يضغط مركز العصا عند أول مفتاح.
- اتجاه المتجه = مجموع المفاتيح المضغوطة (W وA وS وD) بعد التطبيع. الموضع = المركز + الاتجاه × نصف القطر.
- عند رفع كل المفاتيح يُرسل `UP`. ضغط اتجاهين متعاكسين معاً يعطي متجهاً صفرياً (سلوك محدد ومختبر).

## 5. دورة حياة الجلسة

```
Detected ─► CompatChecked ─► Authorized ─► AgentPushed ─► AgentStarted ─► Handshake(version, scid, caps)
   ─► VideoReady ─► ControlReady ─► Running ─┬─► Degraded (latency/frames anomalies; still running)
                                            ├─► Disconnected ─► (auto-retry bounded, same scid cleanup) ─► Detected
                                            └─► Stopping ─► ReleasedAll ─► Stopped
```
- **عند كل انتقال للحالة:** حدث في Diagnostics، وتحديث للواجهة.
- **قبل AgentStarted:** قتل أي agent سابق بنفس الاسم أو scid على الهاتف، وإزالة `adb reverse` القديم.
- **مراقبة الصحة بالأحداث:**
  - إغلاق socket أو خطأ قراءة يعني انقطاعاً.
  - حدث فصل الجهاز من `track-devices`.
  - رسالة keep-alive خفيفة على socket القياس فقط عند غياب حركة، لاكتشاف التجمد الصامت. هي محدودة التواتر وموثقة كاستثناء من قاعدة عدم الـ Polling.
- **تحديث مؤشرات الواجهة:** مرة إلى 4 مرات في الثانية من snapshot جاهز، لكي لا تؤثر المؤشرات على الأداء.
