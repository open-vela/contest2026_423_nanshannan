/****************************************************************************
 * board/kickpi-k7/src/kickpi_k7_lcd.c
 *
 * 5 寸 MIPI 屏（F050008M01，720x1280）的板级电源。
 *
 * ★ 本文件的关键约束：**不要复位面板，也不要给面板断电。**
 *
 *   出厂 U-Boot（Rockchip UBOOT DRM driver v1.0.1）在加载我们的镜像之前
 *   已经完成了面板上电、发送厂商初始化序列、配置 VOP2/DSI/D-PHY，并在
 *   VP1 上显示 logo。而那份初始化序列在
 *   rk3576-kickpi-k7-android-mipi-5-720-1280-F050008M01.dtsi 里，我们
 *   拿不到，也无法重新发送。
 *
 *   因此面板一旦被复位或断电，就再也回不到可显示状态 —— 而背光由
 *   LCD_PWREN/LCD_BL 单独控制，仍然亮着。表现为「背光亮、无显示、
 *   所有寄存器读数正常」，极难与配置错误区分。
 *
 *   本文件此前正是这样做的：每次启动都把 LCD_RESET_L 拉低再释放，
 *   等于开机第一件事就把屏打回未初始化状态，之后无论怎么调 VOP2/DSI
 *   都不可能出图。
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>
#include <stdbool.h>
#include <syslog.h>

#include <arch/board/board.h>

#include "arm64_internal.h"
#include "rk3576_gpio.h"
#include "rk3576_pinmux.h"
#include "kickpi_k7.h"

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kickpi_k7_lcd_power(bool on)
{
  if (!on)
    {
      /* 主动断电会毁掉 U-Boot 留下的面板初始化状态，且无法恢复。
       * 这个接口保留是为了调用点的对称性，但不执行任何动作。
       */

      syslog(LOG_WARNING,
             "LCD: 忽略断电请求 —— 断电后无法重新初始化面板\n");
      return OK;
    }

  /* 只确保电源使能仍为高，不做任何跳变。
   *
   * U-Boot 已经把 LCD_PWREN_H 置高并保持。这里把方向设为输出、再写 1，
   * 对已经是「输出且为高」的引脚是幂等操作，不会产生跳变。
   *
   * ★ 绝不触碰 BOARD_LCD_RST：那一脚一动，面板的初始化就没了。
   */

  rk3576_pinmux_set(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN,
                    RK3576_PINMUX_GPIO);
  rk3576_gpio_setdir(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, true);
  rk3576_gpio_write(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN, true);

  syslog(LOG_INFO,
         "LCD: 沿用 U-Boot 的面板状态 PWREN(gpio%d-%d)=%d"
         "（未复位、未断电）\n",
         BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN,
         rk3576_gpio_read(BOARD_LCD_PWREN_BANK, BOARD_LCD_PWREN_PIN));

  return OK;
}
