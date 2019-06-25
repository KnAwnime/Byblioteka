#pragma once
#include <ATen/core/ivalue.h>
#include <c10/util/Exception.h>
#include <c10/util/Optional.h>

#include <algorithm>
#include <iostream>
#include <memory>
namespace torch {
namespace jit {

struct SourceRange;
using SourceRangeRecord = std::tuple<size_t, std::shared_ptr<SourceRange>>;
using SourceRangeRecords = std::vector<SourceRangeRecord>;

// Class that keeps track of a serialized debug info table and lazily
// unpacks it on query.
class DebugInfo {
 public:
  using SerializedDebugInfo = std::tuple<at::DataPtr, size_t>;

  DebugInfo() = default;

  DebugInfo(SerializedDebugInfo info) : info_(std::move(info)) {}

  std::shared_ptr<SourceRange> query(const SourceRange& q);

 private:
  void deserialize();

  SerializedDebugInfo info_;
  c10::optional<SourceRangeRecords> deserialized_records_;
};

// Source represents a code segment. It keeps track of:
//  - text : the text of the code segment
//  - filename (optional) : if present, represents the name of the file from
//                          which the code semgemnt originated.
//  - starting_line_no : represents the line in the original file where the
//                       code segment started.
struct Source {
  explicit Source(
      std::string text,
      std::shared_ptr<DebugInfo> debug_info = nullptr)
      : text_(std::move(text)),
        filename_(c10::nullopt),
        debug_info_(std::move(debug_info)) {
    calc_line_start_offsets();
  }

  Source(
      std::string text,
      c10::optional<std::string> filename,
      size_t starting_line_no,
      std::shared_ptr<DebugInfo> debug_info = nullptr)
      : text_(std::move(text)),
        filename_(std::move(filename)),
        starting_line_no_(starting_line_no),
        debug_info_(std::move(debug_info)) {
    calc_line_start_offsets();
  }

  // Given a line number (within source_), return the byte offset of the
  // beginning of that line.
  size_t offset_for_line(size_t line) const {
    return line_starting_offsets_.at(line);
  }

  // Calculate the line (within the code segment) on which `offset` resides.
  size_t lineno_for_offset(size_t offset) const {
    return std::upper_bound(
               line_starting_offsets_.begin(),
               line_starting_offsets_.end(),
               offset) -
        line_starting_offsets_.begin() - 1;
  }

  // Calculate the line (within the original source file, if present) on which
  // `lineno` resides.
  size_t lineno_to_source_lineno(size_t lineno) const {
    if (filename_) {
      return lineno + starting_line_no_;
    } else {
      return lineno;
    }
  }

  const std::string& text() const {
    return text_;
  }

  const c10::optional<std::string>& filename() const {
    return filename_;
  }

  size_t starting_line_no() const {
    return starting_line_no_;
  }

  // Serialize as Tuple[str, Optional[str], int, List[int]]
  std::shared_ptr<c10::IValue> __getstate__() {
    if (!serialized_) {
      std::vector<c10::IValue> elements{
          text_, filename_, (int64_t)starting_line_no_};
      serialized_ =
          std::make_shared<c10::IValue>(c10::ivalue::Tuple::create(elements));
    }
    return serialized_;
  }

  static std::shared_ptr<Source> __setstate__(const c10::IValue& iv) {
    auto tup_elems = iv.toTuple()->elements();
    TORCH_INTERNAL_ASSERT(tup_elems.size() == 3);
    std::string text_ = tup_elems[0].toString()->string();
    c10::optional<std::string> filename_ =
        tup_elems[1].toOptional<std::string>();
    int64_t starting_line_no_ = tup_elems[2].toInt();

    return std::make_shared<Source>(
        std::move(text_), std::move(filename_), starting_line_no_);
  }

  std::shared_ptr<SourceRange> query_debug_info(const SourceRange& sr) const {
    if (!debug_info_) {
      return nullptr;
    }
    return debug_info_->query(sr);
  }

