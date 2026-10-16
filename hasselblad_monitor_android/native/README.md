Related files: `iso_packet_size.h`, `test_iso_packet_size.c`, `test_native_load.c`,
`build_libuvc.py`, `../app/build.gradle`, `../app/src/main/jniLibs/arm64-v8a/libuvc.so`,
and `../vendor/UVCCamera/libuvccamera/src/main/jni/libuvc/src/stream.c`.
The app's UvcEngine.kt, PreviewEncoding.kt, GlRenderer.kt, MainActivity.kt, Ui.kt,
AndroidManifest.xml and five PreviewEncodingTest cases were reviewed.

Current update after window50 (supersedes historical deployment text below):
Window50 reattachment reproduced gadget REQBUFS EBUSY and Android's invalid
pthread_join in UVCPreview::stopPreview. Feeder subscribed to CONNECT/DISCONNECT
only in quarantined native-restart mode, so ordinary cable removal retained the
old streaming pool. Reconnect v4 always subscribes, stops/releases on disconnect,
ignores duplicate STREAMON and preserves mappings if STREAMOFF fails. Host and
camera ARM64 syscall-mocked fixtures pass20 cycles and failure ownership checks.
This does not enable the quarantined -R in-stream restart path.
Version2.6 rebuilds the pinned JNI library with explicit created-thread flags;
stop joins preview (the capture-thread creator) before joining capture, and
never joins an uncreated capture thread. Pixel executes300 actual JNI-class
negotiation-failure, stream-start-failure and active-thread stop/reuse cycles.
Android native open/close/destroy now share the USB worker, and open failure
releases the control block/device identity so reattachment can retry. A7-second
no-frame timeout reports failure and cleans up.8 JVM tests, APK build and real
two-cycle Activity teardown (including JNI_OnLoad registration) pass;2.6 installed.
Real camera-to-phone replug recovery is still unverified. Window50 boot changed,
but saved preceding kmsg shows orderly shutdown,userrequested; kpanic_console
is the older window44 crash. Do not attribute this boot change to a new panic.
Evidence: uvc_monitor/artifacts/ss_cache_android_50/RESULT.txt.

Historical update after window48:
Version2.4 produced real Android video with corrected red per user report.
Actual unplug crashed in bundled libusb ISO completion's destroyed transfer lock.
Version2.5 packages local ARM64 libusb100.so from build_libusb.py using the same
pinned source checkout. android_usbfs.c waits for ALL ISO URBs to retire, keeps
error status and maps metadata by original URB position. Actual backend fixture
on Pixel: original baseline fails ownership assertion, fixed passes18 cases.
All98 old libusb_/android_ exports preserved; native JNI/color/SS smoke passed.
Run build_libusb.py with the same --ndk path before Gradle packaging. Existing
libuvc build.json records unchanged link-time dependency; libusb-build.json and
installed25-proof.json record the actual packaged override.
Version2.5 exact APK/native SHA installation proof exists; window50 physical
retest exposed the separate reattachment/start-failure defects above. Atomic
renderer mailbox fixed;8 JVM tests pass. Camera feeder fixes
rotating stale buffers separately; real ghosting improvement is unverified.
Evidence: uvc_monitor/artifacts/ss_color_android_48/RESULT.txt and related logs.

Android SuperSpeed correction — 2026-10-08

The exact AndroidUSBCamera2.2.8 bundled ARM64 libuvc SHA256 is
0f3c36e8b8e3989a36f233955d522eb06d02e08a791e355a17ab640431371424.
Its uvc_stream_start_bandwidth at0x131ec..0x131fc computes the ISO interval budget
from wMaxPacketSize using the USB2 high-bandwidth bits, and does not read SS
companion descriptors. At0x13204..0x13208 it accepts the last alternate setting
even if this budget is smaller than negotiated payload. At0x13458 it stores the
calculated value into each ISO receive packet's length. SS wMaxPacketSize1024
therefore becomes1024 even for a3072-byte interval. This is a verified library
compatibility defect; its role as the sole cause of the phone's black screen
still requires a corrected physical trial.

