/* Copyright 2016-2019 Codership Oy <http://www.codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#ifndef WSREP_TRANS_OBSERVER_H
#define WSREP_TRANS_OBSERVER_H

#include "my_dbug.h"
#include "service_wsrep.h"
#include "sql/binlog.h"
#include "wsrep_applier.h" /* wsrep_apply_error */
#include "wsrep_binlog.h"  /* register/deregister group commit */
#include "wsrep_thd.h"
#include "wsrep_xid.h"

class THD;

/*
   Return true if THD has active wsrep transaction.
 */
static inline bool wsrep_is_active(THD *thd) {
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none &&
          thd->wsrep_cs().transaction().active());
}

/*
  Return true if transaction is ordered.
 */
static inline bool wsrep_is_ordered(THD *thd) {
  return thd->wsrep_trx().ordered();
}

/*
  Return true if transaction has been BF aborted but has not been
  rolled back yet.

  It is required that the caller holds thd->LOCK_wsrep_thd.
*/
static inline bool wsrep_must_abort(THD *thd) {
  mysql_mutex_assert_owner(&thd->LOCK_wsrep_thd);
  return (thd->wsrep_trx().state() == wsrep::transaction::s_must_abort);
}

/*
  Return true if the transaction must be replayed.
 */
static inline bool wsrep_must_replay(THD *thd) {
  return (thd->wsrep_trx().state() == wsrep::transaction::s_must_replay);
}
/*
  Return true if transaction has not been committed.

  Note that we don't require thd->LOCK_wsrep_thd here. Calling this method
  makes sense only from codepaths which are past ordered_commit state
  and the wsrep transaction is immune to BF aborts at that point.
*/
static inline bool wsrep_not_committed(THD *thd) {
  return (thd->wsrep_trx().state() != wsrep::transaction::s_committed);
}

/*
  Return true if THD is either committing a transaction or statement
  is autocommit.
 */
static inline bool wsrep_is_real(THD *thd, bool all) {
  return (all || !thd->get_transaction()->is_active(Transaction_ctx::SESSION));
}

/*
  Check if a transaction has generated changes.
 */
static inline bool wsrep_has_changes(THD *thd) {
  return (thd->wsrep_trx().is_empty() == false);
}

/*
  Check if an active transaction has been BF aborted.
 */
static inline bool wsrep_is_bf_aborted(THD *thd) {
  return (thd->wsrep_trx().active() && thd->wsrep_trx().bf_aborted());
}

static inline int wsrep_check_pk(THD *thd) {
  if (!wsrep_certify_nonPK) {
    for (TABLE *table = thd->open_tables; table != NULL; table = table->next) {
      if (table->key_info == NULL || table->s->primary_key == MAX_KEY) {
        WSREP_DEBUG("No primary key found for table %s.%s", table->s->db.str,
                    table->s->table_name.str);
        wsrep_override_error(thd, ER_LOCK_DEADLOCK);
        return 1;
      }
    }
  }
  return 0;
}

static inline bool wsrep_streaming_enabled(THD *thd) {
  return (thd->wsrep_sr().fragment_size() > 0);
}

/*
  Return number of fragments succesfully certified for the
  current statement.
 */
static inline size_t wsrep_fragments_certified_for_stmt(THD *thd) {
  return thd->wsrep_trx().fragments_certified_for_statement();
}

static inline int wsrep_start_transaction(THD *thd, wsrep_trx_id_t trx_id) {
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none
              ? thd->wsrep_cs().start_transaction(wsrep::transaction_id(trx_id))
              : 0);
}

/**/
static inline int wsrep_start_trx_if_not_started(THD *thd) {
  int ret = 0;
  assert(thd->wsrep_next_trx_id() != WSREP_UNDEFINED_TRX_ID);
  assert(thd->wsrep_cs().mode() == Wsrep_client_state::m_local);
  if (thd->wsrep_trx().active() == false) {
    ret = wsrep_start_transaction(thd, thd->wsrep_next_trx_id());
  }
  return ret;
}

