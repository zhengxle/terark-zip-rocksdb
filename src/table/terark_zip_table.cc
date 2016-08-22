/*
 * terark_zip_table.cc
 *
 *  Created on: 2016��8��9��
 *      Author: leipeng
 */

#include "terark_zip_table.h"
#include <rocksdb/comparator.h>
#include <rocksdb/env.h>
#include <rocksdb/options.h>
#include <rocksdb/table.h>
#include <table/get_context.h>
#include <table/internal_iterator.h>
#include <table/table_builder.h>
#include <table/table_reader.h>
#include <table/meta_blocks.h>
#include <terark/stdtypes.hpp>
#include <terark/util/blob_store.hpp>
#include <terark/util/throw.hpp>
#include <terark/fast_zip_blob_store.hpp>
#include <terark/fsa/nest_louds_trie.hpp>
#include <terark/fsa/nest_trie_dawg.hpp>
#include <terark/io/FileStream.hpp>
#include <terark/io/MemStream.hpp>
#include <terark/io/StreamBuffer.hpp>
#include <terark/io/DataIO.hpp>
#include <memory>
#include <random>
#include <stdlib.h>
#include <stdint.h>

namespace terark { namespace fsa {

}} // namespace terark::fsa

namespace rocksdb {

using std::unique_ptr;
using std::unordered_map;
using std::vector;

using terark::NestLoudsTrieDAWG_SE_512;
using terark::DictZipBlobStore;
using terark::byte_t;
using terark::valvec;
using terark::valvec_no_init;
using terark::valvec_reserve;
using terark::fstring;
using terark::initial_state;
using terark::MemIO;
using terark::AutoGrownMemIO;
using terark::FileStream;
using terark::InputBuffer;
using terark::OutputBuffer;
using terark::LittleEndianDataInput;
using terark::LittleEndianDataOutput;
using terark::SortableStrVec;
using terark::var_uint32_t;
using terark::UintVecMin0;

static const uint64_t kTerarkZipTableMagicNumber = 0x1122334455667788;

static const std::string kTerarkZipTableIndexBlock = "TerarkZipTableIndexBlock";
static const std::string kTerarkZipTableValueTypeBlock = "TerarkZipTableValueTypeBlock";
static const std::string kTerarkZipTableValueDictBlock = "TerarkZipTableValueDictBlock";

class TerarkZipTableIterator;

#if defined(IOS_CROSS_COMPILE)
  #define MY_THREAD_LOCAL(Type, Var)  Type Var
#elif defined(_WIN32)
  #define MY_THREAD_LOCAL(Type, Var)  static __declspec(thread) Type Var
#else
//#define MY_THREAD_LOCAL(Type, Var)  static __thread Type Var
  #define MY_THREAD_LOCAL(Type, Var)  static thread_local Type Var
#endif

MY_THREAD_LOCAL(terark::MatchContext, g_mctx);
MY_THREAD_LOCAL(valvec<byte_t>, g_tbuf);

enum class ZipValueType : unsigned char {
	kZeroSeq = 0,
	kDelete = 1,
	kValue = 2,
	kMulti = 3,
};
const size_t kZipValueTypeBits = 2;

struct ZipValueMultiValue {
	uint32_t num;
	uint32_t offsets[1];

	Slice getValueData(size_t nth) const {
		assert(nth < num);
		size_t offset0 = offsets[nth+0];
		size_t offset1 = offsets[nth+1];
		size_t dlength = offset1 - offset0;
		const char* base = (const char*)(offsets + num + 1);
		return Slice(base + offset0, dlength);
	}
	static size_t calcHeaderSize(size_t n) {
		return sizeof(uint32_t) * (n + 2);
	}
};

template<class ByteArray>
Slice SliceOf(const ByteArray& ba) {
	BOOST_STATIC_ASSERT(sizeof(ba[0] == 1));
	return Slice((const char*)ba.data(), ba.size());
}

inline static fstring fstringOf(const Slice& x) {
	return fstring(x.data(), x.size());
}

/**
 * one user key map to a record id: the index NO. of a key in NestLoudsTrie,
 * the record id is used to direct index a type enum(small integer) array,
 * the record id is also used to access the value store
 */

class TerarkZipTableReader: public TableReader {
public:
  static Status Open(const ImmutableCFOptions& ioptions,
                     const EnvOptions& env_options,
                     RandomAccessFileReader* file,
                     uint64_t file_size,
					 unique_ptr<TableReader>* table);

  InternalIterator*
  NewIterator(const ReadOptions&, Arena*, bool skip_filters) override;

  void Prepare(const Slice& target) override;

  Status Get(const ReadOptions&, const Slice& key, GetContext*,
             bool skip_filters) override;

  uint64_t ApproximateOffsetOf(const Slice& key) override;

  void SetupForCompaction() override;

  std::shared_ptr<const TableProperties>
  GetTableProperties() const override;

