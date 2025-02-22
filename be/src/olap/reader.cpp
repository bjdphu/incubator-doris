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

#include "olap/reader.h"

#include "olap/column_data.h"
#include "olap/olap_table.h"
#include "olap/row_block.h"
#include "olap/row_cursor.h"
#include "util/date_func.h"
#include "util/mem_util.hpp"
#include "runtime/mem_tracker.h"
#include "runtime/mem_pool.h"
#include <sstream>

#include "olap/comparison_predicate.h"
#include "olap/in_list_predicate.h"
#include "olap/null_predicate.h"

using std::nothrow;
using std::set;
using std::vector;

namespace doris {

class CollectIterator {
public:
    ~CollectIterator();

    // Hold reader point to get reader params, 
    // set reverse to true if need read in reverse order.
    OLAPStatus init(Reader* reader);

    OLAPStatus add_child(ColumnData* data, RowBlock* block);

    // Get top row of the heap, NULL if reach end.
    const RowCursor* current_row(bool* delete_flag) const {
        if (_cur_child != nullptr) {
            return _cur_child->current_row(delete_flag);
        }
        return nullptr;
    }

    // Pop the top element and rebuild the heap to 
    // get the next row cursor.
    inline OLAPStatus next(const RowCursor** row, bool* delete_flag);

    // Clear the MergeSet element and reset state.
    void clear();

private:
    class ChildCtx {
    public:
        ChildCtx(ColumnData* data, RowBlock* block, Reader* reader)
                : _data(data),
                _is_delete(data->delete_flag()),
                _reader(reader),
                _row_block(block) {
        }

        OLAPStatus init() {
            auto res = _row_cursor.init(_data->segment_group()->table()->tablet_schema(),
                                        _data->seek_columns());
            if (res != OLAP_SUCCESS) {
                LOG(WARNING) << "failed to init row cursor, res=" << res;
                return res;
            }
            res = _refresh_current_row();
            if (res != OLAP_SUCCESS) {
                return res;
            }
            return OLAP_SUCCESS;
        }

        const RowCursor* current_row(bool* delete_flag) const {
            *delete_flag = _is_delete;
            return _current_row;
        }

        const RowCursor* current_row() const {
            return _current_row;
        }

        int32_t version() const {
            return _data->version().second;
        }

        OLAPStatus next(const RowCursor** row, bool* delete_flag) {
            _row_block->pos_inc();
            auto res = _refresh_current_row();
            *row = _current_row;
            *delete_flag = _is_delete;
            return res;
        }

    private:
        // refresh _current_row, 
        OLAPStatus _refresh_current_row() {
            DCHECK(_row_block != nullptr);
            do {
                if (_row_block->has_remaining()) {
                    size_t pos = _row_block->pos();
                    _row_block->get_row(pos, &_row_cursor);
                    if (_row_block->block_status() == DEL_PARTIAL_SATISFIED &&
                        _reader->_delete_handler.is_filter_data(_data->version().second, _row_cursor)) {
                        _reader->_stats.rows_del_filtered++;
                        _row_block->pos_inc();
                        continue;
                    }
                    _current_row = &_row_cursor;
                    return OLAP_SUCCESS;
                } else {
                    auto res = _data->get_next_block(&_row_block);
                    if (res != OLAP_SUCCESS) {
                        _current_row = nullptr;
                        return res;
                    }
                }
            } while (_row_block != nullptr);
            _current_row = nullptr;
            return OLAP_ERR_DATA_EOF;
        }

        ColumnData* _data = nullptr;
        const RowCursor* _current_row = nullptr;
        bool _is_delete = false;
        Reader* _reader;

        RowCursor _row_cursor;
        RowBlock* _row_block = nullptr;
    };

    // Compare row cursors between multiple merge elements,
    // if row cursors equal, compare data version.
    class ChildCtxComparator {
    public:
        bool operator()(const ChildCtx* a, const ChildCtx* b);
    };

    inline OLAPStatus _merge_next(const RowCursor** row, bool* delete_flag);
    inline OLAPStatus _normal_next(const RowCursor** row, bool* delete_flag);

    // If _merge is true, result row must be ordered
    bool _merge = true;

    typedef std::priority_queue<ChildCtx*, std::vector<ChildCtx*>, ChildCtxComparator> MergeHeap;
    MergeHeap _heap;

    std::vector<ChildCtx*> _children;
    ChildCtx* _cur_child = nullptr;
    // Used when _merge is false
    int _child_idx = 0;