/*
  Called after each row operation.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_row(THD *thd, bool) {
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none &&
      wsrep_thd_is_local(thd)) {
    if (wsrep_check_pk(thd)) {
      return 1;
    } else if (wsrep_streaming_enabled(thd)) {
      return thd->wsrep_cs().after_row();
    }
  }
  return 0;
}

/*
  Helper method to determine whether commit time hooks
  should be run for the transaction.

  Commit hooks must be run in the following cases:
  - The transaction is local and has generated write set and is committing.
  - The transaction has been BF aborted
  - Is running in high priority mode and is ordered. This can be replayer,
    applier or storage access.
 */
static inline bool wsrep_run_commit_hook(THD *thd, bool all) {
  DBUG_TRACE;
  DBUG_PRINT("wsrep", ("Is_active: %d is_real %d has_changes %d is_applying %d "
                       "is_ordered: %d is_substatement: %d",
                       wsrep_is_active(thd), wsrep_is_real(thd, all),
                       wsrep_has_changes(thd), wsrep_thd_is_applying(thd),
                       wsrep_is_ordered(thd),
                       thd->is_operating_substatement_implicitly));
  /* Avoid running commit hooks if a sub-statement is being operated implicitly
   * within current transaction (if it is an internal transaction) */
  if (thd->is_operating_substatement_implicitly) {
    return false;
  }
  /* Is MST commit or autocommit? */
  bool ret = wsrep_is_active(thd) && wsrep_is_real(thd, all);
  /* Do not commit if we are aborting */
  ret = ret && (thd->wsrep_trx().state() != wsrep::transaction::s_aborting);

  /* Action below will log an empty group of GTID.
  This is done when the real action fails to generate any meaningful result on
  executing slave.
  Let's understand with an example:
  * Topology master <-> slave
  * Some action is performed on slave which put it out-of-sync from master.
  * Master then execute same action. Slave may choose to ignore error arising
    from execution of these actions using slave_skip_errors configuration but
    the GTID sequence increment still need to register on slave to keep it in
    sync with master. So a dummy trx of this form is created. Galera
    eco-system too will capture this dummy trx and will execute it for
    internal replication to keep GTID sequence consistent across
    the cluster. */
  if (ret && thd->wsrep_replicate_GTID) {
    return true;
  }

  if (ret && !(wsrep_has_changes(thd) || /* Has generated write set */
               /* Is high priority (replay, applier, storage) and the
                  transaction is scheduled for commit ordering */
               (wsrep_thd_is_applying(thd) && wsrep_is_ordered(thd)))) {
    mysql_mutex_lock(&thd->LOCK_wsrep_thd);
    DBUG_PRINT("wsrep",
               ("state: %s", wsrep::to_c_string(thd->wsrep_trx().state())));
    /* Transaction is local but has no changes, the commit hooks will
       be skipped and the wsrep transaction is terminated in
       wsrep_commit_empty() */
    if (thd->wsrep_trx().state() == wsrep::transaction::s_executing) {
      ret = false;
    }
    mysql_mutex_unlock(&thd->LOCK_wsrep_thd);
  }
  DBUG_PRINT("wsrep", ("return: %d", ret));
  return ret;
}