  size_t ApproximateMemoryUsage() const override;

  ~TerarkZipTableReader();
  TerarkZipTableReader(size_t user_key_len, const ImmutableCFOptions& ioptions);

  Status GetRecId(const Slice& userKey, size_t* pRecId) const;
  void GetValue(size_t recId, valvec<byte_t>* value) const;

private:
  unique_ptr<DictZipBlobStore> valstore_;
  unique_ptr<NestLoudsTrieDAWG_SE_512> keyIndex_;
  terark::UintVecMin0 typeArray_;
  const size_t fixed_key_len_;
  static const size_t kNumInternalBytes = 8;
  Slice  file_data_;
  unique_ptr<RandomAccessFileReader> file_;

  const ImmutableCFOptions& ioptions_;
  uint64_t file_size_ = 0;
  std::shared_ptr<const TableProperties> table_properties_;

  friend class TerarkZipTableIterator;

  Status LoadIndex(Slice mem);

  // No copying allowed
  explicit TerarkZipTableReader(const TerarkZipTableReader&) = delete;
  void operator=(const TerarkZipTableReader&) = delete;
};

class TerarkZipTableBuilder: public TableBuilder {
public:
  TerarkZipTableBuilder(
		  const TerarkZipTableOptions&,
		  const ImmutableCFOptions& ioptions,
		  const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*,
		  uint32_t column_family_id,
		  WritableFileWriter* file,
		  const std::string& column_family_name);

  // REQUIRES: Either Finish() or Abandon() has been called.
  ~TerarkZipTableBuilder();

  // Add key,value to the table being constructed.
  // REQUIRES: key is after any previously added key according to comparator.
  // REQUIRES: Finish(), Abandon() have not been called
  void Add(const Slice& key, const Slice& value) override;

  // Return non-ok iff some error has been detected.
  Status status() const override;

  // Finish building the table.  Stops using the file passed to the
  // constructor after this function returns.
  // REQUIRES: Finish(), Abandon() have not been called
  Status Finish() override;

  // Indicate that the contents of this builder should be abandoned.  Stops
  // using the file passed to the constructor after this function returns.
  // If the caller is not going to call Finish(), it must call Abandon()
  // before destroying this builder.
  // REQUIRES: Finish(), Abandon() have not been called
  void Abandon() override;

  // Number of calls to Add() so far.
  uint64_t NumEntries() const override;

  // Size of the file generated so far.  If invoked after a successful
  // Finish() call, returns the size of the final generated file.
  uint64_t FileSize() const override;

  TableProperties GetTableProperties() const override { return properties_; }

private:
  Arena arena_;
  const TerarkZipTableOptions& table_options_;
  const ImmutableCFOptions& ioptions_;
  std::vector<unique_ptr<IntTblPropCollector>> table_properties_collectors_;

  unique_ptr<DictZipBlobStore::ZipBuilder> zbuilder_;
  unique_ptr<DictZipBlobStore> zstore_;
  valvec<byte_t> prevUserKey_;
  terark::febitvec valueBits_;
  std::string tmpValueFilePath_;
  FileStream  tmpValueFile_;
  NativeDataOutput<OutputBuffer> tmpValueWriter_;
  SortableStrVec tmpKeyVec_;
  std::mt19937_64 randomGenerator_;
  uint64_t sampleUpperBound_;
  size_t numUserKeys_ = 0;
  size_t sampleLenSum_ = 0;

  WritableFileWriter* file_;
  uint64_t offset_ = 0;
  size_t huge_page_tlb_size_;
  Status status_;
  TableProperties properties_;

  std::vector<uint32_t> keys_or_prefixes_hashes_;
  bool closed_ = false;  // Either Finish() or Abandon() has been called.

  // No copying allowed
  TerarkZipTableBuilder(const TerarkZipTableBuilder&) = delete;
  void operator=(const TerarkZipTableBuilder&) = delete;
};

///////////////////////////////////////////////////////////////////////////////

class TerarkZipTableIterator : public InternalIterator {
public:
  explicit TerarkZipTableIterator(TerarkZipTableReader* table) {
	  table_ = table;
	  status_ = Status::InvalidArgument("TerarkZipTableIterator",
			  "Not point to a position");
	  auto dfa = table->keyIndex_.get();
	  iter_.reset(dfa->adfa_make_iter());
	  zValtype_ = ZipValueType::kZeroSeq;
	  pInterKey_.sequence = uint64_t(-1);
	  pInterKey_.type = kMaxValue;
	  recId_ = size_t(-1);
	  valnum_ = 0;
	  validx_ = 0;
  }
  ~TerarkZipTableIterator() {
  }

  bool Valid() const override {
	  return status_.ok();
  }

  void SeekToFirst() override {
	  if (UnzipIterRecord(iter_->seek_begin())) {
		  DecodeCurrKeyValue();
		  validx_ = 1;
	  }
  }