    // Hold reader point to access read params, such as fetch conditions.
    Reader* _reader = nullptr;
};


CollectIterator::~CollectIterator() {
    for (auto child : _children) {
        delete child;
    }
}

OLAPStatus CollectIterator::init(Reader* reader) {
    _reader = reader;
    // when aggregate is enabled or key_type is DUP_KEYS, we don't merge
    // multiple data to aggregate for performance in user fetch
    if (_reader->_reader_type == READER_QUERY &&
            (_reader->_aggregation ||
             _reader->_olap_table->keys_type() == KeysType::DUP_KEYS)) {
        _merge = false;
    }
    return OLAP_SUCCESS;
}

OLAPStatus CollectIterator::add_child(ColumnData* data, RowBlock* block) {
    std::unique_ptr<ChildCtx> child(new ChildCtx(data, block, _reader));
    RETURN_NOT_OK(child->init());
    if (child->current_row() == nullptr) {
        return OLAP_SUCCESS;
    }

    ChildCtx* child_ptr = child.release();
    _children.push_back(child_ptr);
    if (_merge) {
        _heap.push(child_ptr);
        _cur_child = _heap.top();
    } else {
        if (_cur_child == nullptr) {
            _cur_child = _children[_child_idx];
        }
    }
    return OLAP_SUCCESS;
}

inline OLAPStatus CollectIterator::next(const RowCursor** row, bool* delete_flag) {
    DCHECK(_cur_child != nullptr);
    if (_merge) {
        return _merge_next(row, delete_flag);
    } else {
        return _normal_next(row, delete_flag);
    }
}

inline OLAPStatus CollectIterator::_merge_next(const RowCursor** row, bool* delete_flag) {
    _heap.pop();
    auto res = _cur_child->next(row, delete_flag);
    if (res == OLAP_SUCCESS) {
        _heap.push(_cur_child);
        _cur_child = _heap.top();
    } else if (res == OLAP_ERR_DATA_EOF) {
        if (_heap.size() > 0) {
            _cur_child = _heap.top();
        } else {
            _cur_child = nullptr;
            return OLAP_ERR_DATA_EOF;
        }
    } else {
        LOG(WARNING) << "failed to get next from child, res=" << res;
        return res;
    }
    *row = _cur_child->current_row(delete_flag);
    return OLAP_SUCCESS;
}

inline OLAPStatus CollectIterator::_normal_next(const RowCursor** row, bool* delete_flag) {
    auto res = _cur_child->next(row, delete_flag);
    if (LIKELY(res == OLAP_SUCCESS)) {
        return OLAP_SUCCESS;
    } else if (res == OLAP_ERR_DATA_EOF) {
        // this child has been read, to read next
        _child_idx++;
        if (_child_idx < _children.size()) {
            _cur_child = _children[_child_idx]; 
            *row = _cur_child->current_row(delete_flag);
            return OLAP_SUCCESS;
        } else {
            _cur_child = nullptr;
            return OLAP_ERR_DATA_EOF;
        }
    } else {
        LOG(WARNING) << "failed to get next from child, res=" << res;
        return res;
    }
}

bool CollectIterator::ChildCtxComparator::operator()(const ChildCtx* a, const ChildCtx* b) {
    // First compare row cursor.
    const RowCursor* first = a->current_row();
    const RowCursor* second = b->current_row();
    int cmp_res = first->full_key_cmp(*second);
    if (cmp_res != 0) {
        return cmp_res > 0;
    }
    // if row cursors equal, compare data version.
    return a->version() > b->version();
}

void CollectIterator::clear() {
    while (_heap.size() > 0) {
        _heap.pop();
    }
    for (auto child : _children) {
        delete child;
    }
    // _children.swap(std::vector<ChildCtx*>());
    _children.clear();
    _cur_child = nullptr;
    _child_idx = 0;
}

Reader::Reader()
        : _next_key_index(0),
        _aggregation(false),
        _version_locked(false),
        _reader_type(READER_QUERY),
        _next_delete_flag(false),
        _next_key(NULL),
        _merged_rows(0) {
    _tracker.reset(new MemTracker(-1));
    _predicate_mem_pool.reset(new MemPool(_tracker.get()));
}

Reader::~Reader() {
    close();
}

OLAPStatus Reader::init(const ReaderParams& read_params) {
    OLAPStatus res = OLAP_SUCCESS;

    res = _init_params(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init reader when init params.[res=%d]", res);
        return res;
    }

    res = _acquire_data_sources(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init reader when acquire data sources.[res=%d]", res);      
        return res;
    }

    for (auto i_data: _data_sources) {
        i_data->set_stats(&_stats);
    }

    bool eof = false;
    if (OLAP_SUCCESS != (res = _attach_data_to_merge_set(true, &eof))) {
        OLAP_LOG_WARNING("failed to attaching data to merge set. [res=%d]", res);
        return res;
    }

    switch (_olap_table->keys_type()) {
    case KeysType::DUP_KEYS:
        _next_row_func = &Reader::_dup_key_next_row;
        break;
    case KeysType::UNIQUE_KEYS:
        _next_row_func = &Reader::_unique_key_next_row;
        break;
    case KeysType::AGG_KEYS:
        _next_row_func = &Reader::_agg_key_next_row;
        break;
    default:
        break;
    }
    DCHECK(_next_row_func != nullptr) << "No next row function for type:"
        << _olap_table->keys_type();

    return OLAP_SUCCESS;
}

OLAPStatus Reader::_dup_key_next_row(RowCursor* row_cursor, bool* eof) {
    *eof = false;
    if (_next_key == nullptr) {
        auto res = _attach_data_to_merge_set(false, eof);
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("failed to attach data to merge set.");
            return res;
        }
        if (*eof) {
            return OLAP_SUCCESS;
        }
    }
    row_cursor->copy_without_pool(*_next_key);
    auto res = _collect_iter->next(&_next_key, &_next_delete_flag);
    if (res != OLAP_SUCCESS) {
        if (res != OLAP_ERR_DATA_EOF) {
            return res;
        }
    }
    return OLAP_SUCCESS;
}

