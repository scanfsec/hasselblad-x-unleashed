# x2nes — X2D II 100C 上的 NES 模拟器（超级马里奥）

状态（2026-09-22 晚）：**三层入口全部真机跑通**
1. **原生 GUI 左滑菜单 GAME 格**（qrc 全量镜像 + MainScreen.qml 第 12 格注入，
   点击 → hook spawn x2nes）——最终形态
2. x2nes 内置 ROM 菜单（无 GUI 时直接跑）
3. x2nes 游戏内虚拟手柄（wl_touch，45fps）