 private:
  void calc_line_start_offsets() {
    size_t pos = 0;
    do {
      line_starting_offsets_.push_back(pos);
      pos++;
    } while ((pos = text_.find('\n', pos)) != std::string::npos);
  }
  std::string text_;
  c10::optional<std::string> filename_;
  // If filename_ is not present, starting_line_no_ is don't care
  int64_t starting_line_no_;
  // Starting offsets for lines into the source. e.g. line 0 starts at
  // line_starting_offsets_[0], etc.
  std::vector<int64_t> line_starting_offsets_;
  // serialized IValue representing this Source
  // Lazily populated, nullptr if not populated
  //
  // NB: if you introduce methods that mutate a Source object, you *must*
  // adjust this caching mechanism accordingly.
  std::shared_ptr<c10::IValue> serialized_ = nullptr;

  std::shared_ptr<DebugInfo> debug_info_;
};

// A SourceRange is a view into a Source, that points to a subset of the source,
// specified by `start` and `end` byte offsets into the source text.
struct CAFFE2_API SourceRange {
  SourceRange(std::shared_ptr<Source> source_, size_t start_, size_t end_)
      : source_(std::move(source_)), start_(start_), end_(end_) {
    TORCH_CHECK(
        end_ >= start_,
        "Attempted to create a source range that starts before it ends!");
  }
  explicit SourceRange(std::string string_range)
      : source_(std::make_shared<Source>(std::move(string_range))),
        start_(0),
        end_(source_->text().size()) {
    TORCH_CHECK(
        end_ >= start_,
        "Attempted to create a source range that starts before it ends!");
  }

  const std::string text() const {
    return source_->text().substr(start(), end() - start());
  }
  int64_t size() const {
    return end() - start();
  }
  static const int64_t CONTEXT = 10;
  void highlight(std::ostream& out) const;
  const std::shared_ptr<Source>& source() const {
    return source_;
  }
  int64_t start() const {
    return start_;
  }
  int64_t end() const {
    return end_;
  }
  std::string str() const {
    std::stringstream ss;
    highlight(ss);
    return ss.str();
  }

  c10::optional<std::tuple<std::string, size_t, size_t>> file_line_col() const {
    if (!source_ || !source()->filename()) {
      return c10::nullopt;
    }

    auto lineno = source_->lineno_for_offset(start_);
    auto col_offset = (int)start_ - (int)source_->offset_for_line(lineno);
    // TODO: c10::optional<>::value returns an rvalue ref so can't use it here??
    return std::make_tuple<std::string, size_t, size_t>(
        source_->filename().value_or(""),
        source_->lineno_to_source_lineno(lineno),
        (size_t)col_offset);
  }

  // Serialize as Tuple[SourceType, int, int]
  // where SourceType = Tuple[str, Optional[str], int, List[int]],
  // the serialized form of Source
  std::shared_ptr<c10::IValue> __getstate__() {
    if (!serialized_) {
      std::vector<c10::IValue> elements = {
          *source_->__getstate__(), start_, end_};
      serialized_ =
          std::make_shared<c10::IValue>(c10::ivalue::Tuple::create(elements));
    }
    return serialized_;
  }

  static std::shared_ptr<SourceRange> __setstate__(const c10::IValue& iv) {
    auto tup_elems = iv.toTuple()->elements();
    TORCH_INTERNAL_ASSERT(tup_elems.size() == 3);
    std::shared_ptr<Source> source_ = Source::__setstate__(tup_elems[0]);
    int64_t start_ = tup_elems[1].toInt();
    int64_t end_ = tup_elems[2].toInt();
    return std::make_shared<SourceRange>(source_, start_, end_);
  }

  std::shared_ptr<SourceRange> orig_range() const {
    // Cache this to prevent log(n) lookup every time.
    if (orig_range_) {
      return orig_range_;
    }
    if (auto orig = source_->query_debug_info(*this)) {
      orig_range_ = orig;
    }
    return orig_range_;
  }

 private:
  std::shared_ptr<Source> source_;
  int64_t start_;
  int64_t end_;
  // serialized IValue representing this SourceRange
  // Lazily populated, nullptr if not populated
  //
  // NB: if you introduce methods that mutate a Source object, you *must*
  // adjust this caching mechanism accordingly.
  std::shared_ptr<c10::IValue> serialized_ = nullptr;

  // Cached original source range from DebugInfo
  mutable std::shared_ptr<SourceRange> orig_range_ = nullptr;
};

inline std::ostream& operator<<(std::ostream& out, const SourceRange& range) {
  range.highlight(out);
  return out;
}

} // namespace jit
} // namespace torch