/*
  Called before the transaction is prepared.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_prepare(THD *thd, bool all) {
  DBUG_ENTER("wsrep_before_prepare");
  WSREP_DEBUG("wsrep_before_prepare: %d", wsrep_is_real(thd, all));
  int ret = 0;
  assert(wsrep_run_commit_hook(thd, all));

  /* applier too use this routine but before prepare doesn't invoke
  galera replication action. */
  if (!wsrep_thd_is_applying(thd)) {
    THD_STAGE_INFO(thd, stage_wsrep_replicating_commit);
    snprintf(thd->wsrep_info, sizeof(thd->wsrep_info),
             "wsrep: replicating and certifying write set(%lld)",
             (long long)wsrep_thd_trx_seqno(thd));
    WSREP_DEBUG("%s", thd->wsrep_info);
    thd_proc_info(thd, thd->wsrep_info);
  } else {
    THD_STAGE_INFO(thd, stage_wsrep_replicating_commit);
    snprintf(thd->wsrep_info, sizeof(thd->wsrep_info),
             "wsrep: preparing to commit write set(%lld)",
             (long long)wsrep_thd_trx_seqno(thd));
    WSREP_DEBUG("%s", thd->wsrep_info);
    thd_proc_info(thd, thd->wsrep_info);
  }

  if ((ret = thd->wsrep_cs().before_prepare()) == 0) {
    assert(!thd->wsrep_trx().ws_meta().gtid().is_undefined());
    thd->wsrep_xid.reset();
#if 0
    wsrep_xid_init(&thd->wsrep_xid, thd->wsrep_trx().ws_meta().gtid());
#endif
    wsrep_xid_init(thd->get_transaction()->xid_state()->get_xid(),
                   thd->wsrep_trx().ws_meta().gtid());

    if (!wsrep_thd_is_applying(thd)) {
      THD_STAGE_INFO(thd, stage_wsrep_write_set_replicated);
      snprintf(thd->wsrep_info, sizeof(thd->wsrep_info) - 1,
               "wsrep: write set replicated and certified (%lld)",
               (long long)wsrep_thd_trx_seqno(thd));
      WSREP_DEBUG("%s", thd->wsrep_info);
      thd_proc_info(thd, thd->wsrep_info);
    }
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been prepared.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_prepare(THD *thd, bool all) {
  DBUG_ENTER("wsrep_after_prepare");
  WSREP_DEBUG("wsrep_after_prepare: %d", wsrep_is_real(thd, all));
  assert(wsrep_run_commit_hook(thd, all));
  int ret = thd->wsrep_cs().after_prepare();
  assert(ret == 0 || thd->wsrep_cs().current_error() ||
              thd->wsrep_cs().transaction().state() ==
                  wsrep::transaction::s_must_replay);
  DBUG_RETURN(ret);
}

/*
  Called before the transaction is committed.

  This function must be called from both client and
  applier contexts before commit.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_commit(THD *thd, bool all) {
  DBUG_ENTER("wsrep_before_commit");
  WSREP_DEBUG("wsrep_before_commit: %d, %lld", wsrep_is_real(thd, all),
              (long long)wsrep_thd_trx_seqno(thd));
  int ret = 0;
  assert(wsrep_run_commit_hook(thd, all));
  if ((ret = thd->wsrep_cs().before_commit()) == 0) {
    assert(!thd->wsrep_trx().ws_meta().gtid().is_undefined());
#if 0
    wsrep_xid_init(&thd->wsrep_xid, thd->wsrep_trx().ws_meta().gtid());
    wsrep_xid_init(thd->get_transaction()->xid_state()->get_xid(),
                   thd->wsrep_cs().toi_meta().gtid());
#endif

    if (thd->run_wsrep_commit_hooks) {
      /* If the transaction is running as one-phase then register
      THD in wsrep group commit queue at this stage.
      If the transaction is running as 2-phase then THD is registered
      in ordered_commit to ensure thd registration order is same as
      mysql group commit queue order. */
      wsrep_register_for_group_commit(thd);
    }

    /* If the transaction doesn't go through prepare phase */
    wsrep_xid_init(thd->get_transaction()->xid_state()->get_xid(),
                   thd->wsrep_trx().ws_meta().gtid());
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been ordered for commit.

  This function must be called from both client and
  applier contexts after the commit has been ordered.

  @param thd Pointer to THD
  @param all
  @param err Error buffer in case of applying error

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_ordered_commit(THD *thd, bool all) {
  /* Interim Commit Optimization can be used only if log_slave_updates is
  ON that ensures all slave thread (including pxc replication threads)
  binlogs the events there-by following group commit protocol.
  Even if one thread doesn't follow group commit protocol interim
  commit optimization will not work.
  Interim commit optimization rely on ordered commit of MySQL.
  If this feature is turned-off then skip this optimization. */
  if (!opt_log_replica_updates || !opt_binlog_order_commits) {
    return 0;
  }

  DBUG_ENTER("wsrep_ordered_commit");
  WSREP_DEBUG("wsrep_ordered_commit: %d", wsrep_is_real(thd, all));
  assert(wsrep_run_commit_hook(thd, all));

  /* Register thread handler in wsrep group commit queue.
  Note: thread handler executing 2 phase commit transaction is registered
  as part of ordered_commit (and not part of before_commit) as wsrep group
  commit sequence should be same as mysql group commit queue sequence. */
  wsrep_register_for_group_commit(thd);

  DBUG_RETURN(thd->wsrep_cs().ordered_commit());
}

