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

#include "storage/fts/dict/ob_ft_dict_hub.h"

#include "lib/ob_errno.h"
#include "lib/oblog/ob_log_module.h"
#include "lib/utility/ob_macro_utils.h"
#include "storage/fts/dict/ob_ft_cache_container.h"
#include "storage/fts/dict/ob_ft_dict.h"
#include "storage/fts/dict/ob_ft_range_dict.h"
#include "storage/fts/dict/ob_ft_dict_def.h"
namespace oceanbase
{
namespace storage
{
int ObFTDictHub::init()
{
  static constexpr int K_MAX_DICT_BUCKET = 128; // for now, only built-in dicts.
  int ret = OB_SUCCESS;
  if (OB_FAIL(dict_map_.create(K_MAX_DICT_BUCKET, "dict_map"))) {
  } else if (OB_FAIL(rw_dict_lock_.init(K_MAX_DICT_BUCKET))) {
  } else {
    is_inited_ = true;
  }
  return ret;
};

int ObFTDictHub::destroy()
{
  int ret = OB_SUCCESS;
  is_inited_ = false;
  ATOMIC_STORE(&shared_ready_, false);
  if (OB_NOT_NULL(shared_main_dict_)) {
    shared_main_dict_->~ObIFTDict();
    shared_main_dict_ = nullptr;
  }
  if (OB_NOT_NULL(shared_quan_dict_)) {
    shared_quan_dict_->~ObIFTDict();
    shared_quan_dict_ = nullptr;
  }
  if (OB_NOT_NULL(shared_stop_dict_)) {
    shared_stop_dict_->~ObIFTDict();
    shared_stop_dict_ = nullptr;
  }
  shared_dict_alloc_.reset();
  return ret;
}

int ObFTDictHub::build_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict)
{
  int ret = OB_SUCCESS;
  ObFTCacheRangeContainer *container = nullptr;
  ObFTRangeDict *range_dict = nullptr;
  if (OB_ISNULL(container = OB_NEWx(ObFTCacheRangeContainer, &shared_dict_alloc_,
                                    shared_dict_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_FTS_LOG(WARN, "failed to allocate dict cache container", K(ret), "dict_type", desc.type_);
  } else if (OB_FAIL(load_cache(desc, *container))) {
    if (OB_ENTRY_NOT_EXIST == ret) {
      // The dictionary is not resident in the dict cache yet, build it from
      // the built-in dictionary text. This is the expensive path and it only
      // runs once per process.
      if (OB_FAIL(build_cache(desc, *container))) {
        STORAGE_FTS_LOG(WARN, "failed to build dict cache", K(ret), "dict_type", desc.type_);
      }
    } else {
      STORAGE_FTS_LOG(WARN, "failed to load dict cache", K(ret), "dict_type", desc.type_);
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_ISNULL(range_dict = OB_NEWx(ObFTRangeDict, &shared_dict_alloc_,
                                            shared_dict_alloc_, container, desc))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_FTS_LOG(WARN, "failed to allocate range dict", K(ret), "dict_type", desc.type_);
  } else if (OB_FAIL(range_dict->init())) {
    STORAGE_FTS_LOG(WARN, "failed to init range dict", K(ret), "dict_type", desc.type_);
  } else {
    dict = range_dict;
  }
  return ret;
}

int ObFTDictHub::build_shared_ik_dicts()
{
  int ret = OB_SUCCESS;
  const ObFTDictDesc main_dict_desc("main_dict",
                                    ObFTDictType::DICT_IK_MAIN,
                                    ObCharsetType::CHARSET_UTF8MB4,
                                    ObCollationType::CS_TYPE_UTF8MB4_BIN);
  const ObFTDictDesc quan_dict_desc("quan_dict",
                                    ObFTDictType::DICT_IK_QUAN,
                                    ObCharsetType::CHARSET_UTF8MB4,
                                    ObCollationType::CS_TYPE_UTF8MB4_BIN);
  const ObFTDictDesc stopword_dict_desc("stopword",
                                        ObFTDictType::DICT_IK_STOP,
                                        ObCharsetType::CHARSET_UTF8MB4,
                                        ObCollationType::CS_TYPE_UTF8MB4_BIN);
  if (OB_FAIL(build_shared_dict(main_dict_desc, shared_main_dict_))) {
  } else if (OB_FAIL(build_shared_dict(quan_dict_desc, shared_quan_dict_))) {
  } else if (OB_FAIL(build_shared_dict(stopword_dict_desc, shared_stop_dict_))) {
  }
  return ret;
}

int ObFTDictHub::get_ik_dicts(ObIFTDict *&main_dict,
                              ObIFTDict *&quan_dict,
                              ObIFTDict *&stop_dict)
{
  int ret = OB_SUCCESS;
  main_dict = nullptr;
  quan_dict = nullptr;
  stop_dict = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_LIKELY(ATOMIC_LOAD(&shared_ready_))) {
    // hot path: the dictionary set is immutable after being published
    main_dict = shared_main_dict_;
    quan_dict = shared_quan_dict_;
    stop_dict = shared_stop_dict_;
  } else {
    common::SpinWLockGuard guard(shared_lock_);
    if (ATOMIC_LOAD(&shared_ready_)) {
      main_dict = shared_main_dict_;
      quan_dict = shared_quan_dict_;
      stop_dict = shared_stop_dict_;
    } else if (OB_FAIL(build_shared_ik_dicts())) {
      STORAGE_FTS_LOG(WARN, "failed to build shared ik dicts", K(ret));
    } else {
      ATOMIC_STORE(&shared_ready_, true);
      main_dict = shared_main_dict_;
      quan_dict = shared_quan_dict_;
      stop_dict = shared_stop_dict_;
    }
  }
  return ret;
}
int ObFTDictHub::build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  ObFTDictInfo info;
  container.reset();

  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    ObBucketHashWLockGuard guard(rw_dict_lock_, key.hash());

    // try if valid with no recursive lock
    if (OB_FAIL(get_dict_info(key, info))) {
      if (OB_HASH_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
        ret = OB_ENTRY_NOT_EXIST;
      } else {
      }
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
      } else {
      }
    }

    if (OB_FAIL(ret)) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        if (OB_FAIL(ObFTRangeDict::build_cache_from_ik_dict(desc, container))) {
        } else if (FALSE_IT(info.range_count_ = container.get_handles().size())) {
        } else if (OB_FAIL(put_dict_info(key, info))) {
        }
      }
    }
  }
  return ret;
}