OLAPStatus Reader::_agg_key_next_row(RowCursor* row_cursor, bool* eof) {
    *eof = false;

    if (NULL == _next_key) {
        auto res = _attach_data_to_merge_set(false, eof);
        if (OLAP_SUCCESS != res) {
            OLAP_LOG_WARNING("failed to attach data to merge set.");
            return res;
        }
        if (*eof) {
            return OLAP_SUCCESS;
        }
    }
    row_cursor->agg_init(*_next_key);
    int64_t merged_count = 0;
    do {
        auto res = _collect_iter->next(&_next_key, &_next_delete_flag);
        if (res != OLAP_SUCCESS) {
            if (res != OLAP_ERR_DATA_EOF) {
                return res;
            }
            break;
        }

        if (_aggregation && merged_count > config::doris_scanner_row_num) {
            break;
        }
        // break while can NOT doing aggregation
        if (!RowCursor::equal(_key_cids, row_cursor, _next_key)) {
            break;
        }

        RowCursor::aggregate(_value_cids, row_cursor, _next_key);
        ++merged_count;
    } while (true);
    _merged_rows += merged_count;
    row_cursor->finalize_one_merge(_value_cids);
    return OLAP_SUCCESS;
}

OLAPStatus Reader::_unique_key_next_row(RowCursor* row_cursor, bool* eof) {
    *eof = false;
    bool cur_delete_flag = false;
    do {
        if (NULL == _next_key) {
            auto res = _attach_data_to_merge_set(false, eof);
            if (OLAP_SUCCESS != res) {
                OLAP_LOG_WARNING("failed to attach data to merge set.");
                return res;
            }
            if (*eof) {
                return OLAP_SUCCESS;
            }
        }
    
        cur_delete_flag = _next_delete_flag;
        row_cursor->agg_init(*_next_key);

        int64_t merged_count = 0;
        while (NULL != _next_key) {
            auto res = _collect_iter->next(&_next_key, &_next_delete_flag);
            if (res != OLAP_SUCCESS) {
                if (res != OLAP_ERR_DATA_EOF) {
                    return res;
                }
                break;
            }
    
            // we will not do aggregation in two case:
            //   1. DUP_KEYS keys type has no semantic to aggregate,
            //   2. to make cost of  each scan round reasonable, we will control merged_count.
            if (_olap_table->keys_type() == KeysType::DUP_KEYS
                || (_aggregation && merged_count > config::doris_scanner_row_num)) {
                row_cursor->finalize_one_merge(_value_cids);
                break;
            }
            // break while can NOT doing aggregation
            if (!RowCursor::equal(_key_cids, row_cursor, _next_key)) {
                row_cursor->finalize_one_merge(_value_cids);
                break;
            }

            cur_delete_flag = _next_delete_flag;
            RowCursor::aggregate(_value_cids, row_cursor, _next_key);
            ++merged_count;
        }
    
        _merged_rows += merged_count;
    
        if (!cur_delete_flag) {
            return OLAP_SUCCESS;
        }
    
        _stats.rows_del_filtered++;
    } while (cur_delete_flag);

    return OLAP_SUCCESS;
}

void Reader::close() {
    VLOG(3) << "merged rows:" << _merged_rows;
    _conditions.finalize();
    _delete_handler.finalize();
    _olap_table->release_data_sources(&_own_data_sources);

    for (auto pred : _col_predicates) {
        delete pred;
    }

    delete _collect_iter;
}

