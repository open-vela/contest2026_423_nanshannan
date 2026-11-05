# /data/etc/media（mediad 的配置）

K7 没有 ROMFS /etc，这三份文件部署到 eMMC 数据分区 `/data/etc/media/`，
路径由 `CONFIG_MEDIA_SERVER_CONFIG_PATH` 指定（`configs/media-framework.config`）。
另需 `mkdir /data/kvdb`（音量持久化，KVDB 文件后端不会自己建目录）。

| 文件 | 作用 |
|---|---|
| `graph.conf` | FFmpeg filtergraph：`abufsrc@Music0` → `adevsink` → `/dev/audio/pcm1` |
| `criteria.txt` | PFW 判据：流类型 `Music` → graph 节点前缀 `abufsrc@Music`；音量 0–10 |
| `settings.pfw` | 音量档位 → `Music,volume,<增益>`（abufsrc 自带音量） |

注意：

- 设备是 `pcm1`（裸 ES8388）而不是 `pcm0`：`pcm0` 套了 `pcm_decode`，只吃带 WAV 头的流。
- `abufsrc` 必须写 `map=`：不写时 `priv->map` 为 NULL，`link` 命令（每次 start）解引用就崩
  （`asrc_abufsrc.c` 的 unlink 分支判了空，link 分支没判）。
- graph 不做自动格式转换（`AVFILTER_AUTO_CONVERT_NONE`），而 adevsink 只收交错 `s16`。
  MP3 解码器默认出平面 `s16p`，所以放 MP3 时 prepare 要带 `request_sample_fmt=s16`。
  采样率跟随片源（44.1 kHz 的文件会按 44100 打开 pcm1），graph 里写的 48000 不起作用。
- 现在只有一个 `Music0` 节点：同一时刻只能有一个 Music 播放器，没 close 的播放器会一直占着它。
