/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX STORAGE_FTS

#include "storage/fts/ik/ob_ik_cjk_processor.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/ik/ob_ik_char_util.h"
#include "storage/fts/ik/ob_ik_token.h"

namespace oceanbase
{
namespace storage
{
int ObIKCJKProcessor::do_process(TokenizeContext &ctx,
                                 const char *ch,
                                 const uint8_t char_len,
                                 const ObFTCharUtil::CharType type)
{
  int ret = OB_SUCCESS;

  if (ObFTCharUtil::CharType::USELESS != type) {
    const ObString ft_char(char_len, ch);
    // Every pending hit is advanced over the very same character, so the
    // character code only needs to be resolved once per dictionary instead of
    // once per hit. ObFTWordCode is int32_t (see ob_ft_dict_def.h).
    const ObIFTDict *code_dict = nullptr;
    int32_t ft_code = 0;
    bool code_valid = false;
    // handle previous hits_ first and then check from this char
    for (ObList<ObDATrieHit, ObIAllocator>::iterator iter = hits_.begin();
         OB_SUCC(ret) && iter != hits_.end();
         iter++) {
      ObDATrieHit &hit = *iter;
      bool used_code = false;
      int code_ret = OB_SUCCESS;
      if (hit.dict_ == code_dict) {
        used_code = true;
        code_ret = (code_valid ? OB_SUCCESS : OB_ENTRY_NOT_EXIST);
      } else if (OB_ISNULL(hit.dict_)) {
        used_code = false;
      } else {
        code_ret = hit.dict_->get_word_code(ft_char, ft_code);
        if (OB_SUCCESS == code_ret) {
          code_dict = hit.dict_;
          code_valid = true;
          used_code = true;
        } else if (OB_ENTRY_NOT_EXIST == code_ret) {
          code_dict = hit.dict_;
          code_valid = false;
          used_code = true;
        } else {
          // dict cannot resolve codes (or a transient error): fall back
          code_dict = nullptr;
          used_code = false;
        }
      }
      if (used_code) {
        if (OB_SUCCESS == code_ret) {
          ret = hit.dict_->match_with_hit(ft_char, ft_code, hit, hit);
        } else {
          // the character is not in this dictionary at all: the hit is dead
          hit.set_unmatch();
        }
      } else {
        ret = dict_main_.match_with_hit(ft_char, hit, hit);
      }
      if (OB_FAIL(ret)) {
      } else if (hit.is_match()) {
        if (OB_FAIL(ctx.add_token(ctx.fulltext(),
                                  hit.start_pos_,
                                  hit.end_pos_ - hit.start_pos_,
                                  hit.char_cnt_,
                                  ObIKTokenType::IK_CHINESE_TOKEN))) {
        } else if (hit.is_prefix()) {
          // match will record the start_cursor
        }
      } else if (hit.is_prefix()) {
        // nothing
      } else if (hit.is_unmatch()) {
        hits_.erase(hit);
      } else {
        ret = OB_UNEXPECT_INTERNAL_ERROR;
      }
    }
    if (OB_SUCC(ret)) {
      // Start from this char
      // find range of dict
      ObDATrieHit hit(&dict_main_, ctx.get_cursor());
      if (OB_FAIL(dict_main_.match({char_len, ch}, hit))) {
      } else if (hit.is_match()) {
        // output token
        hits_.push_back(hit);
        ctx.add_token(ctx.fulltext(),
                      ctx.get_cursor(),
                      char_len,
                      1,
                      ObIKTokenType::IK_CHINESE_TOKEN);
      } else if (hit.is_prefix()) {
        hits_.push_back(hit);
      } else {
        // ignore mismatch
      }
    }
  } else {
    // stop previous match
    hits_.clear();
  }

  if (OB_SUCC(ret)) {
    if (ctx.is_last()) {
      hits_.clear();
    }
  } else {
    hits_.clear();
  }

  return ret;
}

} //  namespace storage
} //  namespace oceanbase