OLAPStatus Reader::_acquire_data_sources(const ReaderParams& read_params) {
    const std::vector<ColumnData*>* data_sources;
    if (read_params.reader_type == READER_ALTER_TABLE
            || read_params.reader_type == READER_BASE_COMPACTION
            || read_params.reader_type == READER_CUMULATIVE_COMPACTION) {
        data_sources = &read_params.olap_data_arr;
    } else {
        _olap_table->obtain_header_rdlock();
        _olap_table->acquire_data_sources(_version, &_own_data_sources);
        _olap_table->release_header_lock();

        if (_own_data_sources.size() < 1) {
            LOG(WARNING) << "fail to acquire data sources. [table_name='" << _olap_table->full_name()
                         << "' version=" << _version.first << "-" << _version.second << "]";
            return OLAP_ERR_VERSION_NOT_EXIST;
        }
        data_sources = &_own_data_sources;
    }
    
    // do not use index stream cache when be/ce/alter/checksum,
    // to avoid bringing down lru cache hit ratio
    bool is_using_cache = true;
    if (read_params.reader_type != READER_QUERY) {
        is_using_cache = false;
    }

    for (auto i_data: *data_sources) {
        // skip empty version
        if (i_data->empty() || i_data->zero_num_rows()) {
            continue;
        }
        i_data->set_delete_handler(_delete_handler);
        i_data->set_read_params(_return_columns,
                                _seek_columns,
                                _load_bf_columns,
                                _conditions,
                                _col_predicates,
                                is_using_cache,
                                read_params.runtime_state);
        if (i_data->delta_pruning_filter()) {
            VLOG(3) << "filter delta in query in condition:"
                    << i_data->version().first << ", " << i_data->version().second;
            _stats.rows_stats_filtered += i_data->num_rows();
            continue;
        }
        int ret = i_data->delete_pruning_filter();
        if (ret == DEL_SATISFIED) {
            VLOG(3) << "filter delta in delete predicate:"
                    << i_data->version().first << ", " << i_data->version().second;
            _stats.rows_del_filtered += i_data->num_rows();
            continue;
        } else if (ret == DEL_PARTIAL_SATISFIED) {
            VLOG(3) << "filter delta partially in delete predicate:"
                    << i_data->version().first << ", " << i_data->version().second;
            i_data->set_delete_status(DEL_PARTIAL_SATISFIED);
        } else {
            VLOG(3) << "not filter delta in delete predicate:"
                    << i_data->version().first << ", " << i_data->version().second;
            i_data->set_delete_status(DEL_NOT_SATISFIED);
        }
        _data_sources.push_back(i_data);
    }

    return OLAP_SUCCESS;
}

OLAPStatus Reader::_init_params(const ReaderParams& read_params) {
    OLAPStatus res = OLAP_SUCCESS;
    _aggregation = read_params.aggregation;
    _reader_type = read_params.reader_type;
    _olap_table = read_params.olap_table;
    _version = read_params.version;
    
    res = _init_conditions_param(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init conditions param. [res=%d]", res);
        return res;
    }

    res = _init_load_bf_columns(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init load bloom filter columns. [res=%d]", res);
        return res;
    }

    res = _init_delete_condition(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init delete param. [res=%d]", res);
        return res;
    }

    res = _init_return_columns(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init return columns. [res=%d]", res);
        return res;
    }

    res = _init_keys_param(read_params);
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init keys param. [res=%d]", res);
        return res;
    }

    res = _init_seek_columns();
    if (res != OLAP_SUCCESS) {
        OLAP_LOG_WARNING("fail to init seek columns. [res=%d]", res);
        return res;
    }

    _collect_iter = new CollectIterator();
    _collect_iter->init(this);

    return res;
}