The exact APK libUVCCamera native do_preview calls uvc_start_streaming_bandwidth
and creates the capture thread. do_capture_callback creates a direct ByteBuffer
and invokes Java; the current Kotlin setup binds/drains an ImageReader surface,
requests NV21 callbacks before startPreview, and the continuous GL renderer accepts
NV21. Window41 logged native preview startup but no Java FPS callbacks; no Java
exception or proof of received frames exists. A preview-start log is not frame proof.

Native source is pinned to saki4510t/UVCCamera commit
c9399e63dfab4b6d260d8cbef92182abc6de0ee0. This is a rebuildable upstream base, not
a claim that it is the exact source used for the original2.2.8 prebuilt. The local
stream.c correction calls iso_packet_size.h. HS/FS calculation stays unchanged;
SS uses validated wBytesPerInterval and rejects missing/malformed companions.
Bundled libusb100 and JPEG libraries stay unchanged and are SHA-checked. This
correction targets ARM64; other ABI libraries retain the old dependency versions.

Build:
python native/build_libuvc.py --ndk C:/Users/tom/AppData/Local/Android/Sdk/ndk/25.1.8937393
Then run Gradle testDebugUnitTest and assembleDebug. Local ARM64 libuvc is selected
by jniLibs pickFirst; verify APK lib/arm64-v8a/libuvc.so equals the local binary.
build/native_ss_uvc/build.json records source revision, native SHA and dependencies.

Verification performed:
- Host native budget tests: HS/FS, SS multiplier/burst, zero/oversized interval,
  reserved multiplier, excessive burst, truncated/missing descriptor, non-ISO.
- Static ARM64 build and all93 UVC symbols required by the unchanged camera JNI
  library present; no original uvc_* export missing.
- Pixel native smoke: bundled libusb's actual companion parser returns3072,
  HS returns3072, frame ABI asserts and YUYV-to-NV21 conversion pass, and the
  unchanged camera JNI library loads with RTLD_NOW against the rebuilt libuvc.
- Gradle five unit cases and APK build pass. APK contains the rebuilt libuvc,
  SHA04beb3fb12497eee0c6f5a720760c7fb25bf0e1cf93198f7d76abef07d20e577.
- UVCMonitor2.2/versionCode3 installed on Pixel5a; CAMERA permission retained.
  APK SHA1005ad9bf62a24d8e15dfc8d6e8818b992a973247dfbf3fecb2c1f724ab56d9f.

No real phone frames with v2.2 have been verified yet. No new USB window is armed.
Camera window41 restored automatically; camera boot/init and shared Frida were
preserved. Phone/host/native evidence is archived under camera blackbox.
No Git commit was made in the main workspace because it is not a Git checkout;
the pinned vendor checkout is only the third-party build source.

Window44 update: Android2.2 logged interval_bytes1024 versus payload3072
on an actual SuperSpeed link. Wrapped libusb's speed is insufficient to select
SS descriptors. Version2.3 parses the endpoint companion first regardless of
reported speed; only NOT_FOUND with speed below SUPER permits USB2 fallback.
Malformed descriptors or parser errors fail closed. Tests now cover UNKNOWN
and HIGH reported speed with valid SS companions using both mocked and real
bundled parsers. Activity receiver now registers on application Context to
match unregister, and is retained only after registration succeeds. The real
two-cycle Activity finish/destroy instrumentation passed on Pixel.
Camera44 nevertheless suffered a gadget request-list NULL dereference in
uvcg_video_pump/V4L2 QBUF and rebooted; this is separate from SSP descriptor
handling. The native-restart trial mode is quarantined in the host launcher.
Android2.3 is installed; corrected actual phone video remains unverified.

Window45 color follow-up: user reports a scene with red rendered blue.
Audited exact JNI maps Java YUV420SP/4 to the VU conversion and NV21/5 to
the UV conversion in our rebuilt libuvc. Version2.4 selects format4 so GL
and JPEG receive actual NV21. This does not rename libuvc exports or reverse
the shader. Native smoke fixture now checks actual V/U bytes and red/blue
patches; real phone execution/installation await PC reconnection. See
uvc_monitor/artifacts/ss_companion_android_45/COLOR.txt for binding evidence.
