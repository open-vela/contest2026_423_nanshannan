/* SPDX-License-Identifier: Apache-2.0 */

/****************************************************************************
 * SKW6621S Bluetooth vendor HCI initialization.
 *
 * Follows the SDK skwbt driver (skw_btdriver.c btseekwave_download_nv):
 *   HCI Read Local Version -> NVDS pages (0xFC80) from sv6160lite.nvbin
 * then HCI Reset and Read BD_ADDR to prove the controller answers.
 * Commands are sent one at a time and each waits (bounded) for its
 * Command Complete / Command Status.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <string.h>
#include <syslog.h>
#include <sys/stat.h>

#include <nuttx/arch.h>
#include <nuttx/clock.h>
#include <nuttx/fs/fs.h>
#include <nuttx/kmalloc.h>

#include "skw6621s_sdio.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define HCI_CMD_READ_LOCAL_VERSION  0x1001
#define HCI_CMD_READ_BD_ADDR        0x1009
#define HCI_CMD_RESET               0x0c03
#define HCI_CMD_SKW_BT_NVDS         0xfc80

#define NV_FILE_RD_BLOCK_SIZE       252
#define NV_TAG_DSP_LOG_SETTING      0x05
#define NV_MAX_SIZE                 8192

#define HCI_EVT_CMD_COMPLETE        0x0e
#define HCI_EVT_CMD_STATUS          0x0f

#define BT_CMD_TIMEOUT_MS           1000

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Last Command Complete / Status, filled by the HCI callback */

static uint8_t g_evt[260];
static size_t g_evt_len;
static volatile bool g_evt_valid;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bt_init_cb(FAR void *arg, uint8_t type,
                       FAR const uint8_t *data, size_t len)
{
  if (type != 0x04 || len < 2)
    {
      syslog(LOG_INFO, "SKW BT: rx type %02x len %zu\n", type, len);
      return;
    }

  if (data[0] == HCI_EVT_CMD_COMPLETE || data[0] == HCI_EVT_CMD_STATUS)
    {
      g_evt_len = len < sizeof(g_evt) ? len : sizeof(g_evt);
      memcpy(g_evt, data, g_evt_len);
      g_evt_valid = true;
    }
  else
    {
      syslog(LOG_INFO, "SKW BT: event 0x%02x len %zu\n", data[0], len);
    }
}

/* Send one command and wait for its CC / CS.  Returns the HCI status
 * (>= 0) or a negative errno.  On CC the return parameters after the
 * status byte are copied to rp.
 */

static int bt_cmd(uint16_t opcode, FAR const uint8_t *param, uint8_t plen,
                  FAR uint8_t *rp, size_t rpsize, FAR size_t *rplen)
{
  uint8_t cmd[3 + 255];
  clock_t start;
  int ret;

  cmd[0] = opcode & 0xff;
  cmd[1] = opcode >> 8;
  cmd[2] = plen;
  if (plen > 0)
    {
      memcpy(cmd + 3, param, plen);
    }

  g_evt_valid = false;
  ret = skw6621s_hci_send(0x01, cmd, 3 + plen);
  if (ret < 0)
    {
      return ret;
    }

  start = clock_systime_ticks();
  while (TICK2MSEC(clock_systime_ticks() - start) < BT_CMD_TIMEOUT_MS)
    {
      ret = skw6621s_poll();
      if (ret < 0)
        {
          return ret;
        }

      if (g_evt_valid)
        {
          /* CC: evt plen ncmd op_lo op_hi status rp...
           * CS: evt plen status ncmd op_lo op_hi
           */

          if (g_evt[0] == HCI_EVT_CMD_COMPLETE && g_evt_len >= 6 &&
              (g_evt[3] | g_evt[4] << 8) == opcode)
            {
              if (rp != NULL && rplen != NULL)
                {
                  *rplen = g_evt_len - 6 < rpsize ? g_evt_len - 6 : rpsize;
                  memcpy(rp, g_evt + 6, *rplen);
                }

              return g_evt[5];
            }

          if (g_evt[0] == HCI_EVT_CMD_STATUS && g_evt_len >= 6 &&
              (g_evt[4] | g_evt[5] << 8) == opcode)
            {
              return g_evt[2];
            }

          syslog(LOG_INFO, "SKW BT: unmatched event %02x for %04x\n",
                 g_evt[0], opcode);
          g_evt_valid = false;
        }

      if (ret == 0)
        {
          up_mdelay(2);
        }
    }

  syslog(LOG_ERR, "SKW BT: command %04x timeout\n", opcode);
  return -ETIMEDOUT;
}