/*
  Called after the transaction has been committed.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_commit(THD *thd, bool all) {
  DBUG_ENTER("wsrep_after_commit");
  WSREP_DEBUG("wsrep_after_commit: %d, %d, %lld, %d", wsrep_is_real(thd, all),
              wsrep_is_active(thd), (long long)wsrep_thd_trx_seqno(thd),
              wsrep_has_changes(thd));
  assert(wsrep_run_commit_hook(thd, all));

  if (thd->wsrep_enforce_group_commit) {
    /* Ideally, for one-phase (with binlog=off) or two-phase (with binlog=on)
    this step would be executed when transaction commits in InnoDB.
    If galera node is acting as async slave and replicated action from async
    master result in empty changes on slave (slave directly applied the said
    changes and has skipped error through skip-slave-error configuration) it
    can result in said situation. In this case slave protocol directly commits
    gtid through gtid_end_transaction that invokes ordered_commit causing
    thread handler to register in wsrep group commit queue but since storage
    engine commit is not done it would fail to unregister the said thread
    handler as part of storage engine commit. Handle unregistration here. */
    wsrep_wait_for_turn_in_group_commit(thd);
    wsrep_unregister_from_group_commit(thd);
  }

  assert(!thd->wsrep_enforce_group_commit);

  int ret = 0;
  if (thd->wsrep_trx().state() == wsrep::transaction::s_committing) {
    ret = thd->wsrep_cs().ordered_commit();
  }
  thd->wsrep_xid.reset();
  thd->get_transaction()->xid_state()->get_xid()->reset();
  DBUG_RETURN(ret || thd->wsrep_cs().after_commit());
}

/*
  Called before the transaction is rolled back.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_before_rollback(THD *thd, bool all) {
  DBUG_ENTER("wsrep_before_rollback");
  // WSREP_DEBUG("wsrep_before_rollback %u", thd->thread_id());
  int ret = 0;
  if (wsrep_is_active(thd)) {
    if (!all && thd->in_active_multi_stmt_transaction() &&
        thd->wsrep_trx().is_streaming() && !wsrep_stmt_rollback_is_safe(thd)) {
      /* Non-safe statement rollback during SR multi statement
         transasction. Self abort the transaction, the actual rollback
         and error handling will be done in after statement phase. */
      wsrep_thd_self_abort(thd);
      ret = 0;
    } else if (wsrep_is_real(thd, all) &&
               thd->wsrep_trx().state() != wsrep::transaction::s_aborted) {
      /* Real transaction rolling back and wsrep abort not completed
         yet */
      /* Reset XID so that it does not trigger writing serialization
         history in InnoDB. This needs to be avoided because rollback
         may happen out of order and replay may follow. */
      thd->wsrep_xid.reset();
      thd->get_transaction()->xid_state()->get_xid()->reset();
      ret = thd->wsrep_cs().before_rollback();
    }
  }
  DBUG_RETURN(ret);
}

/*
  Called after the transaction has been rolled back.

  Return zero on succes, non-zero on failure.
 */
static inline int wsrep_after_rollback(THD *thd, bool all) {
  DBUG_ENTER("wsrep_after_rollback");
  // WSREP_DEBUG("wsrep_after_rollback %u", thd->thread_id());
  if (!wsrep_is_real(thd, all)) {
    WSREP_DEBUG("wsrep_after_rollback stmt transaction rolled back");
    thd->wsrep_stmt_transaction_rolled_back = true;
  }
  /* resetting aborter thread ID after full rollback */
  if (wsrep_is_real(thd, all)) thd->wsrep_aborter = 0;
  DBUG_RETURN(
      (wsrep_is_real(thd, all) && wsrep_is_active(thd) &&
       thd->wsrep_cs().transaction().state() != wsrep::transaction::s_aborted)
          ? thd->wsrep_cs().after_rollback()
          : 0);
}

static inline int wsrep_before_statement(THD *thd) {
  // WSREP_DEBUG("wsrep_before_statement %u", thd->thread_id());
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none
              ? thd->wsrep_cs().before_statement()
              : 0);
}

static inline int wsrep_after_statement(THD *thd) {
  DBUG_ENTER("wsrep_after_statement");
  // WSREP_DEBUG("wsrep_after_statement %u", thd->thread_id());
  DBUG_RETURN(thd->wsrep_cs().state() != wsrep::client_state::s_none
                  ? thd->wsrep_cs().after_statement()
                  : 0);
}

