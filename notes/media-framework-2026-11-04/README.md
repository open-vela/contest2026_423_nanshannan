# openvela 媒体框架（mediad + FFmpeg + PFW）在 K7 上放音（2026-11-04）

单独的配置片段：`board/kickpi-k7/configs/media-framework.config`（叠在 amp-dual 上）。
板上配置：`board/kickpi-k7/src/etc/media/` → 部署到 `/data/etc/media/`（见那里的 README）。
**不是**默认镜像；镜像 4.3 MB（amp-dual 3.5 MB，FFmpeg 最小集合约 +0.8 MB）。

## 结构

```
mediatool ──PF_LOCAL "md:openvela"──> mediad
                                       ├─ media_player：FFmpeg 解复用 + 解码
                                       ├─ media_policy：PFW（criteria.txt / settings.pfw）
                                       │     流类型 Music → "abufsrc@Music"；MusicVolume → 音量命令
                                       └─ audio_graph：abufsrc@Music0 → adevsink(format=nuttx)
                                                                         └─ /dev/audio/pcm1（ES8388 + SAI1）
音量持久化：PFW persist.* → KVDB（DIRECT + FILE）→ /data/kvdb/
```

## 结果（板上，mediatool）

| 内容 | 结果 |
|---|---|
| WAV 48 kHz s16 立体声 3.2 s / 20 s | PREPARED → STARTED → COMPLETED，时长 3200 / 20000 ms |
| MP3 48 kHz 64 kbps 19.6 s（`request_sample_fmt=s16`） | 放完，时长 19584 ms |
| WAV 44.1 kHz | 放完；pcm1 按 44100 打开 |
| 播放速度 | position 每 10 s 前进 9.73 s（WAV）/ 9.77 s（MP3），即实时 |
| 音量 | `setint MusicVolume 9 1` → domain 切到 index-9；重启后读回的是上次设的值（KVDB） |
| 一首放完后 | mediad 仍在，可以接着 open/prepare 下一首 |

**听感已确认**（人在板旁听）：WAV 哔声、MP3、44.1 kHz 音调都有声音，音调正常；
播放中 `setint MusicVolume` 6→2→10 的音量变化听得出来。

## 跑通前修掉的问题（按出现顺序）

1. **KVDB 没开**：`media_policy.c` 无条件调 `property_get/set_int32` → 链接失败。
   开 KVDB DIRECT + FILE。
2. **`pthread_mutexattr_settype` 返回 ENOSYS**（没开 `PTHREAD_MUTEX_TYPES`）：FFmpeg 在
   `assert_level=3` 下直接 `abort()`，mediad 一加载 graph 就退出。日志只剩半行
   `...type failed with error: Unknown error 38`。
   ★ 这个选项改变 `pthread_mutex_t` 的布局，apps 侧对象不会跟着重编 → 必须全量编。
3. **没有本地 socket**：mediad 只在 PF_LOCAL / RPMSG / INET 上 listen，三者都失败
   → `media_server init failed: -22`。开 `NET_LOCAL`。
4. **客户端去连 RPMSG**：只有 `MEDIA_SERVER_CPUNAME` 等于 `RPMSG_LOCAL_CPUNAME` 时
   才走 PF_LOCAL；默认 `"local"` ≠ K7 的 `"openvela"`。设成 `"openvela"`。
5. **mediatool 栈**：16 KB 在 64 位上实测用到 14.5 KB；同一轮崩过一次（FAR=0x9）。
   改成 32 KB 后没再复现，但没有确证就是溢出。
6. **graph 里 `abufsrc` 不写 `map=` → start 时崩**：`abufsrc_proccess_command` 的 link
   分支读 `priv->map[i]` 不判空（unlink 分支判了）。graph 里写 `outputs=1:map=1`。
7. **一首放完 mediad 就 abort**：`av_assert1(!li->status_in)`（avfilter.c）。audio_graph
   故意把空闲 link 置 EOF，adevsink 不看状态就请求帧。vela 的 FFmpeg Makefile 写死
   `assert_level=3`；片段里用 `--assert-level=0` 覆盖（上游 FFmpeg 的默认值）。
   ★ 改 configure 参数要删 `external/ffmpeg/ffmpeg/ffbuild/config.mak`，否则不会重新 configure。
8. **MP3 卡在 168 ms**：解码器出平面 `s16p`，adevsink（pcm_s16le）只收 `s16`，graph 又
   不自动插转换。prepare 时带 `request_sample_fmt=s16`（mp3 定点解码器支持直接出交错）。

## 调试时踩的坑

- **telnet 的 `\r` 进了参数**：tsess 默认发 `\r\n`，mediatool 只去掉 `\n`，最后一个参数
  变成 `Music\r` / `MusicVolume\r`。表现非常迷惑：`setint MusicVolume 3 1` 能用（最后一个
  参数是数字，`atoi` 不在乎），`getint MusicVolume`、`open Music` 都是 -22。
  测试脚本里命令一律只用 `\n`。
- **kill -9 mediad 之后起不来新的**：`/var/run/md:openvela*` 节点和 telnet 会话都会残留，
  删了节点 listen 还是 -22。每轮测试先 reboot（`media-session.sh`）。
- **没 close 的播放器占着 `Music0`**：客户端退出不会回收，下一次 open 返回 -22。
- **一首已经 COMPLETED 之后再 `stop 0`，mediatool 卡住空转**（ps 里 Running），之后的命令
  都不处理，要 reboot。放完直接 `close` 没问题。原因没查。
- **mediatool 的 position 回得很慢**（约 10 s 一次），拿它的回报时刻当采样时刻，会算出
  "快了 1.7 倍"的假结论。比速度要用相邻两次回报的差。
- **串口丢字**：多核同时打印，`ELR_EL1` 那一行被吃过两次；靠复现拿到完整的一行。
- 有一次 OTA 后首次启动约 16 s 内板子自己复位了一次（镜像 17:22:31），原因没查到，
  之后没再出现。

## 没做的 / 留下的

- 只有一路 `Music0`；多路混音（amix 在这个版本没有 vela 的 timeout 选项，空闲输入会不会
  卡住没试）、Ring/Alarm 等流类型、录音（adevsrc 与 adevsink 共用 pcm1 的冲突）都没做。
- `request_sample_fmt=s16` 要调用方自己带；更好的做法是框架里默认，需要改上游
  （media_player.c 或 graph 协商），还没做。
- 上面第 6、7 条是上游缺陷，没有提补丁；目前靠配置绕开。
- 片段只验证到 mediatool；UI 接入（音乐页 / 音量键）没做。
