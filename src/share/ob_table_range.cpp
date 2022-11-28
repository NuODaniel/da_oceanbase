/*
 * (C) Copyright 2022 Alipay Inc. All Rights Reserved.
 * Authors:
 *     Danling <fengjingkun.fjk@antgroup.com>
 */

#include "ob_table_range.h"


namespace oceanbase
{
using palf::SCN;

namespace share
{
const palf::SCN ObScnRange::MIN_SCN = palf::SCN::min_scn();
const palf::SCN ObScnRange::MAX_SCN = palf::SCN::max_scn();

const int64_t ObScnRange::MIN_TS = palf::OB_MIN_SCN_TS_NS;
const int64_t ObScnRange::MAX_TS = palf::OB_MAX_SCN_TS_NS;
const int64_t ObScnRange::OLD_MAX_TS = INT64_MAX;

ObScnRange::ObScnRange()
  : start_scn_(MIN_SCN),
    end_scn_(MIN_SCN)
{
}

OB_DEF_SERIALIZE(ObScnRange)
{
  using oceanbase::common::serialization::encode;
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_ENCODE, start_scn_, end_scn_);
  return ret;
}

OB_DEF_DESERIALIZE(ObScnRange)
{
  using oceanbase::common::serialization::decode;
  int ret = OB_SUCCESS;
  LST_DO_CODE(OB_UNIS_DECODE, start_scn_, end_scn_);

  if (OB_SUCC(ret)) {
    (void)end_scn_.transform_max();
  }
  return ret;
}

OB_DEF_SERIALIZE_SIZE(ObScnRange)
{
  using oceanbase::common::serialization::encoded_length;
  int64_t len = 0;
  LST_DO_CODE(OB_UNIS_ADD_LEN, start_scn_, end_scn_);
  return len;
}

int64_t ObScnRange::hash() const
{
  int64_t hash_value = 0;
  hash_value = common::murmurhash(&start_scn_, sizeof(start_scn_), hash_value);
  hash_value = common::murmurhash(&end_scn_, sizeof(end_scn_), hash_value);
  return hash_value;
}


} //share
} //oceanbase