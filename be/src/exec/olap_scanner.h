// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef DORIS_BE_SRC_QUERY_EXEC_OLAP_SCANNER_H
#define DORIS_BE_SRC_QUERY_EXEC_OLAP_SCANNER_H

#include <list>
#include <vector>
#include <string>
#include <memory>
#include <utility>

#include "common/status.h"
#include "exec/olap_common.h"
#include "exec/exec_node.h"
#include "exprs/expr.h"
#include "gen_cpp/PaloInternalService_types.h"
#include "gen_cpp/PlanNodes_types.h"
#include "runtime/descriptors.h"
#include "runtime/tuple.h"
#include "runtime/vectorized_row_batch.h"

#include "olap/delete_handler.h"
#include "olap/column_data.h"
#include "olap/olap_cond.h"
#include "olap/olap_engine.h"
#include "olap/reader.h"

namespace doris {

class OlapScanNode;
class OLAPReader;
class RuntimeProfile;
class Field;

class OlapScanner {
public:
    OlapScanner(
        RuntimeState* runtime_state,
        OlapScanNode* parent,
        bool aggregation,
        DorisScanRange* scan_range,
        const std::vector<OlapScanRange>& key_ranges);

    ~OlapScanner();

    Status open();

    Status get_batch(RuntimeState* state, RowBatch* batch, bool* eof);

    Status close(RuntimeState* state);

    RuntimeState* runtime_state() {
        return _runtime_state;
    }

    std::vector<ExprContext*>* conjunct_ctxs() {
        return &_conjunct_ctxs;
    }

    int id() const { return _id; }
    void set_id(int id) { _id = id; }
    bool is_open() const { return _is_open; }
    void set_opened() { _is_open = true; }

    int64_t raw_rows_read() const { return _raw_rows_read; }

    void update_counter();
private:
    Status _prepare(
        DorisScanRange* scan_range,
        const std::vector<OlapScanRange>& key_ranges,
        const std::vector<TCondition>& filters,
        const std::vector<TCondition>& is_nulls);
    Status _init_params(
        const std::vector<OlapScanRange>& key_ranges,
        const std::vector<TCondition>& filters,
        const std::vector<TCondition>& is_nulls);
    Status _init_return_columns();
    void _convert_row_to_tuple(Tuple* tuple);

    // Update profile that need to be reported in realtime.
    void _update_realtime_counter();

    RuntimeState* _runtime_state;
    OlapScanNode* _parent;
    const TupleDescriptor* _tuple_desc;      /**< tuple descripter */
    RuntimeProfile* _profile;
    const std::vector<SlotDescriptor*>& _string_slots;

    std::vector<ExprContext*> _conjunct_ctxs;

    int _id;
    bool _is_open;
    bool _aggregation;
    bool _has_update_counter = false;

    Status _ctor_status;
    int _tuple_idx = 0;
    int _direct_conjunct_size = 0;

    bool _use_pushdown_conjuncts = false;

    ReaderParams _params;
    std::unique_ptr<Reader> _reader;

    OLAPTablePtr _olap_table;
    int64_t _version;

    std::vector<uint32_t> _return_columns;

    RowCursor _read_row_cursor;

    std::vector<uint32_t> _request_columns_size;

    std::vector<SlotDescriptor*> _query_slots;
    std::vector<const Field*> _query_fields;

    // time costed and row returned statistics
    ExecNode::EvalConjunctsFn _eval_conjuncts_fn = nullptr;

    RuntimeProfile::Counter* _rows_read_counter = nullptr;
    int64_t _num_rows_read = 0;
    int64_t _raw_rows_read = 0;

    RuntimeProfile::Counter* _rows_pushed_cond_filtered_counter = nullptr;
    // number rows filtered by pushed condition
    int64_t _num_rows_pushed_cond_filtered = 0;

    bool _is_closed = false;
};

} // namespace doris

#endif