int ObFTDictHub::load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container)
{
  int ret = OB_SUCCESS;
  ObFTDictInfo info;
  container.reset();
  ObFTDictInfoKey key(static_cast<uint64_t>(desc.type_));
  if (!is_inited_) {
    ret = OB_NOT_INIT;
  } else {
    {
      ObBucketHashRLockGuard guard(rw_dict_lock_, key.hash());
      if (OB_FAIL(get_dict_info(key, info))) {
        if (OB_HASH_NOT_EXIST == ret) {
          // dict not exist, make new one, by caller
          ret = OB_ENTRY_NOT_EXIST;
        } else {
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(ObFTRangeDict::try_load_cache(desc, info.range_count_, container))) {
      if (OB_ENTRY_NOT_EXIST == ret) {
        // dict not exist, make new one, by caller
      } else {
      }
    }
  }

  return ret;
}


int ObFTDictHub::get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;

  if (OB_FAIL(dict_map_.get_refactored(key, info))) {
    if (OB_HASH_NOT_EXIST != ret) {
    }
  }

  return ret;
}

int ObFTDictHub::put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info)
{
  int ret = OB_SUCCESS;
  const int cover_exist_flag = 1;
  if (OB_FAIL(dict_map_.set_refactored(key, info, cover_exist_flag))) {
  }

  return ret;
}
} //  namespace storage
} //  namespace oceanbase