static inline void wsrep_after_apply(THD *thd) {
  assert(wsrep_thd_is_applying(thd));
  WSREP_DEBUG("wsrep_after_apply %u", thd->thread_id());
  thd->wsrep_cs().after_applying();
}

static inline void wsrep_open(THD *thd) {
  DBUG_ENTER("wsrep_open");
  if (wsrep_on(thd)) {
    thd->wsrep_cs().open(wsrep::client_id(thd->thread_id()));
    thd->wsrep_cs().debug_log_level(wsrep_debug);
    if (!thd->wsrep_applier && thd->variables.wsrep_trx_fragment_size) {
      thd->wsrep_cs().enable_streaming(
          wsrep_fragment_unit(thd->variables.wsrep_trx_fragment_unit),
          size_t(thd->variables.wsrep_trx_fragment_size));
    }
  }
  DBUG_VOID_RETURN;
}

static inline void wsrep_close(THD *thd) {
  DBUG_ENTER("wsrep_close");
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none) {
    thd->wsrep_cs().close();
  }
  DBUG_VOID_RETURN;
}

static inline void wsrep_wait_rollback_complete_and_acquire_ownership(
    THD *thd) {
  DBUG_ENTER("wsrep_wait_rollback_complete_and_acquire_ownership");
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none) {
    thd->wsrep_cs().wait_rollback_complete_and_acquire_ownership();
  }
  DBUG_VOID_RETURN;
}

static inline int wsrep_before_command(THD *thd) {
  return (thd->wsrep_cs().state() != wsrep::client_state::s_none
              ? thd->wsrep_cs().before_command()
              : 0);
}
/*
  Called after each command.

  Return zero on success, non-zero on failure.
*/
static inline void wsrep_after_command_before_result(THD *thd) {
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none) {
    thd->wsrep_cs().after_command_before_result();
  }
}

static inline void wsrep_after_command_after_result(THD *thd) {
  if (thd->wsrep_cs().state() != wsrep::client_state::s_none) {
    thd->wsrep_cs().after_command_after_result();
  }
}

static inline void wsrep_after_command_ignore_result(THD *thd) {
  wsrep_after_command_before_result(thd);
  assert(!thd->wsrep_cs().current_error());
  wsrep_after_command_after_result(thd);
}

static inline enum wsrep::client_error wsrep_current_error(THD *thd) {
  return thd->wsrep_cs().current_error();
}

static inline enum wsrep::provider::status wsrep_current_error_status(
    THD *thd) {
  return thd->wsrep_cs().current_error_status();
}

/*
  Commit an empty transaction.

  If the transaction is real and the wsrep transaction is still active,
  the transaction did not generate any rows or keys and is committed
  as empty. Here the wsrep transaction is rolled back and after statement
  step is performed to leave the wsrep transaction in the state as it
  never existed.
*/
static inline void wsrep_commit_empty(THD *thd, bool all) {
  DBUG_ENTER("wsrep_commit_empty");
  if (wsrep_is_real(thd, all) && wsrep_thd_is_local(thd) &&
      thd->wsrep_trx().active() &&
      thd->wsrep_trx().state() != wsrep::transaction::s_committed) {
    /* @todo CTAS with STATEMENT binlog format and empty result set
       seems to be committing empty. Figure out why and try to fix
       elsewhere. */
    assert(!wsrep_has_changes(thd) ||
                thd->wsrep_stmt_transaction_rolled_back ||
                (thd->lex->sql_command == SQLCOM_CREATE_TABLE &&
                 !thd->is_current_stmt_binlog_format_row()) ||
                thd->wsrep_post_insert_error);
    bool have_error = wsrep_current_error(thd);
    int ret = wsrep_before_rollback(thd, all) ||
              wsrep_after_rollback(thd, all) || wsrep_after_statement(thd);
    /* The committing transaction was empty but it held some locks and
       got BF aborted. As there were no certified changes in the
       data, we ignore the deadlock error and rely on error reporting
       by storage engine/server. */
    if (!ret && !have_error && wsrep_current_error(thd)) {
      assert(wsrep_current_error(thd) == wsrep::e_deadlock_error);
      thd->wsrep_cs().reset_error();
    }
    if (ret) {
      WSREP_DEBUG("wsrep_commit_empty failed: %d", wsrep_current_error(thd));
    }
  }
  DBUG_VOID_RETURN;
}

#endif /* WSREP_TRANS_OBSERVER */