OLAPStatus Reader::_init_return_columns(const ReaderParams& read_params) {
    if (read_params.reader_type == READER_QUERY) {
        _return_columns = read_params.return_columns;
        if (_delete_handler.conditions_num() != 0 && read_params.aggregation) {
            set<uint32_t> column_set(_return_columns.begin(), _return_columns.end());

            for (auto conds : _delete_handler.get_delete_conditions()) {
                for (auto cond_column : conds.del_cond->columns()) {
                    if (column_set.find(cond_column.first) == column_set.end()) {
                        column_set.insert(cond_column.first);
                        _return_columns.push_back(cond_column.first);
                    }
                }
            }
        }
        for (auto id : read_params.return_columns) {
            if (_olap_table->tablet_schema()[id].is_key) {
                _key_cids.push_back(id);
            } else {
                _value_cids.push_back(id);
            }
        }
    } else if (read_params.return_columns.size() == 0) {
        for (size_t i = 0; i < _olap_table->tablet_schema().size(); ++i) {
            _return_columns.push_back(i);
            if (_olap_table->tablet_schema()[i].is_key) {
                _key_cids.push_back(i);
            } else {
                _value_cids.push_back(i);
            }
        }
        VLOG(3) << "return column is empty, using full column as defaut.";
    } else if (read_params.reader_type == READER_CHECKSUM) {
        _return_columns = read_params.return_columns;
        for (auto id : read_params.return_columns) {
            if (_olap_table->tablet_schema()[id].is_key) {
                _key_cids.push_back(id);
            } else {
                _value_cids.push_back(id);
            }
        }
    } else {
        OLAP_LOG_WARNING("fail to init return columns. [reader_type=%d return_columns_size=%u]",
                         read_params.reader_type, read_params.return_columns.size());
        return OLAP_ERR_INPUT_PARAMETER_ERROR;
    }

    std::sort(_key_cids.begin(), _key_cids.end(), std::greater<uint32_t>());

    return OLAP_SUCCESS;
}

OLAPStatus Reader::_init_seek_columns() {
    std::unordered_set<uint32_t> column_set(_return_columns.begin(), _return_columns.end());
    for (auto& it : _conditions.columns()) {
        column_set.insert(it.first);
    }
    uint32_t max_key_column_count = 0;
    for (auto key : _keys_param.start_keys) {
        if (key->field_count() > max_key_column_count) {
            max_key_column_count = key->field_count();
        }
    }
    for (auto key : _keys_param.end_keys) {
        if (key->field_count() > max_key_column_count) {
            max_key_column_count = key->field_count();
        }
    }
    for (uint32_t i = 0; i < _olap_table->tablet_schema().size(); i++) {
        if (i < max_key_column_count || column_set.find(i) != column_set.end()) {
            _seek_columns.push_back(i);
        }
    }
    return OLAP_SUCCESS;
}

OLAPStatus Reader::_attach_data_to_merge_set(bool first, bool *eof) {
    OLAPStatus res = OLAP_SUCCESS;
    *eof = false;

    do {
        RowCursor *start_key = NULL;
        RowCursor *end_key = NULL;
        bool find_last_row = false;
        bool end_key_find_last_row = false;
        _collect_iter->clear();

        if (_keys_param.start_keys.size() > 0) {
            if (_next_key_index >= _keys_param.start_keys.size()) {
                *eof = true;
                VLOG(3) << "can NOT attach while start_key has been used.";
                return res;
            }
            auto cur_key_index = _next_key_index++;
            
            start_key = _keys_param.start_keys[cur_key_index];

            if (0 != _keys_param.end_keys.size()) {
                end_key = _keys_param.end_keys[cur_key_index];
                if (0 == _keys_param.end_range.compare("lt")) {
                    end_key_find_last_row = false;
                } else if (0 == _keys_param.end_range.compare("le")) {
                    end_key_find_last_row = true;
                } else {
                    OLAP_LOG_WARNING("reader params end_range is error. [range='%s']", 
                                     _keys_param.to_string().c_str());
                    res = OLAP_ERR_READER_GET_ITERATOR_ERROR;
                    return res;
                }
            }
            
            if (0 == _keys_param.range.compare("gt")) {
                if (NULL != end_key
                        && start_key->cmp(*end_key) >= 0) {
                    VLOG(10) << "return EOF when range=" << _keys_param.range
                             << ", start_key=" << start_key->to_string()
                             << ", end_key=" << end_key->to_string();
                    *eof = true;
                    return res;
                }
                
                find_last_row = true;
            } else if (0 == _keys_param.range.compare("ge")) {
                if (NULL != end_key
                        && start_key->cmp(*end_key) > 0) {
                    VLOG(10) << "return EOF when range=" << _keys_param.range
                             << ", start_key=" << start_key->to_string()
                             << ", end_key=" << end_key->to_string();
                    *eof = true;
                    return res;
                }
                
                find_last_row = false;
            } else if (0 == _keys_param.range.compare("eq")) {
                find_last_row = false;
                end_key = start_key;
                end_key_find_last_row = true;
            } else {
                OLAP_LOG_WARNING(
                        "reader params range is error. [range='%s']", 
                        _keys_param.to_string().c_str());
                res = OLAP_ERR_READER_GET_ITERATOR_ERROR;
                return res;
            }
        } else if (false == first) {
            *eof = true;
            return res;
        }

        for (auto data : _data_sources) {
            RowBlock* block = nullptr;
            auto res = data->prepare_block_read(
                start_key, find_last_row, end_key, end_key_find_last_row, &block);
            if (res == OLAP_SUCCESS) {
                res = _collect_iter->add_child(data, block);
                if (res != OLAP_SUCCESS && res != OLAP_ERR_DATA_EOF) {
                    LOG(WARNING) << "failed to add child to iterator";
                    return res;
                }
            } else if (res == OLAP_ERR_DATA_EOF) {
                continue;
            } else {
                LOG(WARNING) << "prepare block failed, res=" << res;
                return res;
            }
        }

        _next_key = _collect_iter->current_row(&_next_delete_flag);
        if (_next_key != NULL) {
            break;
        }

        first = false;
    } while (NULL == _next_key);

    return res;
}

