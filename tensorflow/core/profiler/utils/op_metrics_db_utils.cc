/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "tensorflow/core/profiler/utils/op_metrics_db_utils.h"

#include "absl/strings/string_view.h"
#include "tensorflow/core/platform/logging.h"
#include "tensorflow/core/profiler/protobuf/op_metrics.pb.h"
#include "tensorflow/core/profiler/utils/math_utils.h"
#include "tensorflow/core/profiler/utils/tf_op_utils.h"

namespace tensorflow {
namespace profiler {
namespace {

class DeviceTfOpMetricsDbBuilder : public OpMetricsDbBuilder {
 public:
  explicit DeviceTfOpMetricsDbBuilder(OpMetricsDb* db)
      : OpMetricsDbBuilder(db) {}

  void UpdateTfOpMetricsWithHloOpMetrics(absl::string_view tf_op_name,
                                         absl::string_view tf_op_type,
                                         const OpMetrics& hlo_op_metrics) {
    OpMetrics* tf_op_metrics = OpMetricsDbBuilder::LookupOrInsertNewOpMetrics(
        /*hlo_module_id=*/0, tf_op_name);
    if (tf_op_metrics->category().empty())
      tf_op_metrics->set_category(tf_op_type.data(), tf_op_type.size());
    // The occurrences of a TF-op is the maximum among the occurrences of all
    // HLO-ops that it contains.
    tf_op_metrics->set_occurrences(
        std::max(tf_op_metrics->occurrences(), hlo_op_metrics.occurrences()));
    tf_op_metrics->set_time_ps(tf_op_metrics->time_ps() +
                               hlo_op_metrics.time_ps());
    tf_op_metrics->set_self_time_ps(tf_op_metrics->self_time_ps() +
                                    hlo_op_metrics.self_time_ps());
    tf_op_metrics->set_flops(tf_op_metrics->flops() + hlo_op_metrics.flops());
    tf_op_metrics->set_bytes_accessed(tf_op_metrics->bytes_accessed() +
                                      hlo_op_metrics.bytes_accessed());
  }
};

}  // namespace

OpMetricsDbBuilder::OpMetricsDbBuilder(OpMetricsDb* db)
    : db_(db) {
  DCHECK_NE(db_, nullptr);
  DCHECK_EQ(db_->metrics_db_size(), 0);
}

OpMetrics* OpMetricsDbBuilder::LookupOrInsertNewOpMetrics(
    uint64 hlo_module_id, absl::string_view name) {
  OpMetrics*& op_metrics = op_metrics_map_[hlo_module_id][name];
  if (op_metrics == nullptr) {
    op_metrics = db_->add_metrics_db();
    op_metrics->set_hlo_module_id(hlo_module_id);
    op_metrics->set_name(name.data(), name.size());
  }
  return op_metrics;
}

double IdleTimeRatio(const OpMetricsDb& metrics_db) {
  return 1.0 -
         SafeDivide(metrics_db.total_op_time_ps(), metrics_db.total_time_ps());
}

uint64 IdleTimePs(const OpMetricsDb& metrics_db) {
  return metrics_db.total_time_ps() - metrics_db.total_op_time_ps();
}

void AddIdleOp(OpMetricsDb* db) {
  uint64 idle_time_ps = IdleTimePs(*db);
  OpMetrics* metrics = db->add_metrics_db();
  metrics->set_name("IDLE");
  metrics->set_category("IDLE");
  metrics->set_occurrences(1);
  metrics->set_time_ps(idle_time_ps);
  metrics->set_self_time_ps(idle_time_ps);
}

OpMetricsDb CreateTfMetricsDbFromHloMetricsDb(
    const OpMetricsDb& hlo_metrics_db) {
  OpMetricsDb tf_op_metrics_db;
  DeviceTfOpMetricsDbBuilder builder(&tf_op_metrics_db);
  for (const auto& hlo_op_metrics : hlo_metrics_db.metrics_db()) {
    if (!hlo_op_metrics.provenance().empty()) {
      TfOp tf_op = ParseTfOpFullname(hlo_op_metrics.provenance());
      builder.UpdateTfOpMetricsWithHloOpMetrics(tf_op.name, tf_op.type,
                                                hlo_op_metrics);
    } else {
      DCHECK_EQ(hlo_op_metrics.name(), "IDLE");
      builder.UpdateTfOpMetricsWithHloOpMetrics("IDLE", "IDLE", hlo_op_metrics);
    }
  }
  tf_op_metrics_db.set_total_op_time_ps(hlo_metrics_db.total_op_time_ps());
  tf_op_metrics_db.set_total_time_ps(hlo_metrics_db.total_time_ps());
  return tf_op_metrics_db;
}
}  // namespace profiler
}  // namespace tensorflow
