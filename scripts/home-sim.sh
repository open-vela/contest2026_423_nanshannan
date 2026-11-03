#!/usr/bin/env bash
# 在电脑上假扮几台家居设备，给 K7 家居页联调用（主题约定见
# app/kickpi_ui/k7_home.h）。没有真实设备、也没有 Home Assistant 时用它。
#
#   bash scripts/home-sim.sh [broker 地址，默认 localhost]
#
# 收到 home/<id>/set 就把同样的值以 retain 发回 home/<id>/state，像一台
# 听话的设备；收到 home/scene/set 就按场景改几台设备的状态。
# 想测"设备没响应"：SIM_DEAD=tv bash scripts/home-sim.sh，电视就不回报。
# 依赖：mosquitto-clients（apt install mosquitto-clients）。
set -euo pipefail

HOST=${1:-localhost}
DEAD=${SIM_DEAD:-}
DEVICES="living_light desk_lamp ac fresh_air kettle tv"

state() { mosquitto_pub -h "$HOST" -r -t "home/$1/state" -m "$2"; }

scene() {
  case $1 in
    home)  state living_light ON;  state ac ON;  state fresh_air ON ;;
    away)  for d in $DEVICES; do state "$d" OFF; done ;;
    focus) state desk_lamp ON; state living_light OFF; state tv OFF ;;
    rest)  state living_light OFF; state desk_lamp OFF; state tv OFF ;;
    *)     echo "未知场景 $1" >&2 ;;
  esac
}

# 初始状态：都关着，客厅灯开着
for d in $DEVICES; do state "$d" OFF; done
state living_light ON
echo "模拟设备已就绪（broker $HOST）：$DEVICES"

mosquitto_sub -h "$HOST" -v -t 'home/+/set' | while read -r topic payload; do
  id=${topic#home/}; id=${id%/set}
  echo "$(date +%T) $topic $payload"
  if [[ $id == scene ]]; then
    scene "$payload"
  elif [[ " $DEVICES " == *" $id "* && " $DEAD " != *" $id "* ]]; then
    state "$id" "$payload"
  fi
done