OLAPStatus Reader::_init_keys_param(const ReaderParams& read_params) {
    OLAPStatus res = OLAP_SUCCESS;

    _next_key_index = 0;

    if (read_params.start_key.size() == 0) {
        return OLAP_SUCCESS;
    }

    _keys_param.range = read_params.range;
    _keys_param.end_range = read_params.end_range;

    size_t start_key_size = read_params.start_key.size();
    _keys_param.start_keys.resize(start_key_size, NULL);
    for (size_t i = 0; i < start_key_size; ++i) {
        if ((_keys_param.start_keys[i] = new(nothrow) RowCursor()) == NULL) {
            OLAP_LOG_WARNING("fail to new RowCursor!");
            return OLAP_ERR_MALLOC_ERROR;
        }
        
        res = _keys_param.start_keys[i]->init_scan_key(_olap_table->tablet_schema(),
                                                       read_params.start_key[i].values());
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("fail to init row cursor. [res=%d]", res);
            return res;
        }
        
        res = _keys_param.start_keys[i]->from_tuple(read_params.start_key[i]);
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("fail to init row cursor from Keys. [res=%d key_index=%ld]", res, i);
            return res;
        }
    }

    size_t end_key_size = read_params.end_key.size();
    _keys_param.end_keys.resize(end_key_size, NULL);
    for (size_t i = 0; i < end_key_size; ++i) {
        if ((_keys_param.end_keys[i] = new(nothrow) RowCursor()) == NULL) {
            OLAP_LOG_WARNING("fail to new RowCursor!");
            return OLAP_ERR_MALLOC_ERROR;
        }
        
        res = _keys_param.end_keys[i]->init_scan_key(_olap_table->tablet_schema(),
                                                     read_params.end_key[i].values());
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("fail to init row cursor. [res=%d]", res);
            return res;
        }
        
        res = _keys_param.end_keys[i]->from_tuple(read_params.end_key[i]);
        if (res != OLAP_SUCCESS) {
            OLAP_LOG_WARNING("fail to init row cursor from Keys. [res=%d key_index=%ld]", res, i);
            return res;
        }
    }

    //TODO:check the valid of start_key and end_key.(eg. start_key <= end_key)

    return OLAP_SUCCESS;
}

OLAPStatus Reader::_init_conditions_param(const ReaderParams& read_params) {
    OLAPStatus res = OLAP_SUCCESS;

    _conditions.set_table(_olap_table);
    for (int i = 0; i < read_params.conditions.size(); ++i) {
        _conditions.append_condition(read_params.conditions[i]);
        ColumnPredicate* predicate = _parse_to_predicate(read_params.conditions[i]);
        if (predicate != NULL) {
            _col_predicates.push_back(predicate);
        }
    }

    return res;
}