  void SeekToLast() override {
	  if (UnzipIterRecord(iter_->seek_end())) {
		  validx_ = valnum_ - 1;
		  DecodeCurrKeyValue();
	  }
  }

  void Seek(const Slice& target) override {
	  ParsedInternalKey pikey;
	  if (!ParseInternalKey(target, &pikey)) {
		  status_ = Status::InvalidArgument("TerarkZipTableIterator::Seek()",
				  "param target.size() < 8");
		  return;
	  }
	  if (UnzipIterRecord(iter_->seek_lower_bound(fstringOf(pikey.user_key)))) {
		  do {
			  DecodeCurrKeyValue();
			  validx_++;
			  if (pInterKey_.sequence <= pikey.sequence) {
				  return; // done
			  }
		  } while (validx_ < valnum_);
		  // no visible version/sequence for target, use Next();
		  // if using Next(), version check is not needed
		  Next();
	  }
  }

  void Next() override {
	  if (validx_ < valnum_) {
		  DecodeCurrKeyValue();
		  validx_++;
	  }
	  else {
		  if (UnzipIterRecord(iter_->incr())) {
			  DecodeCurrKeyValue();
			  validx_ = 1;
		  }
	  }
  }

  void Prev() override {
	  if (validx_ > 0) {
		  validx_--;
		  DecodeCurrKeyValue();
	  }
	  else {
		  if (UnzipIterRecord(iter_->decr())) {
			  validx_ = valnum_ - 1;
			  DecodeCurrKeyValue();
		  }
	  }
  }

  Slice key() const override {
	  assert(status_.ok());
	  return pInterKey_.user_key;
  }

  Slice value() const override {
	  assert(status_.ok());
	  return userValue_;
  }

  Status status() const override {
	  return status_;
  }

private:
  size_t GetIterRecId() const {
	  auto dfa = table_->keyIndex_.get();
	  return dfa->state_to_word_id(iter_->word_state());
  }
  bool UnzipIterRecord(bool hasRecord) {
	  validx_ = 0;
	  if (hasRecord) {
		  size_t recId = GetIterRecId();
		  table_->GetValue(recId, &valueBuf_);
		  status_ = Status::OK();
		  zValtype_ = ZipValueType(table_->typeArray_[recId]);
		  if (ZipValueType::kMulti == zValtype_) {
			  auto zmValue = (ZipValueMultiValue*)(valueBuf_.data());
			  assert(zmValue->num > 0);
			  valnum_ = zmValue->num;
		  } else {
			  valnum_ = 1;
		  }
		  recId_ = recId;
		  pInterKey_.user_key = SliceOf(iter_->word());
		  return true;
	  }
	  else {
		  recId_ = size_t(-1);
		  valnum_ = 0;
		  status_ = Status::NotFound();
		  pInterKey_.user_key = Slice();
		  return false;
	  }
  }
  void DecodeCurrKeyValue() {
	assert(status_.ok());
	assert(recId_ < table_->keyIndex_->num_words());
	switch (zValtype_) {
	default:
		status_ = Status::Aborted("TerarkZipTableReader::Get()",
				"Bad ZipValueType");
		abort(); // must not goes here, if it does, it should be a bug!!
		break;
	case ZipValueType::kZeroSeq:
		pInterKey_.sequence = 0;
		pInterKey_.type = kTypeValue;
		userValue_ = SliceOf(valueBuf_);
		break;
	case ZipValueType::kValue: // should be a kTypeValue, the normal case
		// little endian uint64_t
		pInterKey_.sequence = *(uint64_t*)valueBuf_.data() & kMaxSequenceNumber;
		pInterKey_.type = kTypeValue;
		userValue_ = SliceOf(fstring(valueBuf_).substr(7));
		break;
	case ZipValueType::kDelete:
		// little endian uint64_t
		pInterKey_.sequence = *(uint64_t*)valueBuf_.data() & kMaxSequenceNumber;
		pInterKey_.type = kTypeDeletion;
		userValue_ = Slice();
		break;
	case ZipValueType::kMulti: { // more than one value
		auto zmValue = (const ZipValueMultiValue*)(valueBuf_.data());
		assert(0 != valnum_);
		assert(validx_ < valnum_);
		assert(valnum_ == zmValue->num);
		Slice d = zmValue->getValueData(validx_);
		auto snt = unaligned_load<SequenceNumber>(d.data());
		UnPackSequenceAndType(snt, &pInterKey_.sequence, &pInterKey_.type);
		d.remove_prefix(sizeof(SequenceNumber));
		userValue_ = d;
		break; }
	}
  }

