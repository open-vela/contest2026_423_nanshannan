# 相机页：横纹与帧率（2026-11-02）

## 现象

1. 摄像头一动，画面上出现约 5 条横纹，位置固定、间隔固定，"像画面没跟上"。
2. 横纹修掉后，用户反馈"画面挺流畅，但是 fps 有点低"。

Linux 侧 k7cam 稳定出 30fps，帧共享内存里 4 个槽位，不丢帧。所以瓶颈在 openvela
的显示路径上。

## 原因与改法

| 版本 | 改了什么 | 3 秒新帧 | 每帧 |
|---|---|---|---|
| v0 | PARTIAL 单缓冲：分 4 块（202 行一块）直接写进 VOP 正在扫的那块 | — | 有横纹 |
| v1 | 相机页里每次标脏都扩成整屏，画进后缓冲再翻页 | 45（刷新 342 次） | 小块更新全变整屏重画，见 `serial-run1.raw` |
| v2 | 只有本来就是整屏的那一轮刷新才写后缓冲、翻页（`camstat-v2.txt`） | 65~66 ≈ 22fps | 42ms，全在界面线程里串行 |
| v3 | flush 交给 `ui_flush` 线程，画和拷重叠（`run3/camstat-v3.txt`） | 78 ≈ 26fps | 拷贝线程：等 latch 10 + 拷 20 + 刷 cache 翻页 6 |
| v4 | 帧缓冲驱动改成三块轮转（`run3/camstat-v4.txt`） | **84~88 ≈ 29fps** | 等 latch 0，拷贝线程 26ms，比相机帧间隔 33ms 短 |

- **横纹（v0）**：一帧画完要几十毫秒，跨了好几次扫描，扫描线撞上分块的交界。
- **v2 的 42ms**：LVGL 画约 13ms，转 90° 拷进帧缓冲约 18ms，刷 cache 和翻页约 6ms，
  再加 5ms 等翻页 latch，全部串在界面线程里。
- **v3**：两块绘制缓冲本来就有。`flush_cb` 只把这一块交给拷贝线程，LVGL 马上去画
  下一块。LVGL 每次进 `flush_cb` 前都会调 `flush_wait_cb`，这里用信号量等上一块
  拷完，不空转。
- **v4**：两块帧缓冲时，刚换下来的那块要等 VOP 在下一个 VSYNC 真正切走才能写（驱动
  的 FS 标志靠不住，用的是保守的 17ms）。三块轮着写时，要写的那块是两次翻页之前
  换下来的，早就没人扫了。代价是多 3.6MB 内存：Umem 空闲从 41.0MB 降到 37.3MB。

## 改动

- `app/kickpi_ui/kickpi_ui_main.c`：
  - `kickpi_flush_cb` 只定写哪块、投递任务。
  - `kickpi_flushd` 负责旋转拷贝、FBIO_UPDATE 和翻页。
  - `kickpi_flush_wait` 用信号量等待。
  - 选后缓冲的逻辑在 `kickpi_fb_backbuf`。
  - 统计每 3 秒写一次 `/tmp/k7-camstat`。
- `chip/rk3576/rk3576_fb.c`：`FB_NBUF 3`，`yres_virtual = 3*yres`，updatearea 和
  pandisplay 改为按 `y / yres` 选块。
- `app/k7diag/k7diag_main.c`：判断双缓冲改成 `yres_virtual >= 2*yres`。

## 怎么复现测量

```
python3 scripts/xts-netsh.py 'echo 1 > /tmp/k7-open-camera' 5
python3 scripts/xts-netsh.py 'cat /tmp/k7-camstat' 5
```

## 还没做

- 目视确认三块缓冲版本没有横纹、没有回跳（等用户看）。
- 拷贝线程占 26ms/帧，主要是 20ms 的旋转拷贝。要到 60fps 得把拷贝拆给两个线程，或者
  看 VOP 能不能自己转（没查过）。相机只出 30fps，目前不需要。