static int bt_load_nv(FAR const char *path, FAR uint8_t **data,
                      FAR size_t *size)
{
  struct file filep;
  struct stat st;
  ssize_t nread;
  int ret;

  ret = file_open(&filep, path, O_RDONLY);
  if (ret < 0)
    {
      return ret;
    }

  ret = file_fstat(&filep, &st);
  if (ret < 0 || st.st_size <= 4 || st.st_size > NV_MAX_SIZE)
    {
      file_close(&filep);
      return ret < 0 ? ret : -EFBIG;
    }

  *data = kmm_malloc(st.st_size);
  if (*data == NULL)
    {
      file_close(&filep);
      return -ENOMEM;
    }

  nread = file_read(&filep, *data, st.st_size);
  file_close(&filep);
  if (nread != st.st_size)
    {
      kmm_free(*data);
      *data = NULL;
      return -EIO;
    }

  *size = st.st_size;
  return OK;
}

/* btseekwave_download_nv, 6160-LITE branch: skip the 4-byte header, then
 * TLVs (tag, ?, len) packed into pages of < 252 bytes, each sent as
 * NVDS [page, total_len, TLVs...].  The DSP log setting is forced off.
 */

static int bt_download_nv(FAR const char *path)
{
  FAR uint8_t *nv = NULL;
  uint8_t pld[2 + NV_FILE_RD_BLOCK_SIZE];
  size_t size = 0;
  size_t count = 4;
  size_t total = 0;
  int page = 0;
  int ret;

  ret = bt_load_nv(path, &nv, &size);
  if (ret < 0)
    {
      syslog(LOG_ERR, "SKW BT: NV %s: %d\n", path, ret);
      return ret;
    }

  while (count + 3 <= size)
    {
      uint8_t tag = nv[count];
      size_t pkt = nv[count + 2] + 3;

      if (count + pkt > size)
        {
          ret = -EINVAL;
          break;
        }

      if (pkt + total >= NV_FILE_RD_BLOCK_SIZE)
        {
          pld[0] = page;
          pld[1] = total;
          ret = bt_cmd(HCI_CMD_SKW_BT_NVDS, pld, total + 2, NULL, 0, NULL);
          if (ret != 0)
            {
              break;
            }

          page++;
          total = 0;
          continue;
        }

      memcpy(pld + 2 + total, nv + count, pkt);
      if (tag == NV_TAG_DSP_LOG_SETTING)
        {
          pld[2 + total + 3] = 1;
        }

      count += pkt;
      total += pkt;
    }

  if (ret == 0 && total > 0)
    {
      pld[0] = page;
      pld[1] = total;
      ret = bt_cmd(HCI_CMD_SKW_BT_NVDS, pld, total + 2, NULL, 0, NULL);
      page++;
    }

  syslog(LOG_INFO, "SKW BT: NV %zu bytes, %d pages, ret %d\n", size, page,
         ret);
  kmm_free(nv);
  return ret == 0 ? OK : (ret > 0 ? -EIO : ret);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int skw6621s_bt_hci_init(FAR const char *nvpath)
{
  uint8_t rp[16] =
  {
    0
  };

  size_t rplen = 0;
  int ret;

  ret = skw6621s_bt_start();
  if (ret < 0)
    {
      return ret;
    }

  skw6621s_set_hci_cb(bt_init_cb, NULL);

  ret = bt_cmd(HCI_CMD_READ_LOCAL_VERSION, NULL, 0, rp, sizeof(rp), &rplen);
  syslog(LOG_INFO, "SKW BT: read local version %d: hci %02x rev %04x "
         "lmp %02x manuf %04x sub %04x\n", ret, rp[0], rp[1] | rp[2] << 8,
         rp[3], rp[4] | rp[5] << 8, rp[6] | rp[7] << 8);
  if (ret != 0)
    {
      goto out;
    }

  ret = bt_download_nv(nvpath);
  if (ret < 0)
    {
      goto out;
    }

  ret = bt_cmd(HCI_CMD_RESET, NULL, 0, NULL, 0, NULL);
  syslog(LOG_INFO, "SKW BT: reset %d\n", ret);
  if (ret != 0)
    {
      goto out;
    }

  ret = bt_cmd(HCI_CMD_READ_BD_ADDR, NULL, 0, rp, sizeof(rp), &rplen);
  syslog(LOG_INFO, "SKW BT: BD_ADDR %d: %02x:%02x:%02x:%02x:%02x:%02x\n",
         ret, rp[5], rp[4], rp[3], rp[2], rp[1], rp[0]);

out:
  skw6621s_set_hci_cb(NULL, NULL);
  return ret == 0 ? OK : (ret > 0 ? -EIO : ret);
}