  TerarkZipTableReader* table_;
  unique_ptr<terark::ADFA_LexIterator> iter_;
  ParsedInternalKey pInterKey_;
  valvec<byte_t> valueBuf_;
  Slice  userValue_;
  ZipValueType zValtype_;
  size_t recId_; // save as member to reduce a rank1(state)
  size_t valnum_;
  size_t validx_;
  Status status_;
  // No copying allowed
  TerarkZipTableIterator(const TerarkZipTableIterator&) = delete;
  void operator=(const TerarkZipTableIterator&) = delete;
};

TerarkZipTableReader::~TerarkZipTableReader() {
	typeArray_.risk_release_ownership();
}

TerarkZipTableReader::TerarkZipTableReader(size_t user_key_len,
							const ImmutableCFOptions& ioptions)
 : fixed_key_len_(user_key_len), ioptions_(ioptions) {}

Status
TerarkZipTableReader::Open(const ImmutableCFOptions& ioptions,
						   const EnvOptions& env_options,
						   RandomAccessFileReader* file,
						   uint64_t file_size,
						   unique_ptr<TableReader>* table) {
  TableProperties* props = nullptr;
  Status s = ReadTableProperties(file, file_size,
		  	  kTerarkZipTableMagicNumber, ioptions, &props);
  if (!s.ok()) {
	return s;
  }
  assert(nullptr != props);
  unique_ptr<TableProperties> uniqueProps(props);
  Slice file_data;
  if (env_options.use_mmap_reads) {
	s = file->Read(0, file_size, &file_data, nullptr);
	if (!s.ok())
		return s;
  } else {
	return Status::InvalidArgument("TerarkZipTableReader::Open()",
			"EnvOptions::use_mmap_reads must be true");
  }
  unique_ptr<TerarkZipTableReader>
  r(new TerarkZipTableReader(size_t(props->fixed_key_len), ioptions));
  r->file_.reset(file);
  r->file_data_ = file_data;
  r->file_size_ = file_size;
  r->table_properties_.reset(uniqueProps.release());
  BlockContents valueDictBlock, indexBlock, zValueTypeBlock;
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
		  kTerarkZipTableValueDictBlock, &valueDictBlock);
  if (!s.ok()) {
	  return s;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
		  kTerarkZipTableIndexBlock, &indexBlock);
  if (!s.ok()) {
	  return s;
  }
  s = ReadMetaBlock(file, file_size, kTerarkZipTableMagicNumber, ioptions,
		  kTerarkZipTableValueTypeBlock, &zValueTypeBlock);
  if (!s.ok()) {
	  return s;
  }
  r->valstore_.reset(new DictZipBlobStore());
  r->valstore_->load_user_memory(
		  fstringOf(valueDictBlock.data),
		  fstring(file_data.data(), props->data_size));
  s = r->LoadIndex(indexBlock.data);
  if (!s.ok()) {
	  return s;
  }
  r->typeArray_.risk_set_data((byte_t*)zValueTypeBlock.data.data(),
		  zValueTypeBlock.data.size(), kZipValueTypeBits);
  *table = std::move(r);
  return Status::OK();
}

Status TerarkZipTableReader::LoadIndex(Slice mem) {
  try {
	  auto trie = terark::BaseDFA::load_mmap_range(mem.data(), mem.size());
	  keyIndex_.reset(dynamic_cast<NestLoudsTrieDAWG_SE_512*>(trie));
	  if (!keyIndex_) {
		  return Status::InvalidArgument("TerarkZipTableReader::Open()",
				  "Index class is not NestLoudsTrieDAWG_SE_512");
	  }
  }
  catch (const std::exception& ex) {
	  return Status::InvalidArgument("TerarkZipTableReader::Open()", ex.what());
  }
  return Status::OK();
}

InternalIterator*
TerarkZipTableReader::
NewIterator(const ReadOptions& ro, Arena* arena, bool skip_filters) {
	(void)skip_filters; // unused
	if (arena) {
		return new(arena->AllocateAligned(sizeof(TerarkZipTableIterator)))
				TerarkZipTableIterator(this);
	}
	else {
		return new TerarkZipTableIterator(this);
	}
}

void TerarkZipTableReader::Prepare(const Slice& target) {
	// do nothing
}

