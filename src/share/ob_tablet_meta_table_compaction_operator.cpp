/*
*  Copyright (c) 2021 Ant Group CO., Ltd.
*  OceanBase is licensed under Mulan PubL v1.
*  You can use this software according to the terms and conditions of the Mulan PubL v1.
*  You may obtain a copy of Mulan PubL v1 at: http://license.coscl.org.cn/MulanPubL-1.0
*  THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
*  EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
*  MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
*  See the Mulan PubL v1 for more details.
*/
#define USING_LOG_PREFIX SHARE
#include "ob_tablet_meta_table_compaction_operator.h"
#include "lib/mysqlclient/ob_mysql_result.h"
#include "lib/oblog/ob_log.h"
#include "lib/string/ob_sql_string.h"
#include "share/inner_table/ob_inner_table_schema.h"
#include "share/ob_dml_sql_splicer.h"
#include "share/tablet/ob_tablet_filter.h"
#include "observer/ob_server_struct.h"

namespace oceanbase
{
namespace share
{
using namespace oceanbase::common;
using namespace oceanbase::common::sqlclient;

// update status of all rows
int ObTabletMetaTableCompactionOperator::set_info_status(
    const ObTabletCompactionScnInfo &input_info,
    ObTabletCompactionScnInfo &ret_info)
{
  int ret = OB_SUCCESS;
  ObMySQLTransaction trans;
  ObSqlString sql;
  ObDMLSqlSplicer dml;
  int64_t affected_rows = 0;
  if (OB_UNLIKELY(!input_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(input_info));
  } else {
    const uint64_t meta_tenant_id = gen_meta_tenant_id(input_info.tenant_id_);
    if (OB_FAIL(trans.start(GCTX.sql_proxy_, meta_tenant_id))) {// start trans
      LOG_WARN("fail to start transaction", KR(ret), K(input_info), K(meta_tenant_id));
    } else if (OB_FAIL(do_select(trans, true/*select_with_update*/, input_info, ret_info))) {
      LOG_WARN("failed to do select", K(ret), K(input_info));
    } else if (OB_FAIL(dml.add_pk_column("tenant_id", input_info.tenant_id_))
        || OB_FAIL(dml.add_pk_column("ls_id", input_info.ls_id_))
        || OB_FAIL(dml.add_pk_column("tablet_id", input_info.tablet_id_))
        || OB_FAIL(dml.add_column("status", (int64_t)input_info.status_))) {
      LOG_WARN("add column failed", KR(ret), K(input_info));
    } else if (OB_FAIL(dml.splice_update_sql(OB_ALL_TABLET_META_TABLE_TNAME, sql))) {
      LOG_WARN("fail to splice batch insert update sql", KR(ret), K(sql));
    } else if (OB_FAIL(trans.write(meta_tenant_id, sql.ptr(), affected_rows))) {
      LOG_WARN("fail to execute sql", K(input_info), K(meta_tenant_id), K(sql));
    } else if (OB_UNLIKELY(0 == affected_rows)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("affected rows is invalid", K(ret), K(input_info), K(affected_rows));
    } else{
      FLOG_INFO("success to set info status", K(ret), K(input_info), K(ret_info));
    }
    handle_trans_stat(trans, ret);
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::get_status(
    const ObTabletCompactionScnInfo &input_info,
    ObTabletCompactionScnInfo &ret_info)
{
  int ret = OB_SUCCESS;
  ret_info.reset();
  ObISQLClient *sql_client = GCTX.sql_proxy_;
  if (OB_UNLIKELY(!input_info.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", K(ret), K(input_info));
  } else if (OB_ISNULL(sql_client)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql client is null", K(ret), KP(sql_client));
  } else if (OB_FAIL(do_select(*sql_client, false/*select_for_update*/, input_info, ret_info))) {
    if (OB_ENTRY_NOT_EXIST != ret) {
      LOG_WARN("failed to select from tablet compaction scn tablet", KR(ret), K(input_info));
    }
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::diagnose_compaction_scn(
    const int64_t tenant_id,
    int64_t &error_tablet_cnt)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is unexpected null", K(ret));
  }
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObZone zone;
    ObMySQLResult *result = nullptr;
    ObSqlString sql;
    const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
    if (OB_FAIL(sql.append_fmt(
            "SELECT count(1) as c FROM %s WHERE tenant_id = '%ld' AND status = '%ld'",
            OB_ALL_TABLET_META_TABLE_TNAME,
            tenant_id,
            (int64_t )ObTabletReplica::SCN_STATUS_ERROR))) {
      LOG_WARN("failed to append fmt", K(ret), K(tenant_id));
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, meta_tenant_id, sql.ptr()))) {
      LOG_WARN("fail to do read", KR(ret), K(meta_tenant_id), K(sql.ptr()));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get result", KR(ret), K(meta_tenant_id), K(sql.ptr()));
    } else if (OB_FAIL(result->get_int("c", error_tablet_cnt))) {
      LOG_WARN("failed to get int", KR(ret));
    }
  }
  return ret;
}

void ObTabletMetaTableCompactionOperator::handle_trans_stat(
    ObMySQLTransaction &trans,
    int &ret)
{
  if (trans.is_started()) {
    int tmp_ret = OB_SUCCESS;
    if (OB_TMP_FAIL(trans.end(OB_SUCC(ret)))) {
      LOG_WARN("trans end failed", "is_commit", OB_SUCC(ret), K(tmp_ret));
      ret = OB_SUCC(ret) ? tmp_ret : ret;
    }
  }
}

int ObTabletMetaTableCompactionOperator::do_select(
    ObISQLClient &sql_client,
    const bool select_with_update,
    const ObTabletCompactionScnInfo &input_info,
    ObTabletCompactionScnInfo &ret_info)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(input_info.tenant_id_);
  ret_info = input_info; // assign tenant_id / ls_id / tablet_id

