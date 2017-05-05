// Copyright (c) YugaByte, Inc.

#include "yb/client/async_rpc.h"
#include "yb/client/batcher.h"
#include "yb/client/client.h"
#include "yb/client/client-internal.h"
#include "yb/client/in_flight_op.h"
#include "yb/client/meta_cache.h"
#include "yb/client/yb_op-internal.h"
#include "yb/util/cast.h"
#include "yb/util/debug-util.h"
#include "yb/util/logging.h"
#include "yb/common/wire_protocol.h"
#include "yb/common/row_operations.h"

// TODO: do we need word Redis in following two metrics? ReadRpc and WriteRpc objects emitting
// these metrics are used not only in Redis service.
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_write, "yb.client.Write RPC Time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the WriteRpc ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_read, "yb.client.Read RPC Time",
    yb::MetricUnit::kMicroseconds, "Microseconds spent in the ReadRpc ", 60000000LU, 2);
METRIC_DEFINE_histogram(
    server, handler_latency_yb_client_time_to_send,
    "Time taken for a Write/Read rpc to be sent to the server", yb::MetricUnit::kMicroseconds,
    "Microseconds spent before sending the request to the server", 60000000LU, 2);
DECLARE_bool(rpc_dump_all_traces);
DECLARE_bool(collect_end_to_end_traces);