Status
TerarkZipTableReader::Get(const ReadOptions& ro, const Slice& ikey,
						  GetContext* get_context, bool skip_filters) {
	ParsedInternalKey pikey;
	ParseInternalKey(ikey, &pikey);
	size_t recId;
	{
		Status s = GetRecId(pikey.user_key, &recId);
		if (!s.ok()) {
			return s;
		}
	}
	valstore_->get_record(recId, &g_tbuf);
	switch (ZipValueType(typeArray_[recId])) {
	default:
		return Status::Aborted("TerarkZipTableReader::Get()",
				"Bad ZipValueType");
	case ZipValueType::kZeroSeq:
		get_context->SaveValue(Slice((char*)g_tbuf.data(), g_tbuf.size()), 0);
		return Status::OK();
	case ZipValueType::kValue: { // should be a kTypeValue, the normal case
		// little endian uint64_t
		uint64_t seq = *(uint64_t*)g_tbuf.data() & kMaxSequenceNumber;
		if (seq <= pikey.sequence) {
			get_context->SaveValue(SliceOf(fstring(g_tbuf).substr(7)), seq);
		}
		return Status::OK(); }
	case ZipValueType::kDelete: {
		// little endian uint64_t
		uint64_t seq = *(uint64_t*)g_tbuf.data() & kMaxSequenceNumber;
		if (seq <= pikey.sequence) {
			get_context->SaveValue(
				ParsedInternalKey(pikey.user_key, seq, kTypeDeletion),
				Slice());
		}
		return Status::OK(); }
	case ZipValueType::kMulti: { // more than one value
		auto mVal = (const ZipValueMultiValue*)g_tbuf.data();
		const size_t num = mVal->num;
		for(size_t i = 0; i < num; ++i) {
			Slice val = mVal->getValueData(i);
			SequenceNumber sn;
			ValueType valtype;
			{
				auto snt = unaligned_load<SequenceNumber>(val.data());
				UnPackSequenceAndType(snt, &sn, &valtype);
			}
			if (sn <= pikey.sequence) {
				val.remove_prefix(sizeof(SequenceNumber));
				// only kTypeMerge will return true
				bool hasMoreValue = get_context->SaveValue(
					ParsedInternalKey(pikey.user_key, sn, valtype), val);
				if (!hasMoreValue) {
					break;
				}
			}
		}
		return Status::OK(); }
	}
}

uint64_t TerarkZipTableReader::ApproximateOffsetOf(const Slice& key) {
	return 0;
}

void TerarkZipTableReader::SetupForCompaction() {
}

std::shared_ptr<const TableProperties>
TerarkZipTableReader::GetTableProperties() const {
  return table_properties_;
}

size_t TerarkZipTableReader::ApproximateMemoryUsage() const {
  return file_size_;
}

Status
TerarkZipTableReader::GetRecId(const Slice& userKey, size_t* pRecId) const {
	auto dfa = keyIndex_.get();
	const size_t  kn = userKey.size();
	const byte_t* kp = (const byte_t*)userKey.data();
	size_t state = initial_state;
	g_mctx.zbuf_state = size_t(-1);
	for (size_t pos = 0; pos < kn; ++pos) {
		if (dfa->is_pzip(state)) {
			fstring zs = dfa->get_zpath_data(state, &g_mctx);
			if (kn - pos < zs.size()) {
				return Status::NotFound("TerarkZipTableReader::Get()",
						"zpath is longer than remaining key");
			}
			for (size_t j = 0; j < zs.size(); ++j, ++pos) {
				if (zs[j] != kp[pos]) {
					return Status::NotFound("TerarkZipTableReader::Get()",
							"zpath match fail");
				}
			}
			if (pos == kn)
				break;
		}
		byte_t c = kp[pos];
		size_t next = dfa->state_move(state, c);
		if (dfa->nil_state == next) {
			return Status::NotFound("TerarkZipTableReader::Get()",
					"reached nil_state");
		}
		assert(next < dfa->total_states());
		state = next;
	}
	if (!dfa->is_term(state)) {
		return Status::NotFound("TerarkZipTableReader::Get()",
				"input key is a prefix but is not a dfa key");
	}
	*pRecId = dfa->state_to_word_id(state);
	return Status::OK();
}

void
TerarkZipTableReader::GetValue(size_t recId, valvec<byte_t>* value) const {
	assert(recId < keyIndex_->num_words());
	valstore_->get_record(recId, value);
}

///////////////////////////////////////////////////////////////////////////////

TerarkZipTableBuilder::TerarkZipTableBuilder(
		const TerarkZipTableOptions& table_options,
		const ImmutableCFOptions& ioptions,
		const std::vector<std::unique_ptr<IntTblPropCollectorFactory>>*,
		uint32_t column_family_id,
		WritableFileWriter* file,
		const std::string& column_family_name)
  : table_options_(table_options)
  , ioptions_(ioptions)
{
  huge_page_tlb_size_ = 0;
  file_ = nullptr;
  status_ = Status::OK();
  zstore_.reset(new DictZipBlobStore());
  zbuilder_.reset(zstore_->createZipBuilder());
  numUserKeys_ = 0;
  sampleUpperBound_ = randomGenerator_.max() * table_options_.sampleRatio;
  tmpValueFilePath_ = table_options.localTempDir;
  tmpValueFilePath_.append("/TerarkRocks-XXXXXX");
  int fd = mkstemp(&tmpValueFilePath_[0]);
  if (fd < 0) {
	int err = errno;
	THROW_STD(invalid_argument
	  , "ERROR: TerarkZipTableBuilder::TerarkZipTableBuilder(): mkstemp(%s) = %s\n"
	  , tmpValueFilePath_.c_str(), strerror(err));
  }
  tmpValueFile_.dopen(fd, "rb+");
  tmpValueWriter_.attach(&tmpValueFile_);
}

