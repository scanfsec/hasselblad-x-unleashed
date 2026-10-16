Eagle2 ab316 temporary UVC SSP guard — 2026-10-08

Temporary ISO recovery (separate experimental module)
  Actual Android window 35 negotiated ISO video, then four -EXDEV (-18)
  completions caused the exact vendor gadget callback to cancel its video queue.
  uvc_iso_recover.c resolves the gadget callback from an exported address anchor;
  the kernel also contains a different host-driver callback with the same name.
  Exact instruction signatures and request offsets are checked before hooking.
  Only an ISO IN request with the matching callback/context and loaded -18
  status is handled. The loaded status register changes to zero, allowing the
  ordinary callback to recycle requests. Request status, payload and ownership
  stay unchanged. Missing packets are not claimed to have been delivered.
  Other errors, bulk requests and unrelated callbacks keep their usual behavior.

  Build: sh kernel_runtime/build_iso.sh. Nine C policy assertions pass.
  Camera private fixtures call the actual completion function with success and
  -18; the queue remains intact, request ownership/status are preserved, and
  disabled/armed load/unload pass. The fixture does not use the real USB endpoint.
  Proof: artifacts/iso_guard_check/result.json, copied to blackbox.
  Module SHA256: 2cfac2e935b145b7a9672d29bad3b3fd683e8b54eeef06c96689952ff2d2a238.
  Native selftests prove callback recovery, not phone video delivery.
  Window 38 subsequently verified the real PC ISO USB path: 60 captured
  640x480 frames with 21 different hashes (including the initial green buffer),
  a later real scene PNG and eight actual
  recoveries. Timed 30-second window restored factory USB with no reboot;
  both guards unloaded. This short capture does not prove phone video or
  sustained unique 30fps. Evidence: artifacts/iso_pc_38/RESULT.txt.

  Opt in with run_guarded_phone_trial.py --transport iso --iso-recovery
  --wait-for-handoff. uvc_iso_trial.sh wraps the existing SSP/SBPC supervisor.
  Both guards load before handoff; the 90-second window starts after physical
  disconnect/reconfiguration. Recovery count and kernel snapshots are saved
  under the same /blackbox/uvc_monitor trial folder. Normal restore unlinks UVC
  before unloading owned guards; a failed restore retains the required guard.
  No boot/init files are changed. Current Python regression total: 99.

Scope
  Current guard v2 additionally covers the pure-UVC SSP configuration-count gap.
  Exact vendor usb_add_function +0xe8 signature is verified before registering
  the hook. At that site x0 receives the existing SS array only for the verified
  UVC function, missing SSP array, SS capability and maximum SSP speed. Native
  code sets config.superspeed_plus; no descriptor field is aliased or owned twice.
  Private count_configs fixtures run before and after arming. Eighteen host C
  assertions pass. Deployed module: /blackbox/uvc_monitor/uvc_ssp_guard_v2.ko,
  SHA256 86badf6efa5e640bd4342fcb97462bda0705561fe4e709407b6926fecf5921d0.
  Window 30: real PC 5s bind/restore pass. Window 31: actual iPhone SSP configured,
  both fallback counters 1, no video PROBE/COMMIT. Native proof and logs are in
  artifacts/ssp_config_guard_check and artifacts/pure_phone_config_31/device.
  Current shell suite is 54 tests, launcher 18, resident 10. Older counts and
  deployment instructions below describe the first endpoint-only guard.

  A runtime-only workaround for the observed NULL SSP descriptor cursor.
  It changes the current x4 register cursor to the existing SS array only
  for the exact UVC function at USB_SPEED_SUPER_PLUS, companion path active,
  SSP array absent and SS array present. Existing SSP, other functions and
  lower speeds retain their original behavior. Descriptor fields/ownership,
  gadget speed and boot/init files are not changed. Reboot removes the module.