#define COMPARISON_PREDICATE_CONDITION_VALUE(NAME, PREDICATE) \
ColumnPredicate* Reader::_new_##NAME##_pred(FieldInfo& fi, int index, const std::string& cond) { \
    ColumnPredicate* predicate = NULL; \
    switch (fi.type) { \
        case OLAP_FIELD_TYPE_TINYINT: { \
            std::stringstream ss(cond); \
            int32_t value = 0; \
            ss >> value; \
            predicate = new PREDICATE<int8_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_SMALLINT: { \
            std::stringstream ss(cond); \
            int16_t value = 0; \
            ss >> value; \
            predicate = new PREDICATE<int16_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_INT: { \
            std::stringstream ss(cond); \
            int32_t value = 0; \
            ss >> value; \
            predicate = new PREDICATE<int32_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_BIGINT: { \
            std::stringstream ss(cond); \
            int64_t value = 0; \
            ss >> value; \
            predicate = new PREDICATE<int64_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_LARGEINT: { \
            std::stringstream ss(cond); \
            int128_t value = 0; \
            ss >> value; \
            predicate = new PREDICATE<int128_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_DECIMAL: { \
            decimal12_t value(0, 0); \
            value.from_string(cond); \
            predicate = new PREDICATE<decimal12_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_CHAR: {\
            StringValue value; \
            size_t length = std::max(static_cast<size_t>(fi.length), cond.length());\
            char* buffer = reinterpret_cast<char*>(_predicate_mem_pool->allocate(length)); \
            memset(buffer, 0, length); \
            memory_copy(buffer, cond.c_str(), cond.length()); \
            value.len = length; \
            value.ptr = buffer; \
            predicate = new PREDICATE<StringValue>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_VARCHAR: { \
            StringValue value; \
            int32_t length = cond.length(); \
            char* buffer = reinterpret_cast<char*>(_predicate_mem_pool->allocate(length)); \
            memory_copy(buffer, cond.c_str(), length); \
            value.len = length; \
            value.ptr = buffer; \
            predicate = new PREDICATE<StringValue>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_DATE: { \
            uint24_t value = timestamp_from_date(cond); \
            predicate = new PREDICATE<uint24_t>(index, value); \
            break; \
        } \
        case OLAP_FIELD_TYPE_DATETIME: { \
            uint64_t value = timestamp_from_datetime(cond); \
            predicate = new PREDICATE<uint64_t>(index, value); \
            break; \
        } \
        default: break; \
    } \
 \
    return predicate; \
} \

COMPARISON_PREDICATE_CONDITION_VALUE(eq, EqualPredicate)
COMPARISON_PREDICATE_CONDITION_VALUE(ne, NotEqualPredicate)
COMPARISON_PREDICATE_CONDITION_VALUE(lt, LessPredicate)
COMPARISON_PREDICATE_CONDITION_VALUE(le, LessEqualPredicate)
COMPARISON_PREDICATE_CONDITION_VALUE(gt, GreaterPredicate)
COMPARISON_PREDICATE_CONDITION_VALUE(ge, GreaterEqualPredicate)

ColumnPredicate* Reader::_parse_to_predicate(const TCondition& condition) {
    // TODO: not equal and not in predicate is not pushed down
    int index = _olap_table->get_field_index(condition.column_name);
    FieldInfo fi = _olap_table->tablet_schema()[index];
    if (fi.aggregation != FieldAggregationMethod::OLAP_FIELD_AGGREGATION_NONE) {
        return nullptr;
    }
    ColumnPredicate* predicate = NULL;
    if (condition.condition_op == "*="
            && condition.condition_values.size() == 1) {
        predicate = _new_eq_pred(fi, index, condition.condition_values[0]);
    } else if (condition.condition_op == "<<") {
        predicate = _new_lt_pred(fi, index, condition.condition_values[0]);
    } else if (condition.condition_op == "<=") {
        predicate = _new_le_pred(fi, index, condition.condition_values[0]);
    } else if (condition.condition_op == ">>") {
        predicate = _new_gt_pred(fi, index, condition.condition_values[0]);
    } else if (condition.condition_op == ">=") {
        predicate = _new_ge_pred(fi, index, condition.condition_values[0]);
    } else if (condition.condition_op == "*="
            && condition.condition_values.size() > 1) {
        switch (fi.type) {
            case OLAP_FIELD_TYPE_TINYINT: {
                std::set<int8_t> values;
                for (auto& cond_val : condition.condition_values) {
                    int32_t value = 0;
                    std::stringstream ss(cond_val);
                    ss >> value;
                    values.insert(value);
                }
                predicate = new InListPredicate<int8_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_SMALLINT: {
                std::set<int16_t> values;
                for (auto& cond_val : condition.condition_values) {
                    int16_t value = 0;
                    std::stringstream ss(cond_val);
                    ss >> value;
                    values.insert(value);
                }
                predicate = new InListPredicate<int16_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_INT: {
                std::set<int32_t> values;
                for (auto& cond_val : condition.condition_values) {
                    int32_t value = 0;
                    std::stringstream ss(cond_val);
                    ss >> value;
                    values.insert(value);
                }
                predicate = new InListPredicate<int32_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_BIGINT: {
                std::set<int64_t> values;
                for (auto& cond_val : condition.condition_values) {
                    int64_t value = 0;
                    std::stringstream ss(cond_val);
                    ss >> value;
                    values.insert(value);
                }
                predicate = new InListPredicate<int64_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_LARGEINT: {
                std::set<int128_t> values;
                for (auto& cond_val : condition.condition_values) {
                    int128_t value = 0;
                    std::stringstream ss(cond_val);
                    ss >> value;
                    values.insert(value);
                }
                predicate = new InListPredicate<int128_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_DECIMAL: {
                std::set<decimal12_t> values;
                for (auto& cond_val : condition.condition_values) {
                    decimal12_t value;
                    value.from_string(cond_val);
                    values.insert(value);
                }
                predicate = new InListPredicate<decimal12_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_CHAR: {
                std::set<StringValue> values;
                for (auto& cond_val : condition.condition_values) {
                    StringValue value;
                    size_t length = std::max(static_cast<size_t>(fi.length), cond_val.length());
                    char* buffer = reinterpret_cast<char*>(_predicate_mem_pool->allocate(length));
                    memset(buffer, 0, length);
                    memory_copy(buffer, cond_val.c_str(), cond_val.length());
                    value.len = length;
                    value.ptr = buffer;
                    values.insert(value);
                }
                predicate = new InListPredicate<StringValue>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_VARCHAR: {
                std::set<StringValue> values;
                for (auto& cond_val : condition.condition_values) {
                    StringValue value;
                    int32_t length = cond_val.length();
                    char* buffer = reinterpret_cast<char*>(_predicate_mem_pool->allocate(length));
                    memory_copy(buffer, cond_val.c_str(), length);
                    value.len = length;
                    value.ptr = buffer;
                    values.insert(value);
                }
                predicate = new InListPredicate<StringValue>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_DATE: {
                std::set<uint24_t> values;
                for (auto& cond_val : condition.condition_values) {
                    uint24_t value = timestamp_from_date(cond_val);
                    values.insert(value);
                }
                predicate = new InListPredicate<uint24_t>(index, std::move(values));
                break;
            }
            case OLAP_FIELD_TYPE_DATETIME: {
                std::set<uint64_t> values;
                for (auto& cond_val : condition.condition_values) {
                    uint64_t value = timestamp_from_datetime(cond_val);
                    values.insert(value);
                }
                predicate = new InListPredicate<uint64_t>(index, std::move(values));
                break;
            }
            default: break;
        }
    } else if (condition.condition_op == "is") {
        bool is_null = false;
        if (condition.condition_values[0] == "null") {
            is_null = true;
        } else {
            is_null = false;
        }
        predicate = new NullPredicate(index, is_null);
    }
    return predicate;
}

OLAPStatus Reader::_init_load_bf_columns(const ReaderParams& read_params) {
    OLAPStatus res = OLAP_SUCCESS;

    // add all columns with condition to _load_bf_columns
    for (const auto& cond_column : _conditions.columns()) {
        for (const Cond* cond : cond_column.second->conds()) {
            if (cond->op == OP_EQ
                    || (cond->op == OP_IN && cond->operand_set.size() < MAX_OP_IN_FIELD_NUM)) {
                _load_bf_columns.insert(cond_column.first);
            }
        }
    }

    // remove columns which have no bf stream
    for (int i = 0; i < _olap_table->tablet_schema().size(); ++i) {
        if (!_olap_table->tablet_schema()[i].is_bf_column) {
            _load_bf_columns.erase(i);
        }
    }

    // remove columns which have same value between start_key and end_key
    int min_scan_key_len = _olap_table->tablet_schema().size();
    for (int i = 0; i < read_params.start_key.size(); ++i) {
        if (read_params.start_key[i].size() < min_scan_key_len) {
            min_scan_key_len = read_params.start_key[i].size();
        }
    }

    for (int i = 0; i < read_params.end_key.size(); ++i) {
        if (read_params.end_key[i].size() < min_scan_key_len) {
            min_scan_key_len = read_params.end_key[i].size();
        }
    }

    int max_equal_index = -1;
    for (int i = 0; i < read_params.start_key.size(); ++i) {
        int j = 0;
        for (; j < min_scan_key_len; ++j) {
            if (read_params.start_key[i].get_value(j) != read_params.end_key[i].get_value(j)) {
                break;
            }
        }

        if (max_equal_index < j - 1) {
            max_equal_index = j - 1;
        }
    }

    for (int i = 0; i < max_equal_index; ++i) {
        _load_bf_columns.erase(i);
    }

    // remove the max_equal_index column when it's not varchar
    // or longer than number of short key fields
    FieldType type = _olap_table->get_field_type_by_index(max_equal_index);
    if ((type != OLAP_FIELD_TYPE_VARCHAR && type != OLAP_FIELD_TYPE_HLL)
            || max_equal_index + 1 > _olap_table->num_short_key_fields()) {
        _load_bf_columns.erase(max_equal_index);
    }

    return res;
}

OLAPStatus Reader::_init_delete_condition(const ReaderParams& read_params) {
    if (read_params.reader_type != READER_CUMULATIVE_COMPACTION) {
        _olap_table->obtain_header_rdlock();
        OLAPStatus ret = _delete_handler.init(_olap_table, read_params.version.second);
        _olap_table->release_header_lock();

        return ret;
    } else {
        return OLAP_SUCCESS;
    }
}

}  // namespace doris