TerarkZipTableBuilder::~TerarkZipTableBuilder() {
}

void TerarkZipTableBuilder::Add(const Slice& key, const Slice& value) {
	assert(key.size() >= 8);
	fstring userKey(key.data(), key.size()-8);
	valueBits_.push_back(true);
	if (prevUserKey_ != userKey) {
		assert(prevUserKey_ < userKey);
		if (table_options_.fixed_key_len) {
			tmpKeyVec_.m_strpool.append(userKey);
		} else {
			tmpKeyVec_.push_back(userKey);
		}
		prevUserKey_.assign(userKey);
		valueBits_.push_back(false);
		numUserKeys_++;
	}
	else if (terark_unlikely(0 == numUserKeys_)) {
		assert(userKey.empty());
		numUserKeys_++;
	}
	if (!value.empty() && randomGenerator_() < sampleUpperBound_) {
		zbuilder_->addSample(fstringOf(value));
		sampleLenSum_ += value.size();
	}
	tmpValueWriter_.ensureWrite(userKey.end(), 8);
	tmpValueWriter_ << fstringOf(value);
	properties_.num_entries++;
}

Status TerarkZipTableBuilder::status() const {
	return status_;
}

template<class ByteArray>
static
Status WriteBlock(const ByteArray& blockData, WritableFileWriter* file,
                  uint64_t* offset, BlockHandle* block_handle) {
  block_handle->set_offset(*offset);
  block_handle->set_size(blockData.size());
  Status s = file->Append(SliceOf(blockData));
  if (s.ok()) {
    *offset += blockData.size();
  }
  return s;
}