namespace yb {

using std::shared_ptr;
using rpc::ErrorStatusPB;
using rpc::Messenger;
using rpc::Rpc;
using rpc::RpcController;
using tserver::WriteRequestPB;
using tserver::WriteResponsePB;
using tserver::WriteResponsePB_PerRowErrorPB;
using strings::Substitute;

namespace client {

namespace internal {

bool IsTracingEnabled() {
  return FLAGS_collect_end_to_end_traces;
}

AsyncRpcMetrics::AsyncRpcMetrics(const scoped_refptr<yb::MetricEntity>& entity)
    : write_rpc_time(METRIC_handler_latency_yb_client_write.Instantiate(entity)),
      read_rpc_time(METRIC_handler_latency_yb_client_read.Instantiate(entity)),
      time_to_send(METRIC_handler_latency_yb_client_time_to_send.Instantiate(entity)) {
}

AsyncRpc::AsyncRpc(
    const scoped_refptr<Batcher>& batcher, RemoteTablet* const tablet, vector<InFlightOp*> ops,
    const MonoTime& deadline, const shared_ptr<Messenger>& messenger,
    const shared_ptr<AsyncRpcMetrics>& async_rpc_metrics)
    : Rpc(deadline, messenger),
      batcher_(batcher),
      trace_(new Trace),
      tablet_(tablet),
      current_ts_(NULL),
      ops_(std::move(ops)),
      start_(MonoTime::Now(MonoTime::FINE)),
      async_rpc_metrics_(async_rpc_metrics) {
  if (Trace::CurrentTrace()) {
    Trace::CurrentTrace()->AddChildTrace(trace_.get());
  }
}

AsyncRpc::~AsyncRpc() {
  STLDeleteElements(&ops_);
  if (PREDICT_FALSE(FLAGS_rpc_dump_all_traces)) {
    LOG(INFO) << ToString() << " took "
              << MonoTime::Now(MonoTime::FINE).GetDeltaSince(start_).ToMicroseconds()
              << "us. Trace:";
    trace_->Dump(&LOG(INFO), true);
  }
}

void AsyncRpc::SendRpc() {
  TRACE_TO(trace_, "SendRpc() called.");
  // Choose a destination TS according to the following algorithm:
  // 1. Select the leader, provided:
  //    a. One exists, and
  //    b. It hasn't failed, and
  //    c. It isn't currently marked as a follower.
  // 2. If there's no good leader select another replica, provided:
  //    a. It hasn't failed, and
  //    b. It hasn't rejected our write due to being a follower.
  // 3. Preemptively mark the replica we selected in step 2 as "leader" in the
  //    meta cache, so that our selection remains sticky until the next Master
  //    metadata refresh.
  // 4. If we're out of appropriate replicas, force a lookup to the master
  //    to fetch new consensus configuration information.
  // 5. When the lookup finishes, forget which replicas were followers and
  //    retry the write (i.e. goto 1).
  // 6. If we issue the write and it fails because the destination was a
  //    follower, remember that fact and retry the write (i.e. goto 1).
  // 7. Repeat steps 1-6 until the write succeeds, fails for other reasons,
  //    or the write's deadline expires.
  current_ts_ = tablet_->LeaderTServer();
  if (current_ts_ && ContainsKey(followers_, current_ts_)) {
    VLOG(2) << "Tablet " << tablet_->tablet_id() << ": We have a follower for a leader: "
            << current_ts_->ToString();

    // Mark the node as a follower in the cache so that on the next go-round,
    // LeaderTServer() will not return it as a leader unless a full metadata
    // refresh has occurred. This also avoids LookupTabletByKey() going into
    // "fast path" mode and not actually performing a metadata refresh from the
    // Master when it needs to.
    tablet_->MarkTServerAsFollower(current_ts_);
    current_ts_ = NULL;
  }
  if (!current_ts_) {
    // Try to "guess" the next leader.
    vector<RemoteTabletServer*> replicas;
    tablet_->GetRemoteTabletServers(&replicas);
    for (RemoteTabletServer* ts : replicas) {
      if (!ContainsKey(followers_, ts)) {
        current_ts_ = ts;
        break;
      }
    }
    if (current_ts_) {
      // Mark this next replica "preemptively" as the leader in the meta cache,
      // so we go to it first on the next write if writing was successful.
      VLOG(1) << "Tablet " << tablet_->tablet_id() << ": Previous leader failed. "
              << "Preemptively marking tserver " << current_ts_->ToString()
              << " as leader in the meta cache.";
      tablet_->MarkTServerAsLeader(current_ts_);
    }
  }

  // If we've tried all replicas, force a lookup to the master to find the
  // new leader. This relies on some properties of LookupTabletByKey():
  // 1. The fast path only works when there's a non-failed leader (which we
  //    know is untrue here).
  // 2. The slow path always fetches consensus configuration information and
  //    updates the looked-up tablet.
  // Put another way, we don't care about the lookup results at all; we're
  // just using it to fetch the latest consensus configuration information.
  //
  // TODO: When we support tablet splits, we should let the lookup shift
  // the write to another tablet (i.e. if it's since been split).
  if (!current_ts_) {
    batcher_->client_->data_->meta_cache_->LookupTabletByKey(table(),
                                                             tablet_->partition()
                                                                 .partition_key_start(),
                                                             retrier().deadline(),
                                                             NULL,
                                                             Bind(&AsyncRpc::LookupTabletCb,
                                                                  Unretained(this)));
    return;
  }

  // Make sure we have a working proxy before sending out the RPC.
  current_ts_->InitProxy(batcher_->client_,
                         Bind(&AsyncRpc::InitTSProxyCb, Unretained(this)));
}

string AsyncRpc::ToString() const {
  return Substitute("Write(tablet: $0, num_ops: $1, num_attempts: $2)",
                    tablet_->tablet_id(), ops_.size(), num_attempts());
}

const YBTable* AsyncRpc::table() const {
  // All of the ops for a given tablet obviously correspond to the same table,
  // so we'll just grab the table from the first.
  return ops_[0]->yb_op->table();
}

void AsyncRpc::LookupTabletCb(const Status& status) {
  TRACE_TO(trace_, "LookupTabletCb($0)", status.ToString(false));
  // We should retry the RPC regardless of the outcome of the lookup, as
  // leader election doesn't depend on the existence of a master at all.
  //
  // Retry() imposes a slight delay, which is desirable in a lookup loop,
  // but unnecessary the first time through. Seeing as leader failures are
  // rare, perhaps this doesn't matter.
  followers_.clear();
  mutable_retrier()->DelayedRetry(this, status);
}

void AsyncRpc::FailToNewReplica(const Status& reason) {
  VLOG(1) << "Failing " << ToString() << " to a new replica: "
          << reason.ToString();
  bool found = tablet_->MarkReplicaFailed(current_ts_, reason);
  DCHECK(found) << "Tablet " << tablet_->tablet_id() << ": Unable to mark replica "
                << current_ts_->ToString()
                << " as failed. Replicas: " << tablet_->ReplicasAsString();

  mutable_retrier()->DelayedRetry(this, reason);
}

void AsyncRpc::SendRpcCb(const Status& status) {
  TRACE_TO(trace_, "SendRpcCb($0)", status.ToString(false));
  ADOPT_TRACE(trace_.get());
  // Prefer early failures over controller failures.
  Status new_status = status;
  if (new_status.ok() && mutable_retrier()->HandleResponse(this, &new_status)) {
    return;
  }

  // Failover to a replica in the event of any network failure.
  //
  // TODO: This is probably too harsh; some network failures should be
  // retried on the current replica.
  if (new_status.IsNetworkError()) {
    FailToNewReplica(new_status);
    return;
  }

  // Prefer controller failures over response failures.
  Status resp_error_status = response_error_status();
  if (new_status.ok() && !resp_error_status.ok()) {
    new_status = resp_error_status;
  }

  // Oops, we failed over to a replica that wasn't a LEADER. Unlikely as
  // we're using consensus configuration information from the master, but still possible
  // (e.g. leader restarted and became a FOLLOWER). Try again.
  //
  // TODO: IllegalState is obviously way too broad an error category for
  // this case.
  if (new_status.IsIllegalState() || new_status.IsAborted()) {
    followers_.insert(current_ts_);
    mutable_retrier()->DelayedRetry(this, new_status);
    return;
  }

  if (!new_status.ok()) {
    string current_ts_string;
    if (current_ts_) {
      current_ts_string = Substitute("on tablet server $0", current_ts_->ToString());
    } else {
      current_ts_string = "(no tablet server available)";
    }
    const string error_message = new_status.message().ToString();
    new_status = new_status.CloneAndPrepend(
        Substitute("Failed to write batch of $0 ops to tablet $1 "
                       "$2 after $3 attempt(s)",
                   ops_.size(), tablet_->tablet_id(),
                   current_ts_string, num_attempts()));
    LOG(WARNING) << new_status.ToString();
    MarkOpsAsFailed(error_message);
  }
  ProcessResponseFromTserver(new_status);
  batcher_->RemoveInFlightOps(ops_);
  batcher_->CheckForFinishedFlush();
  delete this;
}

void AsyncRpc::InitTSProxyCb(const Status& status) {
  TRACE_TO(trace_, "InitTSProxyCb($0)", status.ToString(false));
  // Fail to a replica in the event of a DNS resolution failure.
  if (!status.ok()) {
    FailToNewReplica(status);
    return;
  }

  VLOG(2) << "Tablet " << tablet_->tablet_id() << ": Writing batch to replica "
          << current_ts_->ToString();

  MonoTime end_time = MonoTime::Now(MonoTime::FINE);
  if (async_rpc_metrics_)
    async_rpc_metrics_->time_to_send->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
  SendRpcToTserver();
}

void AsyncRpc::MarkOpsAsFailed(const string& error_message) {
  for (auto op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        RedisResponsePB* resp = down_cast<YBRedisReadOp*>(yb_op)->mutable_response();
        resp->Clear();
        resp->set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
        resp->set_error_message(error_message);
        break;
      }
      case YBOperation::Type::REDIS_WRITE: {
        RedisResponsePB* resp = down_cast<YBRedisWriteOp *>(yb_op)->mutable_response();
        resp->Clear();
        resp->set_code(RedisResponsePB_RedisStatusCode_SERVER_ERROR);
        resp->set_error_message(error_message);
        break;
      }
      case YBOperation::Type::YQL_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::YQL_WRITE: {
        YQLResponsePB* resp = down_cast<YBqlOp*>(yb_op)->mutable_response();
        resp->Clear();
        resp->set_status(YQLResponsePB::YQL_STATUS_RUNTIME_ERROR);
        resp->set_error_message(error_message);
        break;
      }
      case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
      case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::DELETE:
        break;
      default:
        LOG(FATAL) << "Unsupported operation " << yb_op->type();
        break;
    }
  }
}

WriteRpc::WriteRpc(const scoped_refptr<Batcher>& batcher,
                   RemoteTablet* const tablet,
                   vector<InFlightOp*> ops,
                   const MonoTime& deadline,
                   const shared_ptr<Messenger>& messenger,
                   const std::shared_ptr<AsyncRpcMetrics>& async_rpc_metrics)
    : AsyncRpc(batcher, tablet, ops, deadline, messenger, async_rpc_metrics) {
  TRACE_TO(trace_, "WriteRpc initiated to $0", tablet->tablet_id());
  const Schema* schema = table()->schema().schema_;

  req_.set_tablet_id(tablet->tablet_id());
  req_.set_include_trace(IsTracingEnabled());
  switch (batcher->external_consistency_mode()) {
    case yb::client::YBSession::CLIENT_PROPAGATED:
      req_.set_external_consistency_mode(yb::CLIENT_PROPAGATED);
      break;
    case yb::client::YBSession::COMMIT_WAIT:
      req_.set_external_consistency_mode(yb::COMMIT_WAIT);
      break;
    default:
      LOG(FATAL) << "Unsupported consistency mode: " << batcher->external_consistency_mode();

  }

  RowOperationsPB* requested = req_.mutable_row_operations();

  // Add the rows
  int ctr = 0;
  RowOperationsPBEncoder enc(requested);
  for (InFlightOp* op : ops_) {
#ifndef NDEBUG
    const Partition& partition = op->tablet->partition();
    const PartitionSchema& partition_schema = table()->partition_schema();
    const YBPartialRow& row = op->yb_op->row();

    bool partition_contains_row;
    CHECK(partition_schema.PartitionContainsRow(partition, row, &partition_contains_row).ok());
    CHECK(partition_contains_row)
    << "Row " << partition_schema.RowDebugString(row)
    << "not in partition " << partition_schema.PartitionDebugString(partition, *schema);
#endif
    switch (op->yb_op->type()) {
      case YBOperation::Type::REDIS_WRITE: {
        CHECK_EQ(table()->table_type(), YBTableType::REDIS_TABLE_TYPE);
        RedisWriteRequestPB* redis_req = req_.mutable_redis_write_batch()->Add();
        // We are copying the redis request for now. In future it may be prevented.
        *redis_req = down_cast<YBRedisWriteOp*>(op->yb_op.get())->request();
        break;
      }
      case YBOperation::Type::YQL_WRITE: {
        CHECK_EQ(table()->table_type(), YBTableType::YQL_TABLE_TYPE);
        YQLWriteRequestPB* yql_req = req_.mutable_yql_write_batch()->Add();
        // We are copying the YQL request for now. In future it may be prevented.
        *yql_req = down_cast<YBqlWriteOp*>(op->yb_op.get())->request();
        break;
      }
      case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
      case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::DELETE: {
        CHECK_NE(table()->table_type(), YBTableType::REDIS_TABLE_TYPE)
            << "unsupported table type " << table()->table_type() << " for insert/update/delete";
        enc.Add(ToInternalWriteType(op->yb_op->type()), op->yb_op->row());
        if (!req_.has_schema()) {
          // Only in the Kudu case, we still need the schema as part of every WriteRequest.
          CHECK_OK(SchemaToPB(*schema, req_.mutable_schema(),
              SCHEMA_PB_WITHOUT_STORAGE_ATTRIBUTES | SCHEMA_PB_WITHOUT_IDS));
        }
        break;
      }
      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::YQL_READ:
        LOG(FATAL) << "Not a write operation " << op->yb_op->type();
        break;
      default:
        LOG(FATAL) << "Unsupported write operation " << op->yb_op->type();
        break;
    }

    // Set the state now, even though we haven't yet sent it -- at this point
    // there is no return, and we're definitely going to send it. If we waited
    // until after we sent it, the RPC callback could fire before we got a chance
    // to change its state to 'sent'.
    op->state = InFlightOp::kRequestSent;
    VLOG(4) << ++ctr << ". Encoded row " << op->yb_op->ToString();
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Created batch for " << tablet->tablet_id() << ":\n"
            << req_.ShortDebugString();
  }
}

WriteRpc::~WriteRpc() {
  MonoTime end_time = MonoTime::Now(MonoTime::FINE);
  if (async_rpc_metrics_)
    async_rpc_metrics_->write_rpc_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
}

void WriteRpc::SendRpcToTserver() {
  auto trace = trace_; // It is possible that we receive reply before returning from WriteAsync.
                       // Since send happens before we return from WriteAsync.
                       // So under heavy load it is possible that our request is handled and
                       // reply is received before WriteAsync returned.
  TRACE_TO(trace, "SendRpcToTserver");
  ADOPT_TRACE(trace.get());
  current_ts_->proxy()->WriteAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&WriteRpc::SendRpcCb, this, Status::OK()));
  TRACE_TO(trace, "RpcDispatched Asynchronously");
}

