# WASD Project (اسم مؤقت)

تطبيق Windows للتحكم في ألعاب Android تعمل على هاتف حقيقي، باستخدام لوحة المفاتيح والماوس، مع عرض شاشة الهاتف بزمن تأخير منخفض وقابل للقياس.

> الحالة الحالية: **نموذج أولي قابل للاستخدام (Windows) + محرك أتمتة كامل**. خط الرسم بين التحليل والبناء على جهاز فعلي: انظر [docs/automation.md](docs/automation.md) للتقرير النهائي لخصائص الأتمتة وما تبقى من قياسات ميدانية.

## الوثائق

| الملف | المحتوى |
|---|---|
| [docs/requirements.md](docs/requirements.md) | تحليل المتطلبات والافتراضات، وأصعب خمس مشكلات ومخاطرها |
| [docs/architecture-decisions.md](docs/architecture-decisions.md) | مقارنة الخيارات التقنية ومصفوفة القرار والتوصية والبدائل المرفوضة |
| [docs/architecture.md](docs/architecture.md) | الوحدات ومسؤولياتها ومخطط تدفق الفيديو والتحكم ودورة حياة الجلسة |
| [docs/performance-plan.md](docs/performance-plan.md) | تجارب إثبات الجدوى ومنهجية القياس ومعايير قبول الأداء |
| [docs/testing.md](docs/testing.md) | خطة الاختبارات الوظيفية والآلية والأداء ومصفوفة الأجهزة |
| [docs/security.md](docs/security.md) | الأمان والخصوصية والتراخيص وحدود مكافحة الغش |
| [docs/implementation-plan.md](docs/implementation-plan.md) | المراحل الصغيرة والملفات المنشأة في كل مرحلة والتعقيد والاعتماديات |
| [docs/build-and-release.md](docs/build-and-release.md) | البناء والتوزيع والتوقيع والتحديثات |
| [docs/automation.md](docs/automation.md) | دليل الأتمتة المتقدمة: اكتشاف الألوان، والمناطق الدائرية، والمفاتيح المتقدمة، والمنطق الشرطي |

## الفكرة باختصار

```
[Windows PC]                                   [Android phone - USB]
 UI Shell  ── commands ──►  Engine (C++) ◄══ adb tunnel ══►  Agent (app_process, shell uid)
                            ├─ Video: decode D3D11 → present        ├─ capture display → MediaCodec
                            ├─ Input: Raw Input → keymap → touch ═► ├─ inject MotionEvent (multi-touch)
                            └─ Diagnostics / Config / Device Mgr    └─ timestamps for latency
```
