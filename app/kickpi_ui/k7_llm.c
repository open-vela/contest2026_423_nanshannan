/****************************************************************************
 * app/kickpi_ui/k7_llm.c
 * SPDX-License-Identifier: Apache-2.0
 *
 * ★ 请求
 *
 *   POST /v1/chat/completions，"thinking":{"type":"disabled"}（mimo-v2.5
 *   默认开深度思考，板上实测一次 196s，关掉后降到 20s 上下）。
 *   带图时 user content = [image_url, text]，纯文字时 content 就是字符串。
 *   回答在 choices[0].message.content。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <cJSON.h>

#include "k7_llm.h"
#include "k7_net.h"

#ifdef __has_include
#  if __has_include("k7_agent_key.h")
#    include "k7_agent_key.h"
#  endif
#endif

#define LLM_PATH     "/v1/chat/completions"
#define LLM_RESP_CAP (64 * 1024)

int k7_llm_chat(const char *image_uri, const char *prompt, int max_tokens,
                char *answer, size_t cap)
{
#ifndef K7_AGENT_LLM_KEY
  (void)image_uri;
  (void)prompt;
  (void)max_tokens;
  snprintf(answer, cap, "没有配置模型 Key（k7_agent_key.h）。");
  return -ENOTSUP;
#else
  static const char auth[] = "Bearer " K7_AGENT_LLM_KEY;
  const vela_header_t hdrs[] =
  {
    { "Authorization", auth },
    { NULL, NULL }
  };

  struct timespec t0;
  struct timespec t1;
  cJSON *root;
  cJSON *msgs;
  cJSON *m;
  char *body = NULL;
  char *resp = NULL;
  int status;
  int ret;

  root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "model", image_uri != NULL ?
                          K7_AGENT_VISION_MODEL : K7_AGENT_LLM_MODEL);
  cJSON_AddStringToObject(cJSON_AddObjectToObject(root, "thinking"),
                          "type", "disabled");
  cJSON_AddNumberToObject(root, "max_completion_tokens", max_tokens);
  msgs = cJSON_AddArrayToObject(root, "messages");
  m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", "user");

  if (image_uri != NULL)
    {
      cJSON *content = cJSON_AddArrayToObject(m, "content");
      cJSON *part;

      part = cJSON_CreateObject();
      cJSON_AddStringToObject(part, "type", "image_url");
      cJSON_AddStringToObject(cJSON_AddObjectToObject(part, "image_url"),
                              "url", image_uri);
      cJSON_AddItemToArray(content, part);

      part = cJSON_CreateObject();
      cJSON_AddStringToObject(part, "type", "text");
      cJSON_AddStringToObject(part, "text", prompt);
      cJSON_AddItemToArray(content, part);
    }
  else
    {
      cJSON_AddStringToObject(m, "content", prompt);
    }

  cJSON_AddItemToArray(msgs, m);
  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  root = NULL;

  resp = malloc(LLM_RESP_CAP);
  if (body == NULL || resp == NULL)
    {
      snprintf(answer, cap, "内存不够。");
      ret = -ENOMEM;
      goto out;
    }

  /* 请求前后各记一行：一次请求卡多久、卡在哪，串口上一眼能看出来 */

  clock_gettime(CLOCK_MONOTONIC, &t0);
  syslog(LOG_INFO, "模型: 请求 %zu 字节%s\n", strlen(body),
         image_uri != NULL ? "（带图）" : "");
  status = vela_https_post_json(K7_AGENT_LLM_HOST, "443", LLM_PATH, hdrs,
                                body, resp, LLM_RESP_CAP);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  syslog(LOG_INFO, "模型: HTTP %d，用时 %ld ms\n", status,
         (long)((t1.tv_sec - t0.tv_sec) * 1000 +
                (t1.tv_nsec - t0.tv_nsec) / 1000000));
  if (status != 200)
    {
      resp[status > 0 ? 200 : 0] = '\0';
      syslog(LOG_ERR, "模型: HTTP %d %s\n", status, resp);
      snprintf(answer, cap, "请求模型失败（HTTP %d）。", status);
      ret = status < 0 ? status : -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  m = cJSON_GetObjectItem(cJSON_GetObjectItem(
        cJSON_GetArrayItem(cJSON_GetObjectItem(root, "choices"), 0),
        "message"), "content");
  if (!cJSON_IsString(m))
    {
      snprintf(answer, cap, "模型没有给出回答。");
      ret = -EPROTO;
      goto out;
    }

  snprintf(answer, cap, "%s", m->valuestring);
  ret = 0;

out:
  cJSON_Delete(root);
  cJSON_free(body);
  free(resp);
  return ret;
#endif
}