Build evidence
  system_decrypted/lib/modules/kheaders.ko SHA256 matches the camera:
  b7cbbd598da206e828b96d8103650f790ef7508bd0e1835ab3485c04aad9621b.
  Its embedded headers report 5.4.123-rt59-g5b4774899a19-ab316. They are
  extracted offline, without loading kheaders on the device. CONFIG_MODVERSIONS
  is disabled. Compiler assertions match usb_function SS=32, SSP=40 and
  usb_gadget speed=88. struct module is 704 bytes, matching the original module.
  ARM64 PLT placeholder sections follow Linux 5.4 arch/arm64/kernel/module.lds.
  No existing .ko is patched or used as a donor. All imports are exported by
  this kernel; normal insmod/rmmod accepted the final generated module.

Validation
  Nine host policy checks pass. Disabled registration resolves the live
  kernel symbol and validates exact fault-site and SSP-load instructions plus
  the UVC set_alt prologue before arming. A mismatch refuses activation.
  Disabled load/unload passed on the camera. First packaging attempt failed
  with missing PLT sections before code execution; this was corrected.
  Armed load/unload passed with real config_ep_by_speed_and_alt calls on
  private fixtures for both bulk and interrupt endpoints. A valid-SSP
  baseline proves the probe executes before attempting the missing-SSP case.
  Tests check endpoint descriptors/maxpacket/burst and unchanged ownership.
  The real UDC is not involved in these parser tests. Camera PID, boot ID and
  factory USB remained unchanged. Native proof: artifacts/ssp_guard_check.

Usage
  Build in WSL: sh kernel_runtime/build.sh from uvc_monitor.
  Deploy .ko to /blackbox/uvc_monitor/uvc_ssp_guard.ko, and runtime scripts
  to /data/local/tmp. The new entry point is uvc_ssp_trial.sh, which detaches
  its supervisor, loads/validates the guard BEFORE invoking SBPC USB setup,
  and unloads AFTER the UVC link has been removed by timed restore. Failed
  restore retains the guard and returns failure instead of exposing UVC.
  Reused armed modules are preserved. Disabled modules refuse the trial.
  Seven guard lifecycle tests and eleven child/supervisor tests pass.
  Older unguarded scripts do not protect an SSP reconnect; use the guarded
  entry point for any new phone experiment. Do not rerun the full factory
  plus UVC combination, which lacks hardware endpoints.

Hardware qualification and remaining phone limitation
  Window 22 passed a 70-second PC video observation and timed factory
  restore with exact SBPC source, unchanged boot ID/PID/boot-file hashes,
  and normal guard unload. See artifacts/guarded_pc_22/RESULT.txt.
  Final user requirement is now iPhone 17 Pro receiving standard UVC.
  PC success does not meet that requirement. Window 23 is testing a pure
  UVC iso profile to exclude unrelated composite interfaces and bulk mode.
  Its PC startup capture is a GREEN INITIAL BUFFER, not a scene proof;
  no phone video claim may be inferred from successful DirectShow opening.
  Test 20 subsequently connected the actual iPhone at SSP while guarded
  UVC was active. Two live fallbacks were counted; there was no reboot and
  factory restore/unload passed. Actual phone UVC video is not verified:
  no PROBE/COMMIT arrived and SBPC preflight had failed before dispatch.
  Use run_guarded_phone_trial.py to enforce preflight success before setup.
  Window 21 also survived actual SSP with two fallbacks and a qualified
  publisher (3117 SBPC copies, zero errors), but no host video request.
  The user's self-built iPhone app reported no external UVC capture device.
  Eleven launcher checks include canonical Frida deployment symlinks and
  pure-UVC/iso dispatch through the same qualified/guarded entry point.
  Guarded 90s window 19 restored factory USB and unloaded the module correctly.
  The phone connected about 43 seconds after UVC removal; fallback count was
  zero. That window did not exercise guarded phone UVC enumeration/video.
  Kernel parser selftests alone do not prove host protocol or app support.
  The earlier PC bulk test 18 delivered real SBPC video but was interrupted by
  an unguarded phone reconnect and kernel panic. The camera source is exact
  liveview_after_sbpc, not a substitute stream.

Primary references
  https://raw.githubusercontent.com/torvalds/linux/v5.4/include/linux/kprobes.h
  https://raw.githubusercontent.com/torvalds/linux/v5.4/arch/arm64/kernel/module.lds
  https://raw.githubusercontent.com/torvalds/linux/v5.4/drivers/usb/gadget/composite.c
