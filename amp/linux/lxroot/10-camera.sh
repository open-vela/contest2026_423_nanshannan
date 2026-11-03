#!/bin/sh
# lxroot 钩子：装摄像头驱动，等 IMX415 绑定上。由 /init 在后台调用（限时 60 秒），
# 输出进 dmesg（/init 把 stdout 接到了 /dev/kmsg）。
#
# ★ 模组的供电（GPIO0_C7）和 MCLK 是 openvela 给的（CONFIG_KICKPI_K7_CAMERA_LINUX）。
#   两边几乎同时启动，Linux 这边走到这里时 openvela 不一定已经上完电，
#   IMX415 探测读不到型号就会放弃绑定 —— 所以绑定失败要隔一秒重试，
#   而不是当成"没有摄像头"。

M=/lib/modules/$(uname -r)
log() { echo "k7 camera: $*"; }

while read -r m; do
	insmod "$M/$m" || log "insmod $m 失败"
done < "$M/load.order"

i=0
while [ ! -e /sys/bus/i2c/drivers/imx415/8-0037 ]; do
	i=$((i + 1))
	if [ $i -gt 15 ]; then
		log "IMX415 15 秒内没绑定上（openvela 给 cam3 上电、送 MCLK 了吗？）"
		exit 1
	fi
	sleep 1
	echo 8-0037 > /sys/bus/i2c/drivers/imx415/bind 2>/dev/null
done
log "IMX415 已绑定（等了 ${i} 秒）"

# 异步子设备要等整条链都绑上才注册 video 节点，给一点时间再报清单。
sleep 2
for v in /sys/class/video4linux/*; do
	[ -e "$v/name" ] && log "$(basename "$v") $(cat "$v/name")"
done
