#include "MergeTreeBaseBlockInputStream.h"
#include <Storages/MergeTree/MergeTreeReader.h>
#include <Storages/MergeTree/MergeTreeBlockReadUtils.h>
#include <Columns/ColumnConst.h>
#include <ext/range.h>


namespace DB
{

namespace ErrorCodes
{
    extern const int ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER;
}


MergeTreeBaseBlockInputStream::MergeTreeBaseBlockInputStream(
    MergeTreeData & storage,
    const ExpressionActionsPtr & prewhere_actions,
    const String & prewhere_column,
    size_t max_block_size_rows,
    size_t preferred_block_size_bytes,
    size_t min_bytes_to_use_direct_io,
    size_t max_read_buffer_size,
    bool use_uncompressed_cache,
    bool save_marks_in_cache,
    const Names & virt_column_names)
:
    storage(storage),
    prewhere_actions(prewhere_actions),
    prewhere_column(prewhere_column),
    max_block_size_rows(max_block_size_rows),
    preferred_block_size_bytes(preferred_block_size_bytes),
    min_bytes_to_use_direct_io(min_bytes_to_use_direct_io),
    max_read_buffer_size(max_read_buffer_size),
    use_uncompressed_cache(use_uncompressed_cache),
    save_marks_in_cache(save_marks_in_cache),
    virt_column_names(virt_column_names),
    max_block_size_marks(max_block_size_rows / storage.index_granularity)
{
}


Block MergeTreeBaseBlockInputStream::readImpl()
{
    Block res;

    while (!res && !isCancelled())
    {
        if (!task && !getNewTask())
            break;

        res = readFromPart();

        if (res)
            injectVirtualColumns(res);

        if (task->isFinished())
            task.reset();
    }

    return res;
}


Block MergeTreeBaseBlockInputStream::readFromPart()
{
    Block res;

    if (task->size_predictor)
        task->size_predictor->startBlock();

    if (prewhere_actions)
    {
        do
        {
            /// Let's read the full block of columns needed to calculate the expression in PREWHERE.
            size_t space_left = std::max(1LU, max_block_size_rows);
            MarkRanges ranges_to_read;

            if (task->size_predictor)
            {
                /// FIXME: size prediction model is updated by filtered rows, but it predicts size of unfiltered rows also

                size_t recommended_rows = task->size_predictor->estimateNumRows(preferred_block_size_bytes);
                if (res && recommended_rows < 1)
                    break;

                space_left = std::min(space_left, std::max(1LU, recommended_rows));
            }

            std::experimental::optional<MergeTreeRangeReader> range_reader;
            if (task->current_range_reader)
                range_reader = task->current_range_reader->copyForReader(*pre_reader);

            while ((range_reader || !task->mark_ranges.empty()) && space_left && !isCancelled())
            {
                if (!range_reader)
                {
                    auto & range = task->mark_ranges.back();
                    task->current_range_reader = pre_reader->readRange(range.begin, range.end);
                    ranges_to_read.push_back(range);
                    task->mark_ranges.pop_back();
                }

                size_t rows_to_read = std::min(range_reader->unreadRows(), space_left);
                if (!range_reader->read(res, rows_to_read))
                    range_reader = std::experimental::nullopt;

                space_left -= rows_to_read;
            }

            /// In case of isCancelled.
            if (!res)
                return res;

            progressImpl({ res.rows(), res.bytes() });
            pre_reader->fillMissingColumns(res, task->ordered_names, task->should_reorder);

            /// Compute the expression in PREWHERE.
            prewhere_actions->execute(res);

            ColumnPtr column = res.getByName(prewhere_column).column;
            if (task->remove_prewhere_column)
                res.erase(prewhere_column);

            const auto pre_bytes = res.bytes();

            ColumnPtr observed_column;
            if (column->isNullable())
            {
                ColumnNullable & nullable_col = static_cast<ColumnNullable &>(*column);
                observed_column = nullable_col.getNestedColumn();
            }
            else
                observed_column = column;

            /** If the filter is a constant (for example, it says PREWHERE 1),
                * then either return an empty block, or return the block unchanged.
                */
            if (const auto column_const = typeid_cast<const ColumnConstUInt8 *>(observed_column.get()))
            {
                if (!column_const->getData())
                {
                    res.clear();
                    return res;
                }

                auto rows_to_read = column_const->size();
                if (task->current_range_reader && rows_to_read < task->current_range_reader->unreadRows())
                    task->current_range_reader->read(res, rows_to_read);
                else
                {
                    if (task->current_range_reader)
                    {
                        rows_to_read -= task->current_range_reader->unreadRows();
                        task->current_range_reader->read(res, task->current_range_reader->unreadRows());
                        task->current_range_reader = std::experimental::nullopt;
                    }

                    for (const auto & range : ranges_to_read)
                    {
                        if ((range.end - range.begin) * storage.index_granularity <= rows_to_read)
                            reader->readRange(range.begin, range.end, res);
                        else
                        {
                            task->current_range_reader = reader->readRange(range.begin, range.end);
                            task->current_range_reader->read(res, rows_to_read);
                        }
                    }
                }

                progressImpl({ 0, res.bytes() - pre_bytes });
            }
            else if (const auto column_vec = typeid_cast<const ColumnUInt8 *>(observed_column.get()))
            {
                throw Exception("column_vec");
                const auto & pre_filter = column_vec->getData();
                IColumn::Filter post_filter(pre_filter.size());

                /// Let's read the rest of the columns in the required segments and compose our own filter for them.
                size_t pre_filter_pos = 0;
                size_t post_filter_pos = 0;

                size_t next_range_idx = 0;
                while (pre_filter_pos < pre_filter.size())
                {
                    if (!task->current_range_reader)
                    {
                        const auto & range = ranges_to_read[next_range_idx++];
                        task->current_range_reader = reader->readRange(range.begin, range.end);
                    }
                    MergeTreeRangeReader & range_reader = task->current_range_reader.value();

                    auto pre_filter_begin_pos = pre_filter_pos;

                    while (range_reader.unreadRows() > 0 && pre_filter_pos < pre_filter.size())
                    {
                        auto rows_should_be_copied = pre_filter_pos - pre_filter_begin_pos;
                        auto unread_rows_in_current_part = range_reader.skipRows(rows_should_be_copied).unreadRowsInCurrentPart();
                        const size_t limit = std::min(pre_filter.size(), pre_filter_pos + unread_rows_in_current_part);
                        UInt8 nonzero = 0;
                        for (size_t row = pre_filter_pos; row < limit; ++row)
                            nonzero |= pre_filter[row];

                        bool will_read_until_mark = unread_rows_in_current_part == limit - pre_filter_pos;
                        /// can't skip empty rows if won't read until mark
                        if (!nonzero && will_read_until_mark)
                        {
                            if (pre_filter_pos != pre_filter_begin_pos)
                            {
                                auto rows = pre_filter_pos - pre_filter_begin_pos;
                                memcpy(&post_filter[post_filter_pos], &pre_filter[pre_filter_begin_pos], rows);
                                post_filter_pos += rows;
                                size_t cur_rows = res.rows();
                                range_reader.read(res, rows);
                                if (cur_rows + rows != res.rows())
                                    throw Exception("read " + std::to_string(res.rows() - cur_rows) + " expected " + std::to_string(rows) + " was rows " + std::to_string(cur_rows));
                            }

                            pre_filter_begin_pos = pre_filter_pos = limit;
                            range_reader.skipToNextMark();
                        }
                        else
                            pre_filter_pos = limit;
                    }

                    if (pre_filter_pos != pre_filter_begin_pos)
                    {
                        auto rows = pre_filter_pos - pre_filter_begin_pos;
                        memcpy(&post_filter[post_filter_pos], &pre_filter[pre_filter_begin_pos], rows);
                        post_filter_pos += rows;
                        size_t cur_rows = res.rows();
                        range_reader.read(res, rows);
                        if (cur_rows + rows != res.rows())
                            throw Exception("read " + std::to_string(res.rows() - cur_rows) + " expected " + std::to_string(rows) + " was rows " + std::to_string(cur_rows));
                    }

                    if (range_reader.unreadRows() == 0)
                        task->current_range_reader = std::experimental::nullopt;
                }

                if (!post_filter_pos)
                {
                    res.clear();
                    continue;
                }

                progressImpl({ 0, res.bytes() - pre_bytes });

                post_filter.resize(post_filter_pos);

                /// Filter the columns related to PREWHERE using pre_filter,
                ///  other columns - using post_filter.
                size_t rows = 0;
                for (const auto i : ext::range(0, res.columns()))
                {
                    auto & col = res.safeGetByPosition(i);
                    if (col.name == prewhere_column && res.columns() > 1)
                        continue;
                    col.column =
                        col.column->filter(task->column_name_set.count(col.name) ? post_filter : pre_filter, -1);
                    rows = col.column->size();
                }

                /// Replace column with condition value from PREWHERE to a constant.
                if (!task->remove_prewhere_column)
                    res.getByName(prewhere_column).column = std::make_shared<ColumnConstUInt8>(rows, 1);
            }
            else
                throw Exception{
                    "Illegal type " + column->getName() + " of column for filter. Must be ColumnUInt8 or ColumnConstUInt8.",
                    ErrorCodes::ILLEGAL_TYPE_OF_COLUMN_FOR_FILTER
                };

            if (res)
            {
                if (task->size_predictor)
                    task->size_predictor->update(res);

                reader->fillMissingColumnsAndReorder(res, task->ordered_names);
            }
        }
        while (!task->isFinished() && !res && !isCancelled());
    }
    else
    {
        size_t space_left = std::max(1LU, max_block_size_rows);
        while (!task->isFinished() && space_left && !isCancelled())
        {
            if (!task->current_range_reader)
            {
                auto & range = task->mark_ranges.back();
                task->current_range_reader = reader->readRange(range.begin, range.end);
                task->mark_ranges.pop_back();
            }

            size_t rows_to_read = space_left;

            if (task->size_predictor)
            {
                size_t recommended_rows = task->size_predictor->estimateNumRows(preferred_block_size_bytes);

                /// TODO: stop reading if recommended_rows small enough
                if (res && recommended_rows < 1)
                    break;
                rows_to_read = std::min(rows_to_read, std::max(1LU, recommended_rows));
            }

            if (!task->current_range_reader->read(res, rows_to_read))
                task->current_range_reader = std::experimental::nullopt;

            if (task->size_predictor)
                task->size_predictor->update(res);

            space_left -= rows_to_read;
        }

        /// In the case of isCancelled.
        if (!res)
            return res;

        progressImpl({ res.rows(), res.bytes() });
        reader->fillMissingColumns(res, task->ordered_names, task->should_reorder);
    }

    return res;
}


void MergeTreeBaseBlockInputStream::injectVirtualColumns(Block & block)
{
    const auto rows = block.rows();

    /// add virtual columns
    /// Except _sample_factor, which is added from the outside.
    if (!virt_column_names.empty())
    {
        for (const auto & virt_column_name : virt_column_names)
        {
            if (virt_column_name == "_part")
            {
                block.insert(ColumnWithTypeAndName{
                    ColumnConst<String>{rows, task->data_part->name}.convertToFullColumn(),
                    std::make_shared<DataTypeString>(),
                    virt_column_name
                });
            }
            else if (virt_column_name == "_part_index")
            {
                block.insert(ColumnWithTypeAndName{
                    ColumnConst<UInt64>{rows, task->part_index_in_query}.convertToFullColumn(),
                    std::make_shared<DataTypeUInt64>(),
                    virt_column_name
                });
            }
        }
    }
}


MergeTreeBaseBlockInputStream::~MergeTreeBaseBlockInputStream() = default;

}
