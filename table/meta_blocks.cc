//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#include "table/meta_blocks.h"

#include <map>
#include <string>

#include "db/table_properties_collector.h"
#include "rocksdb/table.h"
#include "rocksdb/table_properties.h"
#include "table/block.h"
#include "table/block_fetcher.h"
#include "table/format.h"
#include "table/internal_iterator.h"
#include "table/persistent_cache_helper.h"
#include "table/table_properties_internal.h"
#include "util/coding.h"
#include "util/file_reader_writer.h"

#include "rocksdb/terark_namespace.h"
namespace TERARKDB_NAMESPACE {

MetaIndexBuilder::MetaIndexBuilder()
    : meta_index_block_(new BlockBuilder(1 /* restart interval */)) {}

void MetaIndexBuilder::Add(const std::string& key, const BlockHandle& handle) {
  std::string handle_encoding;
  handle.EncodeTo(&handle_encoding);
  meta_block_handles_.insert({key, handle_encoding});
}

Slice MetaIndexBuilder::Finish() {
  for (const auto& metablock : meta_block_handles_) {
    meta_index_block_->Add(metablock.first, metablock.second);
  }
  return meta_index_block_->Finish();
}

// Property block will be read sequentially and cached in a heap located
// object, so there's no need for restart points. Thus we set the restart
// interval to infinity to save space.
PropertyBlockBuilder::PropertyBlockBuilder()
    : properties_block_(
          new BlockBuilder(port::kMaxInt32 /* restart interval */)) {}

void PropertyBlockBuilder::Add(const std::string& name,
                               const std::string& val) {
  props_.insert({name, val});
}

void PropertyBlockBuilder::Add(const std::string& name, uint64_t val) {
  assert(props_.find(name) == props_.end());

  std::string dst;
  PutVarint64(&dst, val);

  Add(name, dst);
}

void PropertyBlockBuilder::Add(const std::string& name,
                               const std::vector<uint64_t>& val) {
  std::string dst;
  PutVarint64(&dst, val.size());
  for (const auto& v : val) {
    PutVarint64(&dst, v);
  }

  Add(name, dst);
}

void PropertyBlockBuilder::Add(
    const UserCollectedProperties& user_collected_properties) {
  for (const auto& prop : user_collected_properties) {
    Add(prop.first, prop.second);
  }
}

void PropertyBlockBuilder::AddTableProperty(const TableProperties& props) {
  Add(TablePropertiesNames::kRawKeySize, props.raw_key_size);
  Add(TablePropertiesNames::kRawValueSize, props.raw_value_size);
  Add(TablePropertiesNames::kDataSize, props.data_size);
  Add(TablePropertiesNames::kIndexSize, props.index_size);
  if (props.index_partitions != 0) {
    Add(TablePropertiesNames::kIndexPartitions, props.index_partitions);
    Add(TablePropertiesNames::kTopLevelIndexSize, props.top_level_index_size);
  }
  Add(TablePropertiesNames::kIndexKeyIsUserKey, props.index_key_is_user_key);
  Add(TablePropertiesNames::kIndexValueIsDeltaEncoded,
      props.index_value_is_delta_encoded);
  Add(TablePropertiesNames::kNumEntries, props.num_entries);
  Add(TablePropertiesNames::kDeletedKeys, props.num_deletions);
  Add(TablePropertiesNames::kMergeOperands, props.num_merge_operands);
  Add(TablePropertiesNames::kNumRangeDeletions, props.num_range_deletions);
  Add(TablePropertiesNames::kNumDataBlocks, props.num_data_blocks);
  Add(TablePropertiesNames::kFilterSize, props.filter_size);
  Add(TablePropertiesNames::kFormatVersion, props.format_version);
  Add(TablePropertiesNames::kFixedKeyLen, props.fixed_key_len);
  Add(TablePropertiesNames::kColumnFamilyId, props.column_family_id);
  Add(TablePropertiesNames::kCreationTime, props.creation_time);
  Add(TablePropertiesNames::kOldestKeyTime, props.oldest_key_time);
  if (!props.snapshots.empty()) {
    Add(TablePropertiesNames::kSnapshots, props.snapshots);
  }
  if (props.purpose != 0) {
    Add(TablePropertiesNames::kPurpose, props.purpose);
  }
  if (props.max_read_amp > 1) {
    std::string val;
    PutVarint32Varint64(&val, props.max_read_amp, DoubleToU64(props.read_amp));
    Add(TablePropertiesNames::kReadAmp, val);
  }
  if (!props.dependence.empty()) {
    std::vector<uint64_t> val;
    val.reserve(props.dependence.size());
    for (auto& dependence : props.dependence) {
      val.emplace_back(dependence.file_number);
    }
    Add(TablePropertiesNames::kDependence, val);
    val.clear();
    for (auto& dependence : props.dependence) {
      val.emplace_back(dependence.entry_count);
    }
    Add(TablePropertiesNames::kDependenceEntryCount, val);
  }
  if (!props.inheritance_chain.empty()) {
    Add(TablePropertiesNames::kInheritanceChain, props.inheritance_chain);
  }

  if (!props.filter_policy_name.empty()) {
    Add(TablePropertiesNames::kFilterPolicy, props.filter_policy_name);
  }
  if (!props.comparator_name.empty()) {
    Add(TablePropertiesNames::kComparator, props.comparator_name);
  }

  if (!props.merge_operator_name.empty()) {
    Add(TablePropertiesNames::kMergeOperator, props.merge_operator_name);
  }
  if (!props.value_meta_extractor_name.empty()) {
    Add(TablePropertiesNames::kValueMetaExtractorName,
        props.value_meta_extractor_name);
  }
  if (!props.prefix_extractor_name.empty()) {
    Add(TablePropertiesNames::kPrefixExtractorName,
        props.prefix_extractor_name);
  }
  if (!props.property_collectors_names.empty()) {
    Add(TablePropertiesNames::kPropertyCollectors,
        props.property_collectors_names);
  }
  if (!props.column_family_name.empty()) {
    Add(TablePropertiesNames::kColumnFamilyName, props.column_family_name);
  }

  if (!props.compression_name.empty()) {
    Add(TablePropertiesNames::kCompression, props.compression_name);
  }
  if (true) {
    Add(TablePropertiesNames::kRatioExpireTime, props.ratio_expire_time);
    Add(TablePropertiesNames::kScanGapExpireTime, props.scan_gap_expire_time);
  }
}

Slice PropertyBlockBuilder::Finish() {
  for (const auto& prop : props_) {
    properties_block_->Add(prop.first, prop.second);
  }

  return properties_block_->Finish();
}

void LogPropertiesCollectionError(Logger* info_log, const std::string& method,
                                  const std::string& name) {
  assert(method == "Add" || method == "Finish");

  std::string msg =
      "Encountered error when calling TablePropertiesCollector::" + method +
      "() with collector name: " + name;
  ROCKS_LOG_ERROR(info_log, "%s", msg.c_str());
}

bool NotifyCollectTableCollectorsOnAdd(
    const Slice& key, const Slice& value, uint64_t file_size,
    const std::vector<std::unique_ptr<IntTblPropCollector>>& collectors,
    Logger* info_log) {
  bool all_succeeded = true;
  for (auto& collector : collectors) {
    Status s = collector->InternalAdd(key, value, file_size);
    all_succeeded = all_succeeded && s.ok();
    if (!s.ok()) {
      LogPropertiesCollectionError(info_log, "Add" /* method */,
                                   collector->Name());
    }
  }
  return all_succeeded;
}

bool NotifyCollectTableCollectorsOnFinish(
    const std::vector<std::unique_ptr<IntTblPropCollector>>& collectors,
    Logger* info_log, PropertyBlockBuilder* builder) {
  bool all_succeeded = true;
  for (auto& collector : collectors) {
    UserCollectedProperties user_collected_properties;
    Status s = collector->Finish(&user_collected_properties);

    all_succeeded = all_succeeded && s.ok();
    if (!s.ok()) {
      LogPropertiesCollectionError(info_log, "Finish" /* method */,
                                   collector->Name());
    } else {
      builder->Add(user_collected_properties);
    }
  }

  return all_succeeded;
}

Status ReadProperties(const Slice& handle_value, RandomAccessFileReader* file,
                      FilePrefetchBuffer* prefetch_buffer, const Footer& footer,
                      const ImmutableCFOptions& ioptions,
                      TableProperties** table_properties,
                      bool /*compression_type_missing*/,
                      MemoryAllocator* memory_allocator) {
  assert(table_properties);

  Slice v = handle_value;
  BlockHandle handle;
  if (!handle.DecodeFrom(&v).ok()) {
    return Status::InvalidArgument("Failed to decode properties block handle");
  }

  BlockContents block_contents;
  ReadOptions read_options;
  read_options.verify_checksums = false;
  Status s;
  Slice compression_dict;
  PersistentCacheOptions cache_options;

  BlockFetcher block_fetcher(file, prefetch_buffer, footer, read_options,
                             handle, &block_contents, ioptions,
                             false /* decompress */, false /*maybe_compressed*/,
                             compression_dict, cache_options, memory_allocator);
  s = block_fetcher.ReadBlockContents();
  // property block is never compressed. Need to add uncompress logic if we are
  // to compress it..

  if (!s.ok()) {
    return s;
  }

  Block properties_block(std::move(block_contents),
                         kDisableGlobalSequenceNumber);
  DataBlockIter iter;
  properties_block.NewIterator<DataBlockIter>(BytewiseComparator(),
                                              BytewiseComparator(), &iter);

  auto new_table_properties = new TableProperties();
  // All pre-defined properties of type uint64_t
  std::unordered_map<std::string, uint64_t*> predefined_uint64_properties = {
      {TablePropertiesNames::kDataSize, &new_table_properties->data_size},
      {TablePropertiesNames::kIndexSize, &new_table_properties->index_size},
      {TablePropertiesNames::kIndexPartitions,
       &new_table_properties->index_partitions},
      {TablePropertiesNames::kTopLevelIndexSize,
       &new_table_properties->top_level_index_size},
      {TablePropertiesNames::kIndexKeyIsUserKey,
       &new_table_properties->index_key_is_user_key},
      {TablePropertiesNames::kIndexValueIsDeltaEncoded,
       &new_table_properties->index_value_is_delta_encoded},
      {TablePropertiesNames::kFilterSize, &new_table_properties->filter_size},
      {TablePropertiesNames::kRawKeySize, &new_table_properties->raw_key_size},
      {TablePropertiesNames::kRawValueSize,
       &new_table_properties->raw_value_size},
      {TablePropertiesNames::kNumDataBlocks,
       &new_table_properties->num_data_blocks},
      {TablePropertiesNames::kNumEntries, &new_table_properties->num_entries},
      {TablePropertiesNames::kDeletedKeys,
       &new_table_properties->num_deletions},
      {TablePropertiesNames::kMergeOperands,
       &new_table_properties->num_merge_operands},
      {TablePropertiesNames::kNumRangeDeletions,
       &new_table_properties->num_range_deletions},
      {TablePropertiesNames::kFormatVersion,
       &new_table_properties->format_version},
      {TablePropertiesNames::kFixedKeyLen,
       &new_table_properties->fixed_key_len},
      {TablePropertiesNames::kColumnFamilyId,
       &new_table_properties->column_family_id},
      {TablePropertiesNames::kCreationTime,
       &new_table_properties->creation_time},
      {TablePropertiesNames::kOldestKeyTime,
       &new_table_properties->oldest_key_time},
      {TablePropertiesNames::kRatioExpireTime,
       &new_table_properties->ratio_expire_time},
      {TablePropertiesNames::kScanGapExpireTime,
       &new_table_properties->scan_gap_expire_time},
  };

  auto GetUint64Vector = [&](const std::string& key, Slice* raw_val,
                             std::vector<uint64_t>& val) {
    bool ok = true;
    uint64_t size;
    if (GetVarint64(raw_val, &size)) {
      val.resize(size);
      for (auto& v : val) {
        if (!GetVarint64(raw_val, &v)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        return;
      }
    }
    val.clear();
    auto error_msg =
        "Detect malformed value in properties meta-block:\tkey: " + key +
        "\tval: " + raw_val->ToString();
    ROCKS_LOG_ERROR(ioptions.info_log, "%s", error_msg.c_str());
  };

  std::string last_key;
  for (iter.SeekToFirst(); iter.Valid(); iter.Next()) {
    s = iter.status();
    if (!s.ok()) {
      break;
    }

    auto key = iter.key().ToString();
    // properties block is strictly sorted with no duplicate key.
    assert(last_key.empty() ||
           BytewiseComparator()->Compare(key, last_key) > 0);
    last_key = key;

    auto raw_val = iter.value();
    auto pos = predefined_uint64_properties.find(key);

    auto log_error = [&] {
      auto error_msg =
          "Detect malformed value in properties meta-block:\tkey: " + key +
          "\tval: " + raw_val.ToString();
      ROCKS_LOG_ERROR(ioptions.info_log, "%s", error_msg.c_str());
    };

    if (pos != predefined_uint64_properties.end()) {
      if (key == TablePropertiesNames::kDeletedKeys ||
          key == TablePropertiesNames::kMergeOperands) {
        // Insert in user-collected properties for API backwards compatibility
        new_table_properties->user_collected_properties.insert(
            {key, raw_val.ToString()});
      }
      // handle predefined rocksdb properties
      uint64_t val;
      if (!GetVarint64(&raw_val, &val)) {
        // skip malformed value
        log_error();
        continue;
      }
      *pos->second = val;
    } else if (key == TablePropertiesNames::kFilterPolicy) {
      new_table_properties->filter_policy_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kColumnFamilyName) {
      new_table_properties->column_family_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kComparator) {
      new_table_properties->comparator_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kMergeOperator) {
      new_table_properties->merge_operator_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kValueMetaExtractorName) {
      new_table_properties->value_meta_extractor_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kPrefixExtractorName) {
      new_table_properties->prefix_extractor_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kPropertyCollectors) {
      new_table_properties->property_collectors_names = raw_val.ToString();
    } else if (key == TablePropertiesNames::kCompression) {
      new_table_properties->compression_name = raw_val.ToString();
    } else if (key == TablePropertiesNames::kSnapshots) {
      GetUint64Vector(key, &raw_val, new_table_properties->snapshots);
    } else if (key == TablePropertiesNames::kPurpose) {
      uint64_t val;
      if (!GetVarint64(&raw_val, &val)) {
        // skip malformed value
        log_error();
        continue;
      }
      new_table_properties->purpose = val;
    } else if (key == TablePropertiesNames::kReadAmp) {
      uint32_t u32_val;
      uint64_t u64_val;
      if (!GetVarint32(&raw_val, &u32_val) ||
          !GetVarint64(&raw_val, &u64_val)) {
        // skip malformed value
        log_error();
        continue;
      }
      new_table_properties->max_read_amp = uint16_t(u32_val);
      new_table_properties->read_amp = U64ToDouble(u64_val);
    } else if (key == TablePropertiesNames::kDependence) {
      std::vector<uint64_t> val;
      GetUint64Vector(key, &raw_val, val);
      if (new_table_properties->dependence.empty()) {
        new_table_properties->dependence.resize(val.size());
      } else if (new_table_properties->dependence.size() != val.size()) {
        log_error();
        continue;
      }
      for (size_t i = 0; i < val.size(); ++i) {
        new_table_properties->dependence[i].file_number = val[i];
      }
    } else if (key == TablePropertiesNames::kDependenceEntryCount) {
      std::vector<uint64_t> val;
      GetUint64Vector(key, &raw_val, val);
      if (new_table_properties->dependence.empty()) {
        new_table_properties->dependence.resize(val.size());
      } else if (new_table_properties->dependence.size() != val.size()) {
        log_error();
        continue;
      }
      for (size_t i = 0; i < val.size(); ++i) {
        new_table_properties->dependence[i].entry_count = val[i];
      }
    } else if (key == TablePropertiesNames::kInheritanceChain) {
      GetUint64Vector(key, &raw_val, new_table_properties->inheritance_chain);
    } else {
      // handle user-collected properties
      new_table_properties->user_collected_properties.insert(
          {key, raw_val.ToString()});
    }
  }
  if (s.ok()) {
    *table_properties = new_table_properties;
  } else {
    delete new_table_properties;
  }

  return s;
}

Status ReadTableProperties(RandomAccessFileReader* file, uint64_t file_size,
                           uint64_t table_magic_number,
                           const ImmutableCFOptions& ioptions,
                           TableProperties** properties,
                           bool compression_type_missing,
                           MemoryAllocator* memory_allocator) {
  // -- Read metaindex block
  Footer footer;
  auto s = ReadFooterFromFile(file, nullptr /* prefetch_buffer */, file_size,
                              &footer, table_magic_number);
  if (!s.ok()) {
    return s;
  }

  auto metaindex_handle = footer.metaindex_handle();
  BlockContents metaindex_contents;
  ReadOptions read_options;
  read_options.verify_checksums = false;
  Slice compression_dict;
  PersistentCacheOptions cache_options;

  BlockFetcher block_fetcher(file, nullptr /* prefetch_buffer */, footer,
                             read_options, metaindex_handle,
                             &metaindex_contents, ioptions,
                             false /* decompress */, false /*maybe_compressed*/,
                             compression_dict, cache_options, memory_allocator);
  s = block_fetcher.ReadBlockContents();
  if (!s.ok()) {
    return s;
  }
  // property blocks are never compressed. Need to add uncompress logic if we
  // are to compress it.
  Block metaindex_block(std::move(metaindex_contents),
                        kDisableGlobalSequenceNumber);
  std::unique_ptr<InternalIteratorBase<Slice>> meta_iter(
      metaindex_block.NewIterator<DataBlockIter>(BytewiseComparator(),
                                                 BytewiseComparator()));

  // -- Read property block
  bool found_properties_block = true;
  s = SeekToPropertiesBlock(meta_iter.get(), &found_properties_block);
  if (!s.ok()) {
    return s;
  }

  TableProperties table_properties;
  if (found_properties_block == true) {
    s = ReadProperties(meta_iter->value(), file, nullptr /* prefetch_buffer */,
                       footer, ioptions, properties, compression_type_missing,
                       memory_allocator);
  } else {
    s = Status::NotFound();
  }

  return s;
}

Status FindMetaBlock(InternalIteratorBase<Slice>* meta_index_iter,
                     const std::string& meta_block_name,
                     BlockHandle* block_handle) {
  meta_index_iter->Seek(meta_block_name);
  if (meta_index_iter->status().ok() && meta_index_iter->Valid() &&
      meta_index_iter->key() == meta_block_name) {
    Slice v = meta_index_iter->value();
    return block_handle->DecodeFrom(&v);
  } else {
    return Status::Corruption("Cannot find the meta block", meta_block_name);
  }
}

Status FindMetaBlock(RandomAccessFileReader* file, uint64_t file_size,
                     uint64_t table_magic_number,
                     const ImmutableCFOptions& ioptions,
                     const std::string& meta_block_name,
                     BlockHandle* block_handle,
                     bool /*compression_type_missing*/,
                     MemoryAllocator* memory_allocator) {
  Footer footer;
  auto s = ReadFooterFromFile(file, nullptr /* prefetch_buffer */, file_size,
                              &footer, table_magic_number);
  if (!s.ok()) {
    return s;
  }

  auto metaindex_handle = footer.metaindex_handle();
  BlockContents metaindex_contents;
  ReadOptions read_options;
  read_options.verify_checksums = false;
  Slice compression_dict;
  PersistentCacheOptions cache_options;
  BlockFetcher block_fetcher(
      file, nullptr /* prefetch_buffer */, footer, read_options,
      metaindex_handle, &metaindex_contents, ioptions,
      false /* do decompression */, false /*maybe_compressed*/,
      compression_dict, cache_options, memory_allocator);
  s = block_fetcher.ReadBlockContents();
  if (!s.ok()) {
    return s;
  }
  // meta blocks are never compressed. Need to add uncompress logic if we are to
  // compress it.
  Block metaindex_block(std::move(metaindex_contents),
                        kDisableGlobalSequenceNumber);

  std::unique_ptr<InternalIteratorBase<Slice>> meta_iter;
  meta_iter.reset(metaindex_block.NewIterator<DataBlockIter>(
      BytewiseComparator(), BytewiseComparator()));

  return FindMetaBlock(meta_iter.get(), meta_block_name, block_handle);
}

Status ReadMetaBlock(RandomAccessFileReader* file,
                     FilePrefetchBuffer* prefetch_buffer, uint64_t file_size,
                     uint64_t table_magic_number,
                     const ImmutableCFOptions& ioptions,
                     const std::string& meta_block_name,
                     BlockContents* contents, bool /*compression_type_missing*/,
                     MemoryAllocator* memory_allocator) {
  Status status;
  Footer footer;
  status = ReadFooterFromFile(file, prefetch_buffer, file_size, &footer,
                              table_magic_number);
  if (!status.ok()) {
    return status;
  }

  // Reading metaindex block
  auto metaindex_handle = footer.metaindex_handle();
  BlockContents metaindex_contents;
  ReadOptions read_options;
  read_options.verify_checksums = false;
  Slice compression_dict;
  PersistentCacheOptions cache_options;

  BlockFetcher block_fetcher(file, prefetch_buffer, footer, read_options,
                             metaindex_handle, &metaindex_contents, ioptions,
                             false /* decompress */, false /*maybe_compressed*/,
                             compression_dict, cache_options, memory_allocator);
  status = block_fetcher.ReadBlockContents();
  if (!status.ok()) {
    return status;
  }
  // meta block is never compressed. Need to add uncompress logic if we are to
  // compress it.

  // Finding metablock
  Block metaindex_block(std::move(metaindex_contents),
                        kDisableGlobalSequenceNumber);

  std::unique_ptr<InternalIteratorBase<Slice>> meta_iter;
  meta_iter.reset(metaindex_block.NewIterator<DataBlockIter>(
      BytewiseComparator(), BytewiseComparator()));

  BlockHandle block_handle;
  status = FindMetaBlock(meta_iter.get(), meta_block_name, &block_handle);

  if (!status.ok()) {
    return status;
  }

  // Reading metablock
  BlockFetcher block_fetcher2(
      file, prefetch_buffer, footer, read_options, block_handle, contents,
      ioptions, false /* decompress */, false /*maybe_compressed*/,
      compression_dict, cache_options, memory_allocator);
  return block_fetcher2.ReadBlockContents();
}

}  // namespace TERARKDB_NAMESPACE
