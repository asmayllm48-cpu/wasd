# البناء والإصدار

> يُكمّل بعد PoC-0. القيم هنا خطة وليست أوامر مختبرة بعد.

## المتطلبات (تُثبَّت إصداراتها في PoC-0)
- Windows 10/11 x64، وVisual Studio 2022 (MSVC، C++20)، وCMake، وNinja، وvcpkg (baseline مثبت).
- Qt 6 (إصدار LTS)، وFFmpeg عبر vcpkg بميزات LGPL فقط.
- JDK 17، وAndroid SDK (build-tools وplatform مستهدف)، وGradle wrapper.
- Python 3 (pandas وmatplotlib) لتحليل نتائج الأداء.

## البناء (مستهدف)
```
cmake --preset windows-release
cmake --build --preset windows-release
ctest --preset windows-release
cd android && ./gradlew :agent:assembleRelease :latency-probe:assembleDebug
```

## الإصدار (المرحلة 7)
- **Installer:** MSIX أو WiX (MSI)، والقرار في المرحلة 7 حسب متطلبات التحديث والتوقيع.
- **التوقيع:** Authenticode عند توفر شهادة، مع توقيع الـ installer والملفات التنفيذية.
- **محتويات الحزمة:**
  - التبعيات المطلوبة فقط: Qt DLLs عبر windeployqt، وFFmpeg DLLs، وagent.jar، وadb حسب قرار الترخيص.
  - ملفات التراخيص وNOTICE.
- **التحديثات:** ملف manifest موقّع يحدد الإصدار وhash، والتحقق قبل التثبيت، والرجوع للإصدار السابق عند الفشل.
- **تقارير الأعطال:** minidump محلي، ويُرسل فقط بموافقة المستخدم.
- **اختبارات الإصدار:** تثبيت نظيف، وترقية من الإصدار السابق مع بقاء الإعدادات، وإزالة نظيفة، والتشغيل كمستخدم عادي.