Status WriteRpc::response_error_status() {
  if (!resp_.has_error()) return Status::OK();
  return StatusFromPB(resp_.error().status());
}

void WriteRpc::ProcessResponseFromTserver(Status status) {
  TRACE_TO(trace_, "ProcessResponseFromTserver($0)", status.ToString(false));
  if (resp_.has_trace_buffer()) {
    TRACE_TO(trace_, "Received from server: $0", resp_.trace_buffer());
  }
  batcher_->ProcessWriteResponse(*this, status);
  if (!status.ok()) return;
  if (resp_.has_error()) {
    LOG(WARNING) << "Write Rpc to tablet server has error:"
                 << resp_.error().DebugString()
                 << ". Requests not processed.";
    // If there is an error at the Rpc itself,
    // there should be no individual responses. All of them need to be
    // marked as failed.
    MarkOpsAsFailed(resp_.error().status().message());
    return;
  }
  size_t redis_idx = 0;
  size_t yql_idx = 0;
  // Retrieve Redis and YQL responses and make sure we received all the responses back.
  for (InFlightOp* op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_WRITE: {
        if (redis_idx >= resp_.redis_response_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        *(down_cast<YBRedisWriteOp*>(yb_op)->mutable_response()) =
            std::move(resp_.redis_response_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::YQL_WRITE: {
        if (yql_idx >= resp_.yql_response_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        *(down_cast<YBqlWriteOp*>(yb_op)->mutable_response()) =
            std::move(resp_.yql_response_batch(yql_idx));
        const auto& yql_response = down_cast<YBqlWriteOp*>(yb_op)->response();
        if (yql_response.has_rows_data_sidecar()) {
          Slice rows_data;
          CHECK_OK(retrier().controller().GetSidecar(
              yql_response.rows_data_sidecar(), &rows_data));
          down_cast<YBqlWriteOp*>(yb_op)->mutable_rows_data()->assign(
              util::to_char_ptr(rows_data.data()), rows_data.size());
        }
        yql_idx++;
        break;
      }

      case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
      case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::DELETE:
        break; // these writes have no separate responses

      case YBOperation::Type::REDIS_READ: FALLTHROUGH_INTENDED;
      case YBOperation::Type::YQL_READ:
        LOG(FATAL) << "Not a write operation " << op->yb_op->type();
        break;
    }
  }

  if (redis_idx != resp_.redis_response_batch().size() ||
      yql_idx != resp_.yql_response_batch().size()) {
    LOG(ERROR) << Substitute("Write response count mismatch: "
                             "$0 Redis requests sent, $1 responses received. "
                             "$2 YQL requests sent, $3 responses received.",
                             redis_idx, resp_.redis_response_batch().size(),
                             yql_idx, resp_.yql_response_batch().size());
    batcher_->AddOpCountMismatchError();
    MarkOpsAsFailed("Write response count mismatch");
  }
}

ReadRpc::ReadRpc(
    const scoped_refptr<Batcher>& batcher, RemoteTablet* const tablet, vector<InFlightOp*> ops,
    const MonoTime& deadline, const shared_ptr<Messenger>& messenger,
    const std::shared_ptr<AsyncRpcMetrics>& async_rpc_metrics)
    : AsyncRpc(batcher, tablet, ops, deadline, messenger, async_rpc_metrics) {
  TRACE_TO(trace_, "ReadRpc initiated to $0", tablet->tablet_id());
  req_.set_tablet_id(tablet->tablet_id());
  req_.set_include_trace(IsTracingEnabled());
  int ctr = 0;
  for (InFlightOp* op : ops_) {
    switch (op->yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        CHECK_EQ(table()->table_type(), YBTableType::REDIS_TABLE_TYPE);
        RedisReadRequestPB* redis_req = req_.mutable_redis_batch()->Add();
        *redis_req = down_cast<YBRedisReadOp*>(op->yb_op.get())->request();
        break;
      }
      case YBOperation::Type::YQL_READ: {
        CHECK_EQ(table()->table_type(), YBTableType::YQL_TABLE_TYPE);
        YQLReadRequestPB* yql_req = req_.mutable_yql_batch()->Add();
        *yql_req = down_cast<YBqlReadOp*>(op->yb_op.get())->request();
        break;
      }
      case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
      case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::YQL_WRITE:
        LOG(FATAL) << "Not a read operation " << op->yb_op->type();
        break;
      default:
        LOG(FATAL) << "Unsupported read operation " << op->yb_op->type();
        break;
    }
    op->state = InFlightOp::kRequestSent;
    VLOG(4) << ++ctr << ". Encoded row " << op->yb_op->ToString();
  }

  if (VLOG_IS_ON(3)) {
    VLOG(3) << "Created batch for " << tablet->tablet_id() << ":\n" << req_.ShortDebugString();
  }
}

ReadRpc::~ReadRpc() {
  MonoTime end_time = MonoTime::Now(MonoTime::FINE);
  if (async_rpc_metrics_)
    async_rpc_metrics_->read_rpc_time->Increment(end_time.GetDeltaSince(start_).ToMicroseconds());
}

void ReadRpc::SendRpcToTserver() {
  auto trace = trace_; // It is possible that we receive reply before returning from ReadAsync.
                       // Detailed explanation in WriteRpc::SendRpcToTserver.
  TRACE_TO(trace, "SendRpcToTserver");
  ADOPT_TRACE(trace.get());
  current_ts_->proxy()->ReadAsync(
      req_, &resp_, mutable_retrier()->mutable_controller(),
      std::bind(&ReadRpc::SendRpcCb, this, Status::OK()));
  TRACE_TO(trace, "RpcDispatched Asynchronously");
}

Status ReadRpc::response_error_status() {
  if (!resp_.has_error()) return Status::OK();
  return StatusFromPB(resp_.error().status());
}

void ReadRpc::ProcessResponseFromTserver(Status status) {
  TRACE_TO(trace_, "ProcessResponseFromTserver($0)", status.ToString(false));
  if (resp_.has_trace_buffer()) {
    TRACE_TO(trace_, "Received from server: $0", resp_.trace_buffer());
  }
  batcher_->ProcessReadResponse(*this, status);
  if (!status.ok()) return;
  if (resp_.has_error()) {
    LOG(WARNING) << "Read Rpc to tablet server has error:"
                 << resp_.error().DebugString()
                 << ". Requests not processed.";
    // If there is an error at the Rpc itself,
    // there should be no individual responses. All of them need to be
    // marked as failed.
    MarkOpsAsFailed(resp_.error().status().message());
    return;
  }
  // Retrieve Redis and YQL responses and make sure we received all the responses back.
  size_t redis_idx = 0;
  size_t yql_idx = 0;
  for (InFlightOp* op : ops_) {
    YBOperation* yb_op = op->yb_op.get();
    switch (yb_op->type()) {
      case YBOperation::Type::REDIS_READ: {
        if (redis_idx >= resp_.redis_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        *(down_cast<YBRedisReadOp*>(yb_op)->mutable_response()) =
            std::move(resp_.redis_batch(redis_idx));
        redis_idx++;
        break;
      }
      case YBOperation::Type::YQL_READ: {
        if (yql_idx >= resp_.yql_batch().size()) {
          batcher_->AddOpCountMismatchError();
          return;
        }
        *(down_cast<YBqlReadOp*>(yb_op)->mutable_response()) =
            std::move(resp_.yql_batch(yql_idx));
        const auto& yql_response = down_cast<YBqlReadOp*>(yb_op)->response();
        if (yql_response.has_rows_data_sidecar()) {
          Slice rows_data;
          CHECK_OK(retrier().controller().GetSidecar(
              yql_response.rows_data_sidecar(), &rows_data));
          down_cast<YBqlReadOp*>(yb_op)->mutable_rows_data()->assign(
              util::to_char_ptr(rows_data.data()), rows_data.size());
        }
        yql_idx++;
        break;
      }
      case YBOperation::Type::INSERT: FALLTHROUGH_INTENDED;
      case YBOperation::Type::UPDATE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::DELETE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::REDIS_WRITE: FALLTHROUGH_INTENDED;
      case YBOperation::Type::YQL_WRITE:
        LOG(FATAL) << "Not a read operation " << op->yb_op->type();
        break;
    }
  }

  if (redis_idx != resp_.redis_batch().size() ||
      yql_idx != resp_.yql_batch().size()) {
    LOG(ERROR) << Substitute("Read response count mismatch: "
                             "$0 Redis requests sent, $1 responses received. "
                             "$2 YQL requests sent, $3 responses received.",
                             redis_idx, resp_.redis_batch().size(),
                             yql_idx, resp_.yql_batch().size());
    batcher_->AddOpCountMismatchError();
    MarkOpsAsFailed("Read response count mismatch");
  }
}

}  // namespace internal
}  // namespace client
}  // namespace yb