Status TerarkZipTableBuilder::Finish() {
	assert(0 == table_options_.fixed_key_len);
	assert(!closed_);
	closed_ = true;

	if (0 == sampleLenSum_) { // prevent from empty
		zbuilder_->addSample("Hello World!");
	}

	// the guard, if last same key seq is longer than 1, this is required
	valueBits_.push_back(false);
	tmpValueWriter_.flush();
	tmpValueFile_.rewind();
	unique_ptr<NestLoudsTrieDAWG_SE_512> dawg(new NestLoudsTrieDAWG_SE_512());
	terark::NestLoudsTrieConfig conf;
	conf.nestLevel = table_options_.indexNestLevel;
	dawg->build_from(tmpKeyVec_, conf);
	assert(dawg->num_words() == numUserKeys_);
	tmpKeyVec_.clear();
	dawg->save_mmap(tmpValueFilePath_ + ".index");
	dawg.reset(); // free memory
	zbuilder_->prepare(properties_.num_entries, tmpValueFilePath_ + ".zbs");
	NativeDataInput<InputBuffer> input(&tmpValueFile_);
	UintVecMin0 zvType(properties_.num_entries, kZipValueTypeBits);
	valvec<byte_t> value;
	valvec<byte_t> mValue;
	size_t entryId = 0;
	size_t bitPos = 0;
	size_t recId = 0;
	for (; recId < numUserKeys_; recId++) {
		uint64_t seqType = input.load_as<uint64_t>();
		uint64_t seqNum;
		ValueType vType;
		UnPackSequenceAndType(seqType, &seqNum, &vType);
		input >> value;
		size_t oneSeqLen = valueBits_.one_seq_len(bitPos);
		assert(oneSeqLen >= 1);
		if (1==oneSeqLen && (kTypeDeletion==vType || kTypeValue==vType)) {
			if (0 == seqNum && kTypeValue==vType) {
				zvType.set_wire(recId, size_t(ZipValueType::kZeroSeq));
			} else {
				if (kTypeValue==vType) {
					zvType.set_wire(recId, size_t(ZipValueType::kValue));
				} else {
					zvType.set_wire(recId, size_t(ZipValueType::kDelete));
				}
				value.insert(0, (byte_t*)&seqNum, 7);
			}
			zbuilder_->addRecord(value);
		}
		else {
			zvType.set_wire(recId, size_t(ZipValueType::kMulti));
			size_t headerSize = ZipValueMultiValue::calcHeaderSize(oneSeqLen);
			mValue.erase_all();
			mValue.resize(headerSize);
			((ZipValueMultiValue*)mValue.data())->num = oneSeqLen;
			((ZipValueMultiValue*)mValue.data())->offsets[0] = 0;
			for (size_t j = 0; j < oneSeqLen; j++) {
				if (j > 0) {
					seqType = input.load_as<uint64_t>();
					input >> value;
				}
				mValue.append((byte_t*)&seqType, 8);
				mValue.append(value);
				((ZipValueMultiValue*)mValue.data())->offsets[j+1] = mValue.size() - headerSize;
			}
			zbuilder_->addRecord(mValue);
		}
		bitPos += oneSeqLen + 1;
		entryId += oneSeqLen;
	}
	assert(entryId == properties_.num_entries);
	zstore_->completeBuild(*zbuilder_);
	zbuilder_.reset();
	value.clear();
	mValue.clear();
	try{auto trie = terark::BaseDFA::load_mmap(tmpValueFilePath_ + ".index");
		dawg.reset(dynamic_cast<NestLoudsTrieDAWG_SE_512*>(trie));
	} catch (const std::exception&) {}
	if (!dawg) {
		return Status::InvalidArgument("TerarkZipTableBuilder::Finish()",
				"index temp file is broken");
	}
#if 0
	{
		// reorder word id from byte lex order to LoudsTrie order
		terark::AutoFree<uint32_t> newToOld(numUserKeys_, UINT32_MAX);
		UintVecMin0 zvType2(numUserKeys_, kZipValueTypeBits);
		dawg->tpl_for_each_word([&](size_t nth, fstring key, size_t state) {
			(void)key; // unused
			size_t newId = dawg->state_to_word_id(state);
			size_t oldId = nth;
			newToOld[newId] = oldId;
			zvType2[newId] = zvType[oldId];
		});
		zvType.clear();
		zvType.swap(zvType2);
		std::string newFile = tmpValueFilePath_ + ".zbs.new";
		bool keepOldFiles = false;
		zstore_->reorder_and_load(newToOld.p, newFile, keepOldFiles);
	}
#else
	{
		// reorder word id from byte lex order to LoudsTrie order without
		// using mapping array 'newToOld'.
		// zstore_->reorder_and_load() will call generateMap, generateMap
		// generate all (newId, oldId) mappings and feed the mappings to
		// 'doMap', 'doMap' is implemented in dawg->reorder_and_load
		UintVecMin0 zvType2(numUserKeys_, kZipValueTypeBits);
		std::string newFile = tmpValueFilePath_ + ".zbs.new";
		bool keepOldFiles = false;
		auto generateMap =
		[&](const std::function<void(size_t newId, size_t oldId)>& doMap) {
			terark::NonRecursiveDictionaryOrderToStateMapGenerator gen;
			gen(*dawg, [&](size_t byteLexNth, size_t state) {
				size_t newId = dawg->state_to_word_id(state);
				size_t oldId = byteLexNth;
				doMap(newId, oldId);
				zvType2.set_wire(newId, zvType[oldId]);
			});
		};
		zstore_->reorder_and_load(generateMap, newFile, keepOldFiles);
		zvType.clear();
		zvType.swap(zvType2);
	}
#endif
	BlockHandle dataBlock, dictBlock, indexBlock, zvTypeBlock;
	offset_ = 0;
	Status s = WriteBlock(zstore_->get_data(), file_, &offset_, &dataBlock);
	if (!s.ok()) {
		return s;
	}
	s = WriteBlock(zstore_->get_dict(), file_, &offset_, &dictBlock);
	if (!s.ok()) {
		return s;
	}
	s = WriteBlock(dawg->get_mmap(), file_, &offset_, &indexBlock);
	if (!s.ok()) {
		return s;
	}
	fstring zvTypeMem(zvType.data(), zvType.mem_size());
	s = WriteBlock(zvTypeMem, file_, &offset_, &zvTypeBlock);
	if (!s.ok()) {
		return s;
	}
	MetaIndexBuilder metaindexBuiler;
	metaindexBuiler.Add(kTerarkZipTableValueDictBlock, dictBlock);
	metaindexBuiler.Add(kTerarkZipTableIndexBlock, indexBlock);
	metaindexBuiler.Add(kTerarkZipTableValueTypeBlock, zvTypeBlock);
	PropertyBlockBuilder propBlockBuilder;
	propBlockBuilder.AddTableProperty(properties_);
	propBlockBuilder.Add(properties_.user_collected_properties);
	NotifyCollectTableCollectorsOnFinish(table_properties_collectors_,
	                                     ioptions_.info_log,
	                                     &propBlockBuilder);
	BlockHandle propBlock, metaindexBlock;
	s = WriteBlock(propBlockBuilder.Finish(), file_, &offset_, &propBlock);
	if (!s.ok()) {
		return s;
	}
	metaindexBuiler.Add(kPropertiesBlock, propBlock);
	s = WriteBlock(metaindexBuiler.Finish(), file_, &offset_, &metaindexBlock);
	if (!s.ok()) {
		return s;
	}
	Footer footer(kTerarkZipTableMagicNumber, 0);
	footer.set_metaindex_handle(metaindexBlock);
	footer.set_index_handle(BlockHandle::NullBlockHandle());
	std::string footer_encoding;
	footer.EncodeTo(&footer_encoding);
	s = file_->Append(footer_encoding);
	if (s.ok()) {
		offset_ += footer_encoding.size();
	}
	return s;
}