  if (OB_FAIL(sql.append_fmt(
      "SELECT max(report_scn) as report_scn, max(status) as status"
      " FROM %s WHERE tenant_id = '%lu' AND ls_id = '%ld' AND tablet_id = '%ld'%s",
          OB_ALL_TABLET_META_TABLE_TNAME,
          input_info.tenant_id_,
          input_info.ls_id_,
          input_info.tablet_id_,
          select_with_update ? " FOR UPDATE" : ""))) {
    LOG_WARN("failed to append fmt", K(ret), K(input_info));
  } else {
    ret = execute_select_sql(sql_client, meta_tenant_id, sql, ret_info);
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::execute_select_sql(
    ObISQLClient &sql_client,
    const int64_t meta_tenant_id,
    const ObSqlString &sql,
    ObTabletCompactionScnInfo &ret_info)
{
  int ret = OB_SUCCESS;
  SMART_VAR(ObMySQLProxy::MySQLResult, res) {
    ObMySQLResult *result = nullptr;
    if (OB_FAIL(sql_client.read(res, meta_tenant_id, sql.ptr()))) {
      LOG_WARN("fail to do read", KR(ret), K(meta_tenant_id), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fail to get result", KR(ret), K(meta_tenant_id), K(sql));
    } else if (OB_FAIL(construct_compaction_related_info(*result, ret_info))) {
      if (OB_ENTRY_NOT_EXIST != ret) {
        LOG_WARN("fail to get medium snapshot info", KR(ret), KP(result), K(sql));
      }
    } else {
      LOG_TRACE("success to get medium snapshot info", K(ret_info));
    }
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::batch_update_unequal_report_scn_tablet(
      const uint64_t tenant_id,
      const share::ObLSID &ls_id,
      const int64_t major_frozen_scn,
      const common::ObIArray<ObTabletID> &input_tablet_id_array)
{
  int ret = OB_SUCCESS;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
  int64_t start_idx = 0;
  int64_t end_idx = min(MAX_BATCH_COUNT, input_tablet_id_array.count());
  common::ObSEArray<ObTabletID, 32> unequal_tablet_id_array;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("sql proxy is unexpected null", K(ret));
  }
  while (OB_SUCC(ret) && (start_idx < end_idx)) {
    ObSqlString sql;
    if (OB_FAIL(sql.append_fmt(
        "select distinct(tablet_id) from %s where tenant_id = '%lu' AND ls_id = '%ld'"
          " AND tablet_id IN (",
            OB_ALL_TABLET_META_TABLE_TNAME,
            tenant_id,
            ls_id.id()))) {
      LOG_WARN("failed to assign sql", K(ret), K(tenant_id), K(start_idx));
    } else if (OB_FAIL(append_tablet_id_array(tenant_id, input_tablet_id_array, start_idx, end_idx, sql))) {
      LOG_WARN("fail to append tablet id array", KR(ret), K(tenant_id),
        K(input_tablet_id_array.count()), K(start_idx), K(end_idx));
    } else if (OB_FAIL(sql.append_fmt(") AND compaction_scn = '%lu' AND report_scn < '%lu'",
        major_frozen_scn, major_frozen_scn))) {
      LOG_WARN("failed to assign sql", K(ret), K(tenant_id), K(start_idx));
    } else {
      SMART_VAR(ObISQLClient::ReadResult, result) {
        if (OB_FAIL(GCTX.sql_proxy_->read(result, meta_tenant_id, sql.ptr()))) {
          LOG_WARN("fail to execute sql", KR(ret), K(tenant_id), K(meta_tenant_id), "sql", sql.ptr());
        } else if (OB_ISNULL(result.get_result())) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fail to get mysql result", KR(ret), "sql", sql.ptr());
        } else if (OB_FAIL(construct_unequal_tablet_id_array(*result.get_result(), unequal_tablet_id_array))) {
          LOG_WARN("fail to construct tablet id array", KR(ret), "sql", sql.ptr());
        }
      }
      if (OB_FAIL(ret)) {
      } else if (unequal_tablet_id_array.empty()) {
        // do nothing
      } else if (OB_FAIL(inner_batch_update_unequal_report_scn_tablet(
              tenant_id,
              ls_id,
              major_frozen_scn,
              unequal_tablet_id_array))) {
        LOG_WARN("fail to update unequal tablet id array", KR(ret), "sql", sql.ptr());
      } else {
        unequal_tablet_id_array.reuse();
      }
    }
    if (OB_SUCC(ret)) {
      start_idx = end_idx;
      end_idx = min(start_idx + MAX_BATCH_COUNT, input_tablet_id_array.count());
    }
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::construct_unequal_tablet_id_array(
    sqlclient::ObMySQLResult &result,
    common::ObIArray<ObTabletID> &unequal_tablet_id_array)
{
  int ret = OB_SUCCESS;
  int64_t tablet_id = 0;
  while (OB_SUCC(ret)) {
    if (OB_FAIL(result.next())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      } else {
        LOG_WARN("fail to get next result", KR(ret));
      }
      break;
    } else if (OB_FAIL(result.get_int("tablet_id", tablet_id))) {
      LOG_WARN("fail to get uint", KR(ret));
    } else if (OB_FAIL(unequal_tablet_id_array.push_back(ObTabletID(tablet_id)))) {
      LOG_WARN("failed to push back tablet id", K(ret), K(tablet_id));
    }
  }
  if (OB_SUCC(ret) && unequal_tablet_id_array.count() > 0) {
    LOG_TRACE("success to get uneuqal tablet_id array", K(ret), K(unequal_tablet_id_array));
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::append_tablet_id_array(
    const uint64_t tenant_id,
    const common::ObIArray<ObTabletID> &input_tablet_id_array,
    const int64_t start_idx,
    const int64_t end_idx,
    ObSqlString &sql)
{
  int ret = OB_SUCCESS;
  for (int64_t idx = start_idx; OB_SUCC(ret) && (idx < end_idx); ++idx) {
    const ObTabletID &tablet_id = input_tablet_id_array.at(idx);
    if (OB_UNLIKELY(!tablet_id.is_valid_with_tenant(tenant_id))) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("invalid tablet_id with tenant", KR(ret), K(tenant_id), K(tablet_id));
    } else if (OB_FAIL(sql.append_fmt(
        "%s %ld",
        start_idx == idx ? "" : ",",
        tablet_id.id()))) {
      LOG_WARN("fail to assign sql", KR(ret), K(idx), K(tablet_id));
    }
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::inner_batch_update_unequal_report_scn_tablet(
    const uint64_t tenant_id,
    const share::ObLSID &ls_id,
    const int64_t major_frozen_scn,
    const common::ObIArray<ObTabletID> &unequal_tablet_id_array)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
  ObSqlString sql;
  if (OB_FAIL(sql.append_fmt("UPDATE %s SET report_scn='%lu' WHERE tenant_id='%lu' AND ls_id='%ld' AND tablet_id IN (",
      OB_ALL_TABLET_META_TABLE_TNAME,
      major_frozen_scn,
      tenant_id,
      ls_id.id()))) {
    LOG_WARN("failed to append fmt", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(append_tablet_id_array(tenant_id, unequal_tablet_id_array, 0, unequal_tablet_id_array.count(), sql))) {
    LOG_WARN("fail to append tablet id array", KR(ret), K(tenant_id), K(unequal_tablet_id_array));
  } else if (OB_FAIL(sql.append_fmt(") AND compaction_scn = '%lu' AND report_scn <'%lu'",
      major_frozen_scn, major_frozen_scn))) {
    LOG_WARN("failed to assign sql", K(ret), K(tenant_id), K(ls_id));
  } else if (OB_FAIL(GCTX.sql_proxy_->write(meta_tenant_id, sql.ptr(), affected_rows))) {
    LOG_WARN("fail to execute sql", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
  } else if (affected_rows > 0) {
    LOG_INFO("success to update unequal report_scn", K(ret), K(tenant_id), K(ls_id), K(unequal_tablet_id_array.count()));
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::construct_compaction_related_info(
    sqlclient::ObMySQLResult &result,
    ObTabletCompactionScnInfo &info)
{
  int ret = OB_SUCCESS;
  uint64_t report_scn_in_table = 0;
  int64_t status = 0;
  if (OB_FAIL(result.get_uint("report_scn", report_scn_in_table))) {
    if (OB_ERR_NULL_VALUE == ret) {
      ret = OB_ENTRY_NOT_EXIST;
    } else {
      LOG_WARN("failed to get int", KR(ret), K(info));
    }
  } else if (OB_FAIL(result.get_int("status", status))) {
    LOG_WARN("failed to get int", KR(ret), K(status));
  } else if (OB_UNLIKELY(!ObTabletReplica::is_status_valid((ObTabletReplica::ScnStatus)status))) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("status is invalid", KR(ret), K(status));
  } else {
    info.report_scn_ = (int64_t)report_scn_in_table;
    info.status_ = ObTabletReplica::ScnStatus(status);
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::batch_update_report_scn(
    const uint64_t tenant_id,
    const uint64_t global_braodcast_scn_val,
    const ObTabletReplica::ScnStatus &except_status)
{
  int ret = OB_SUCCESS;
  uint64_t compat_version = 0;
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id));
  } else if (OB_FAIL(GET_MIN_DATA_VERSION(tenant_id, compat_version))) {
    LOG_WARN("fail to get data version", KR(ret), K(tenant_id));
  } else if (compat_version < DATA_VERSION_4_1_0_0) {
    // do nothing
  } else {
    ObMySQLTransaction trans;
    int64_t affected_rows = 0;
    const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
    if (OB_FAIL(trans.start(GCTX.sql_proxy_, meta_tenant_id))) {
      LOG_WARN("fail to start transaction", KR(ret), K(tenant_id), K(meta_tenant_id));
    } else {
      ObSqlString sql;
      // TODO tenant may have a great many tablets, so we should use batch splitting strategy to update
      if (OB_FAIL(sql.assign_fmt("UPDATE %s SET report_scn = '%lu' WHERE tenant_id = '%ld' "
          "AND compaction_scn >= '%lu' AND status != '%ld'",
              OB_ALL_TABLET_META_TABLE_TNAME,
              global_braodcast_scn_val,
              tenant_id,
              global_braodcast_scn_val,
              (int64_t )except_status))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(global_braodcast_scn_val), K(except_status));
      } else if (OB_FAIL(trans.write(meta_tenant_id, sql.ptr(), affected_rows))) {
        LOG_WARN("fail to execute sql", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
      }
    }
    handle_trans_stat(trans, ret);
    LOG_INFO("finish to batch update report scn", KR(ret), K(tenant_id), K(affected_rows));
  }
  return ret;
}

int ObTabletMetaTableCompactionOperator::batch_update_report_scn(
    const uint64_t tenant_id,
    const uint64_t global_braodcast_scn_val,
    const ObIArray<ObTabletLSPair> &tablet_pairs,
    const ObTabletReplica::ScnStatus &except_status)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  uint64_t compat_version = 0;
  ObDMLSqlSplicer dml;
  const int64_t all_pair_cnt = tablet_pairs.count();
  if (OB_UNLIKELY((all_pair_cnt < 1)
      || !is_valid_tenant_id(tenant_id))) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(all_pair_cnt));
  } else if (OB_FAIL(GET_MIN_DATA_VERSION(tenant_id, compat_version))) {
    LOG_WARN("fail to get data version", KR(ret), K(tenant_id));
  } else if (compat_version < DATA_VERSION_4_1_0_0) {
  } else {
    const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
    ObMySQLTransaction trans;
    if (OB_FAIL(trans.start(GCTX.sql_proxy_, meta_tenant_id))) {
      LOG_WARN("fail to start transaction", KR(ret), K(tenant_id), K(meta_tenant_id));
    }
    for (int64_t i = 0; OB_SUCC(ret) && (i < all_pair_cnt); i += MAX_BATCH_COUNT) {
      const int64_t cur_end_idx = MIN(i + MAX_BATCH_COUNT, all_pair_cnt);
      ObSqlString sql;
      if (OB_FAIL(sql.append_fmt(
          "UPDATE %s SET report_scn = '%lu' WHERE tenant_id = %ld AND (tablet_id,ls_id) IN (",
          OB_ALL_TABLET_META_TABLE_TNAME,
          global_braodcast_scn_val,
          tenant_id))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(global_braodcast_scn_val));
      } else {
        // handle each batch tablet_ls_pairs
        for (int64_t idx = i; OB_SUCC(ret) && (idx < cur_end_idx); ++idx) {
          const ObTabletID &tablet_id = tablet_pairs.at(idx).get_tablet_id();
          const ObLSID &ls_id = tablet_pairs.at(idx).get_ls_id();
          if (OB_UNLIKELY(!tablet_id.is_valid_with_tenant(tenant_id)
              || !ls_id.is_valid_with_tenant(tenant_id))) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid tablet_id with tenant", KR(ret), K(tenant_id), K(tablet_id), K(ls_id));
          } else if (OB_FAIL(sql.append_fmt(
              "%s (%ld,%ld)",
              i == idx ? "" : ",",
              tablet_id.id(),
              ls_id.id()))) {
            LOG_WARN("fail to assign sql", KR(ret), K(tablet_id));
          }
        } // end for
        if (FAILEDx(sql.append_fmt(") AND compaction_scn >= '%lu' AND status != %ld",
            global_braodcast_scn_val,
            (int64_t)(except_status)))) {
          LOG_WARN("fail to assign sql", KR(ret), K(tenant_id), K(meta_tenant_id), K(except_status),
            K(global_braodcast_scn_val));
        } else if (OB_FAIL(trans.write(meta_tenant_id, sql.ptr(), affected_rows))) {
          LOG_WARN("fail to execute sql", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
        } else {
          LOG_TRACE("success to update report_scn", KR(ret), K(tenant_id), K(meta_tenant_id), K(tablet_pairs), K(sql));
        }
      }
    }
    handle_trans_stat(trans, ret);
  }

  return ret;
}

int ObTabletMetaTableCompactionOperator::get_unique_status(
    const uint64_t tenant_id,
    ObIArray<ObTabletLSPair> &pairs,
    ObIArray<ObTabletReplica::ScnStatus> &status_arr)
{
  int ret = OB_SUCCESS;

  const int64_t pair_cnt = pairs.count();
  if (OB_UNLIKELY(!is_valid_tenant_id(tenant_id) || pair_cnt < 1)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(pair_cnt));
  } else {
    ObSqlString sql;
    SMART_VAR(ObISQLClient::ReadResult, res) {
      ObMySQLResult *result = nullptr;
      if (OB_FAIL(sql.assign_fmt("SELECT distinct status FROM %s WHERE tenant_id = '%lu' AND (ls_id, tablet_id) "
          "IN (", OB_ALL_TABLET_META_TABLE_TNAME, tenant_id))) {
        LOG_WARN("fail to assign sql", KR(ret), K(tenant_id));
      } else {
        for (int64_t i = 0; (i < pair_cnt) && OB_SUCC(ret); ++i) {
          const ObTabletLSPair &pair = pairs.at(i);
          if (OB_UNLIKELY(!pair.is_valid())) {
            ret = OB_INVALID_ARGUMENT;
            LOG_WARN("invalid argument", KR(ret), K(tenant_id), K(i), K(pair));
          } else if (OB_FAIL(sql.append_fmt("(%ld, %ld)%s", pair.get_ls_id().id(),
              pair.get_tablet_id().id(), ((i == pair_cnt - 1) ? ")" : ", ")))) {
            LOG_WARN("fail to assign sql", KR(ret), K(i), K(tenant_id), K(pair));
          }
        }
      }

      const uint64_t meta_tenant_id = gen_meta_tenant_id(tenant_id);
      if (FAILEDx(GCTX.sql_proxy_->read(res, meta_tenant_id, sql.ptr()))) {
        LOG_WARN("fail to execute sql", KR(ret), K(tenant_id), K(meta_tenant_id), K(sql));
      } else if (OB_ISNULL(result = res.get_result())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fail to get mysql result", KR(ret), K(tenant_id), K(sql));
      } else {
        while (OB_SUCC(ret)) {
          int64_t tmp_status = 0;
          if (OB_FAIL(result->next())) {
            if (OB_ITER_END != ret) {
              LOG_WARN("fail to get next result", KR(ret), K(tenant_id), K(sql));
            }
          } else if (OB_FAIL(result->get_int("status", tmp_status))) {
            LOG_WARN("failed to get int", KR(ret), K(tmp_status));
          } else if (OB_UNLIKELY(!ObTabletReplica::is_status_valid((ObTabletReplica::ScnStatus)tmp_status))) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("status is invalid", KR(ret), K(tenant_id), K(tmp_status));
          } else if (OB_FAIL(status_arr.push_back(ObTabletReplica::ScnStatus(tmp_status)))) {
            LOG_WARN("fail to push back status", KR(ret), K(tenant_id), K(tmp_status));
          }
        } // end while loop

        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

} // end namespace share
} // end namespace oceanbase