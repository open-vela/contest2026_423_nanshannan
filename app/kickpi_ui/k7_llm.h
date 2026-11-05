/****************************************************************************
 * app/kickpi_ui/k7_llm.h
 * SPDX-License-Identifier: Apache-2.0
 *
 * 一次 chat/completions 请求（模型见 k7_agent_key.h，关深度思考），桌面问答、速记打标签
 * 共用。阻塞（几秒到几十秒），在工作线程里调。
 ****************************************************************************/

#ifndef __APP_KICKPI_UI_K7_LLM_H
#define __APP_KICKPI_UI_K7_LLM_H

#include <stddef.h>

/* image_uri 为 NULL 时是纯文字请求，否则是 data:image/jpeg;base64,... 。
 * 返回 0 成功，answer 里是模型的回答；失败时 answer 里是给用户看的说明。
 */

int k7_llm_chat(const char *image_uri, const char *prompt, int max_tokens,
                char *answer, size_t cap);

#endif /* __APP_KICKPI_UI_K7_LLM_H */
