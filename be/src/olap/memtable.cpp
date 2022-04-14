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

#include "olap/memtable.h"

#include "common/logging.h"
#include "olap/row.h"
#include "olap/row_cursor.h"
#include "olap/rowset/column_data_writer.h"
#include "olap/rowset/rowset_writer.h"
#include "olap/schema.h"
#include "runtime/tuple.h"
#include "util/debug_util.h"
#include "util/doris_metrics.h"

namespace doris {

MemTable::MemTable(int64_t tablet_id, Schema* schema, const TabletSchema* tablet_schema,
                   const std::vector<SlotDescriptor*>* slot_descs, TupleDescriptor* tuple_desc,
                   KeysType keys_type, RowsetWriter* rowset_writer,
                   const std::shared_ptr<MemTracker>& parent_tracker)
        : _tablet_id(tablet_id),
          _schema(schema),
          _tablet_schema(tablet_schema),
          _slot_descs(slot_descs),
          _keys_type(keys_type),
          _mem_tracker(MemTracker::create_tracker(-1, "MemTable", parent_tracker)),
          _buffer_mem_pool(new MemPool(_mem_tracker.get())),
          _table_mem_pool(new MemPool(_mem_tracker.get())),
          _schema_size(_schema->schema_size()),
          _rowset_writer(rowset_writer) {
    if (tablet_schema->sort_type() == SortType::ZORDER) {
        _row_comparator =
                std::make_shared<TupleRowZOrderComparator>(_schema, tablet_schema->sort_col_num());
    } else {
        _row_comparator = std::make_shared<RowCursorComparator>(_schema);
    }
    _skip_list = new Table(_row_comparator.get(), _table_mem_pool.get(),
                           _keys_type == KeysType::DUP_KEYS);
}

MemTable::~MemTable() {
    delete _skip_list;
}

MemTable::RowCursorComparator::RowCursorComparator(const Schema* schema) : _schema(schema) {}

int MemTable::RowCursorComparator::operator()(const char* left, const char* right) const {
    ContiguousRow lhs_row(_schema, left);
    ContiguousRow rhs_row(_schema, right);
    return compare_row(lhs_row, rhs_row);
}

void MemTable::insert(const Tuple* tuple) {
    _rows++;
    bool overwritten = false;
    uint8_t* _tuple_buf = nullptr;
    if (_keys_type == KeysType::DUP_KEYS) {
        // Will insert directly, so use memory from _table_mem_pool
        _tuple_buf = _table_mem_pool->allocate(_schema_size);
        ContiguousRow row(_schema, _tuple_buf);
        _tuple_to_row(tuple, &row, _table_mem_pool.get());
        _skip_list->Insert((TableKey)_tuple_buf, &overwritten);
        DCHECK(!overwritten) << "Duplicate key model meet overwrite in SkipList";
        return;
    }

    // For non-DUP models, for the data rows passed from the upper layer, when copying the data,
    // we first allocate from _buffer_mem_pool, and then check whether it already exists in
    // _skiplist.  If it exists, we aggregate the new row into the row in skiplist.
    // otherwise, we need to copy it into _table_mem_pool before we can insert it.
    _tuple_buf = _buffer_mem_pool->allocate(_schema_size);
    ContiguousRow src_row(_schema, _tuple_buf);
    _tuple_to_row(tuple, &src_row, _buffer_mem_pool.get());

    bool is_exist = _skip_list->Find((TableKey)_tuple_buf, &_hint);
    if (is_exist) {
        _aggregate_two_row(src_row, _hint.curr->key);
    } else {
        _tuple_buf = _table_mem_pool->allocate(_schema_size);
        ContiguousRow dst_row(_schema, _tuple_buf);
        _agg_object_pool.acquire_data(&_agg_buffer_pool);
        copy_row_in_memtable(&dst_row, src_row, _table_mem_pool.get());
        _skip_list->InsertWithHint((TableKey)_tuple_buf, is_exist, &_hint);
    }

    // Make MemPool to be reusable, but does not free its memory
    _buffer_mem_pool->clear();
    _agg_buffer_pool.clear();
}

void MemTable::_tuple_to_row(const Tuple* tuple, ContiguousRow* row, MemPool* mem_pool) {
    for (size_t i = 0; i < _slot_descs->size(); ++i) {
        auto cell = row->cell(i);
        const SlotDescriptor* slot = (*_slot_descs)[i];

        bool is_null = tuple->is_null(slot->null_indicator_offset());
        const auto* value = (const char*)tuple->get_slot(slot->tuple_offset());
        _schema->column(i)->consume(&cell, value, is_null, mem_pool, &_agg_buffer_pool);
    }
}

void MemTable::_aggregate_two_row(const ContiguousRow& src_row, TableKey row_in_skiplist) {
    ContiguousRow dst_row(_schema, row_in_skiplist);
    if (_tablet_schema->has_sequence_col()) {
        agg_update_row_with_sequence(&dst_row, src_row, _tablet_schema->sequence_col_idx(),
                                     _table_mem_pool.get());
    } else {
        agg_update_row(&dst_row, src_row, _table_mem_pool.get());
    }
}

Status MemTable::flush() {
    VLOG_CRITICAL << "begin to flush memtable for tablet: " << _tablet_id
                  << ", memsize: " << memory_usage() << ", rows: " << _rows;
    int64_t duration_ns = 0;
    {
        SCOPED_RAW_TIMER(&duration_ns);
        Status st = _rowset_writer->flush_single_memtable(this, &_flush_size);
        if (st == Status::OLAPInternalError(OLAP_ERR_FUNC_NOT_IMPLEMENTED)) {
            // For alpha rowset, we do not implement "flush_single_memtable".
            // Flush the memtable like the old way.
            Table::Iterator it(_skip_list);
            for (it.SeekToFirst(); it.Valid(); it.Next()) {
                char* row = (char*)it.key();
                ContiguousRow dst_row(_schema, row);
                agg_finalize_row(&dst_row, _table_mem_pool.get());
                RETURN_NOT_OK(_rowset_writer->add_row(dst_row));
            }
            RETURN_NOT_OK(_rowset_writer->flush());
        } else {
            RETURN_NOT_OK(st);
        }
    }
    DorisMetrics::instance()->memtable_flush_total->increment(1);
    DorisMetrics::instance()->memtable_flush_duration_us->increment(duration_ns / 1000);
    VLOG_CRITICAL << "after flush memtable for tablet: " << _tablet_id
                  << ", flushsize: " << _flush_size;
    return Status::OK();
}

Status MemTable::close() {
    return flush();
}

MemTable::Iterator::Iterator(MemTable* memtable)
        : _mem_table(memtable), _it(memtable->_skip_list) {}

void MemTable::Iterator::seek_to_first() {
    _it.SeekToFirst();
}

bool MemTable::Iterator::valid() {
    return _it.Valid();
}

void MemTable::Iterator::next() {
    _it.Next();
}

ContiguousRow MemTable::Iterator::get_current_row() {
    char* row = (char*)_it.key();
    ContiguousRow dst_row(_mem_table->_schema, row);
    agg_finalize_row(&dst_row, _mem_table->_table_mem_pool.get());
    return dst_row;
}

} // namespace doris
