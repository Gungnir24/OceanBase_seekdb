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

#ifndef _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
#define _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_

#include "lib/allocator/page_arena.h"
#include "lib/atomic/ob_atomic.h"
#include "lib/charset/ob_charset.h"
#include "lib/lock/ob_bucket_lock.h"
#include "lib/lock/ob_spin_rwlock.h"
#include "storage/fts/dict/ob_ft_dict_def.h"

namespace oceanbase
{
namespace storage
{
class ObIFTDict;
// typedef uint64_t ObFTTableID;
// typedef ObFTTableID ObFTDictTableID;

class ObFTDictInfo
{
public:
  ObFTDictInfo()
      : name_(""),
        type_(ObFTDictType::DICT_TYPE_INVALID),
        charset_(CHARSET_INVALID),
        version_(0),
        range_count_(0)
  {
  }

public:
  char name_[2048]; // for now
  ObFTDictType type_;
  ObCharsetType charset_;
  int64_t version_; // in memory
  int32_t range_count_;
};

struct ObFTDictInfoKey
{
public:
  ObFTDictInfoKey()
      : type_(static_cast<uint64_t>(ObFTDictType::DICT_TYPE_INVALID))
  {
  } // default constructor
  ObFTDictInfoKey(const uint64_t type)
      : type_(type)
  {
  }
  int hash(uint64_t &hash_value) const
  {
    int ret = OB_SUCCESS;
    hash_value = hash();
    return ret;
  }

  uint64_t hash() const
  {
    uint64_t hash = 0;
    hash = common::murmurhash(&type_, sizeof(int64_t), hash);
    return hash;
  }

  bool operator==(const ObFTDictInfoKey &other) const
  {
    return type_ == other.type_;
  }

  int compare(const ObFTDictInfoKey &other) const
  {
    int ret = 0;
    if (0 == ret) {
      ret = type_ - other.type_;
    }
    return ret;
  }

private:
  uint64_t type_;
  // name
};

class ObFTCacheRangeContainer;
class ObFTDictHub
{
public:
  ObFTDictHub()
      : is_inited_(false),
        dict_map_(),
        rw_dict_lock_(),
        shared_lock_(),
        shared_dict_alloc_(lib::ObMemAttr("FTDictShared")),
        shared_ready_(false),
        shared_main_dict_(nullptr),
        shared_quan_dict_(nullptr),
        shared_stop_dict_(nullptr)
  {
  }
  ~ObFTDictHub() {}

  int init();

  int destroy();

  int build_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  int load_cache(const ObFTDictDesc &desc, ObFTCacheRangeContainer &container);

  // Get the shared IK dictionary set.
  //
  // The built-in IK dictionaries are immutable once their DAT blocks are put
  // into the dict cache, so they can be shared by all tokenizer instances
  // instead of being re-fetched (and re-assembled into range dicts) for every
  // single parse call. The set is materialized on first use and afterwards the
  // hot path is a single atomic load.
  int get_ik_dicts(ObIFTDict *&main_dict, ObIFTDict *&quan_dict, ObIFTDict *&stop_dict);

private:
  int get_dict_info(const ObFTDictInfoKey &key, ObFTDictInfo &info);

  int put_dict_info(const ObFTDictInfoKey &key, const ObFTDictInfo &info);

  int build_shared_ik_dicts();

  int build_shared_dict(const ObFTDictDesc &desc, ObIFTDict *&dict);

private:
  bool is_inited_;
  // holds info of dict
  hash::ObHashMap<ObFTDictInfoKey, ObFTDictInfo> dict_map_;
  ObBucketLock rw_dict_lock_;

  // shared (built once, read-only afterwards) IK dictionary set
  common::SpinRWLock shared_lock_;
  common::ObArenaAllocator shared_dict_alloc_;
  bool shared_ready_;
  ObIFTDict *shared_main_dict_;
  ObIFTDict *shared_quan_dict_;
  ObIFTDict *shared_stop_dict_;

  DISALLOW_COPY_AND_ASSIGN(ObFTDictHub);
};

} //  namespace storage
} //  namespace oceanbase

#endif // _OCEANBASE_STORAGE_FTS_DICT_OB_FT_DICT_HUB_H_