void TerarkZipTableBuilder::Abandon() {
	closed_ = true;
}

uint64_t TerarkZipTableBuilder::NumEntries() const {
	return properties_.num_entries;
}

uint64_t TerarkZipTableBuilder::FileSize() const {
	return offset_;
}

/////////////////////////////////////////////////////////////////////////////

class TerarkZipTableFactory : public TableFactory {
 public:
  ~TerarkZipTableFactory();
  explicit
  TerarkZipTableFactory(const TerarkZipTableOptions& = TerarkZipTableOptions());

  const char* Name() const override;

  Status
  NewTableReader(const TableReaderOptions& table_reader_options,
                 unique_ptr<RandomAccessFileReader>&& file,
                 uint64_t file_size,
				 unique_ptr<TableReader>* table,
				 bool prefetch_index_and_filter_in_cache) const override;

  TableBuilder*
  NewTableBuilder(const TableBuilderOptions& table_builder_options,
				  uint32_t column_family_id,
				  WritableFileWriter* file) const override;

  std::string GetPrintableTableOptions() const override;

  const TerarkZipTableOptions& table_options() const;

  // Sanitizes the specified DB Options.
  Status SanitizeOptions(const DBOptions& db_opts,
                         const ColumnFamilyOptions& cf_opts) const override;

  void* GetOptions() override;

 private:
  TerarkZipTableOptions table_options_;
};

class TableFactory* NewTerarkZipTableFactory(const TerarkZipTableOptions& opt) {
	return new TerarkZipTableFactory(opt);
}

TerarkZipTableFactory::~TerarkZipTableFactory() {
}

TerarkZipTableFactory::TerarkZipTableFactory(const TerarkZipTableOptions& tzto)
  : table_options_(tzto)
{
}

const char*
TerarkZipTableFactory::Name() const { return "TerarkZipTable"; }

inline static
bool IsBytewiseComparator(const Comparator* cmp) {
#if 1
	return fstring(cmp->Name()) == "leveldb.BytewiseComparator";
#else
	return BytewiseComparator() == cmp;
#endif
}
inline static
bool IsBytewiseComparator(const InternalKeyComparator& icmp) {
	return IsBytewiseComparator(icmp.user_comparator());
}

Status
TerarkZipTableFactory::NewTableReader(
		const TableReaderOptions& table_reader_options,
		unique_ptr<RandomAccessFileReader>&& file,
		uint64_t file_size, unique_ptr<TableReader>* table,
		bool prefetch_index_and_filter_in_cache)
const {
	(void)prefetch_index_and_filter_in_cache; // unused
	if (!IsBytewiseComparator(table_reader_options.internal_comparator)) {
		return Status::InvalidArgument("TerarkZipTableFactory::NewTableReader()",
				"user comparator must be 'leveldb.BytewiseComparator'");
	}
	return TerarkZipTableReader::Open(
			table_reader_options.ioptions,
			table_reader_options.env_options,
			file.release(),
			file_size,
			table);
}

TableBuilder*
TerarkZipTableFactory::NewTableBuilder(
		const TableBuilderOptions& table_builder_options,
		uint32_t column_family_id,
		WritableFileWriter* file)
const {
	if (!IsBytewiseComparator(table_builder_options.internal_comparator)) {
		THROW_STD(invalid_argument,
				"TerarkZipTableFactory::NewTableBuilder(): "
				"user comparator must be 'leveldb.BytewiseComparator'");
	}
	return new TerarkZipTableBuilder(
			table_options_,
		    table_builder_options.ioptions,
		    table_builder_options.int_tbl_prop_collector_factories,
			column_family_id,
		    file,
		    table_builder_options.column_family_name);
}

std::string
TerarkZipTableFactory::GetPrintableTableOptions() const {
  std::string ret;
  ret.reserve(20000);
  const int kBufferSize = 200;
  char buffer[kBufferSize];

  snprintf(buffer, kBufferSize, "  fixed_key_len: %u\n",
		   table_options_.fixed_key_len);
  ret.append(buffer);
  return ret;
}

const TerarkZipTableOptions&
TerarkZipTableFactory::table_options() const {
	return table_options_;
}

// Sanitizes the specified DB Options.
Status
TerarkZipTableFactory::SanitizeOptions(const DBOptions& db_opts,
                       	   	   	   	   const ColumnFamilyOptions& cf_opts)
const {
	if (!IsBytewiseComparator(cf_opts.comparator)) {
		return Status::InvalidArgument("TerarkZipTableFactory::NewTableReader()",
				"user comparator must be 'leveldb.BytewiseComparator'");
	}
	return Status::OK();
}

void*
TerarkZipTableFactory::GetOptions() { return &table_options_; }


} /* namespace rocksdb */