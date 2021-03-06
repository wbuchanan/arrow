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

// Functions for comparing Arrow data structures

#include "arrow/compare.h"

#include <cstdint>
#include <memory>
#include <vector>

#include "arrow/array.h"
#include "arrow/status.h"
#include "arrow/tensor.h"
#include "arrow/type.h"
#include "arrow/type_traits.h"
#include "arrow/util/bit-util.h"
#include "arrow/util/decimal.h"
#include "arrow/util/logging.h"
#include "arrow/visitor_inline.h"

namespace arrow {

// ----------------------------------------------------------------------
// Public method implementations

class RangeEqualsVisitor {
 public:
  RangeEqualsVisitor(const Array& right, int64_t left_start_idx, int64_t left_end_idx,
      int64_t right_start_idx)
      : right_(right),
        left_start_idx_(left_start_idx),
        left_end_idx_(left_end_idx),
        right_start_idx_(right_start_idx),
        result_(false) {}

  template <typename ArrayType>
  inline Status CompareValues(const ArrayType& left) {
    const auto& right = static_cast<const ArrayType&>(right_);

    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      const bool is_null = left.IsNull(i);
      if (is_null != right.IsNull(o_i) ||
          (!is_null && left.Value(i) != right.Value(o_i))) {
        result_ = false;
        return Status::OK();
      }
    }
    result_ = true;
    return Status::OK();
  }

  bool CompareBinaryRange(const BinaryArray& left) const {
    const auto& right = static_cast<const BinaryArray&>(right_);

    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      const bool is_null = left.IsNull(i);
      if (is_null != right.IsNull(o_i)) { return false; }
      if (is_null) continue;
      const int32_t begin_offset = left.value_offset(i);
      const int32_t end_offset = left.value_offset(i + 1);
      const int32_t right_begin_offset = right.value_offset(o_i);
      const int32_t right_end_offset = right.value_offset(o_i + 1);
      // Underlying can't be equal if the size isn't equal
      if (end_offset - begin_offset != right_end_offset - right_begin_offset) {
        return false;
      }

      if (end_offset - begin_offset > 0 &&
          std::memcmp(left.data()->data() + begin_offset,
              right.data()->data() + right_begin_offset,
              static_cast<size_t>(end_offset - begin_offset))) {
        return false;
      }
    }
    return true;
  }

  bool CompareLists(const ListArray& left) {
    const auto& right = static_cast<const ListArray&>(right_);

    const std::shared_ptr<Array>& left_values = left.values();
    const std::shared_ptr<Array>& right_values = right.values();

    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      const bool is_null = left.IsNull(i);
      if (is_null != right.IsNull(o_i)) { return false; }
      if (is_null) continue;
      const int32_t begin_offset = left.value_offset(i);
      const int32_t end_offset = left.value_offset(i + 1);
      const int32_t right_begin_offset = right.value_offset(o_i);
      const int32_t right_end_offset = right.value_offset(o_i + 1);
      // Underlying can't be equal if the size isn't equal
      if (end_offset - begin_offset != right_end_offset - right_begin_offset) {
        return false;
      }
      if (!left_values->RangeEquals(
              begin_offset, end_offset, right_begin_offset, right_values)) {
        return false;
      }
    }
    return true;
  }

  bool CompareStructs(const StructArray& left) {
    const auto& right = static_cast<const StructArray&>(right_);
    bool equal_fields = true;
    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      if (left.IsNull(i) != right.IsNull(o_i)) { return false; }
      if (left.IsNull(i)) continue;
      for (int j = 0; j < static_cast<int>(left.fields().size()); ++j) {
        // TODO: really we should be comparing stretches of non-null data rather
        // than looking at one value at a time.
        const int64_t left_abs_index = i + left.offset();
        const int64_t right_abs_index = o_i + right.offset();

        equal_fields = left.field(j)->RangeEquals(
            left_abs_index, left_abs_index + 1, right_abs_index, right.field(j));
        if (!equal_fields) { return false; }
      }
    }
    return true;
  }

  bool CompareUnions(const UnionArray& left) const {
    const auto& right = static_cast<const UnionArray&>(right_);

    const UnionMode union_mode = left.mode();
    if (union_mode != right.mode()) { return false; }

    const auto& left_type = static_cast<const UnionType&>(*left.type());

    // Define a mapping from the type id to child number
    uint8_t max_code = 0;

    const std::vector<uint8_t>& type_codes = left_type.type_codes();
    for (size_t i = 0; i < type_codes.size(); ++i) {
      const uint8_t code = type_codes[i];
      if (code > max_code) { max_code = code; }
    }

    // Store mapping in a vector for constant time lookups
    std::vector<uint8_t> type_id_to_child_num(max_code + 1);
    for (uint8_t i = 0; i < static_cast<uint8_t>(type_codes.size()); ++i) {
      type_id_to_child_num[type_codes[i]] = i;
    }

    const uint8_t* left_ids = left.raw_type_ids();
    const uint8_t* right_ids = right.raw_type_ids();

    uint8_t id, child_num;
    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      if (left.IsNull(i) != right.IsNull(o_i)) { return false; }
      if (left.IsNull(i)) continue;
      if (left_ids[i] != right_ids[o_i]) { return false; }

      id = left_ids[i];
      child_num = type_id_to_child_num[id];

      const int64_t left_abs_index = i + left.offset();
      const int64_t right_abs_index = o_i + right.offset();

      // TODO(wesm): really we should be comparing stretches of non-null data
      // rather than looking at one value at a time.
      if (union_mode == UnionMode::SPARSE) {
        if (!left.child(child_num)->RangeEquals(left_abs_index, left_abs_index + 1,
                right_abs_index, right.child(child_num))) {
          return false;
        }
      } else {
        const int32_t offset = left.raw_value_offsets()[i];
        const int32_t o_offset = right.raw_value_offsets()[i];
        if (!left.child(child_num)->RangeEquals(
                offset, offset + 1, o_offset, right.child(child_num))) {
          return false;
        }
      }
    }
    return true;
  }

  Status Visit(const BinaryArray& left) {
    result_ = CompareBinaryRange(left);
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryArray& left) {
    const auto& right = static_cast<const FixedSizeBinaryArray&>(right_);

    int32_t width = left.byte_width();

    const uint8_t* left_data = nullptr;
    const uint8_t* right_data = nullptr;

    if (left.data()) { left_data = left.raw_data() + left.offset() * width; }

    if (right.data()) { right_data = right.raw_data() + right.offset() * width; }

    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      const bool is_null = left.IsNull(i);
      if (is_null != right.IsNull(o_i)) {
        result_ = false;
        return Status::OK();
      }
      if (is_null) continue;

      if (std::memcmp(left_data + width * i, right_data + width * o_i, width)) {
        result_ = false;
        return Status::OK();
      }
    }
    result_ = true;
    return Status::OK();
  }

  Status Visit(const DecimalArray& left) {
    const auto& right = static_cast<const DecimalArray&>(right_);

    int32_t width = left.byte_width();

    const uint8_t* left_data = nullptr;
    const uint8_t* right_data = nullptr;

    if (left.data()) { left_data = left.raw_data() + left.offset() * width; }

    if (right.data()) { right_data = right.raw_data() + right.offset() * width; }

    for (int64_t i = left_start_idx_, o_i = right_start_idx_; i < left_end_idx_;
         ++i, ++o_i) {
      if (left.IsNegative(i) != right.IsNegative(o_i)) {
        result_ = false;
        return Status::OK();
      }

      const bool is_null = left.IsNull(i);
      if (is_null != right.IsNull(o_i)) {
        result_ = false;
        return Status::OK();
      }
      if (is_null) continue;

      if (std::memcmp(left_data + width * i, right_data + width * o_i, width)) {
        result_ = false;
        return Status::OK();
      }
    }
    result_ = true;
    return Status::OK();
  }

  Status Visit(const NullArray& left) {
    UNUSED(left);
    result_ = true;
    return Status::OK();
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<PrimitiveArray, T>::value, Status>::type Visit(
      const T& left) {
    return CompareValues<T>(left);
  }

  Status Visit(const ListArray& left) {
    result_ = CompareLists(left);
    return Status::OK();
  }

  Status Visit(const StructArray& left) {
    result_ = CompareStructs(left);
    return Status::OK();
  }

  Status Visit(const UnionArray& left) {
    result_ = CompareUnions(left);
    return Status::OK();
  }

  Status Visit(const DictionaryArray& left) {
    const auto& right = static_cast<const DictionaryArray&>(right_);
    if (!left.dictionary()->Equals(right.dictionary())) {
      result_ = false;
      return Status::OK();
    }
    result_ = left.indices()->RangeEquals(
        left_start_idx_, left_end_idx_, right_start_idx_, right.indices());
    return Status::OK();
  }

  bool result() const { return result_; }

 protected:
  const Array& right_;
  int64_t left_start_idx_;
  int64_t left_end_idx_;
  int64_t right_start_idx_;

  bool result_;
};

class ArrayEqualsVisitor : public RangeEqualsVisitor {
 public:
  explicit ArrayEqualsVisitor(const Array& right)
      : RangeEqualsVisitor(right, 0, right.length(), 0) {}

  Status Visit(const NullArray& left) { return Status::OK(); }

  Status Visit(const BooleanArray& left) {
    const auto& right = static_cast<const BooleanArray&>(right_);

    if (left.null_count() > 0) {
      const uint8_t* left_data = left.data()->data();
      const uint8_t* right_data = right.data()->data();

      for (int64_t i = 0; i < left.length(); ++i) {
        if (!left.IsNull(i) &&
            BitUtil::GetBit(left_data, i + left.offset()) !=
                BitUtil::GetBit(right_data, i + right.offset())) {
          result_ = false;
          return Status::OK();
        }
      }
      result_ = true;
    } else {
      result_ = BitmapEquals(left.data()->data(), left.offset(), right.data()->data(),
          right.offset(), left.length());
    }
    return Status::OK();
  }

  bool IsEqualPrimitive(const PrimitiveArray& left) {
    const auto& right = static_cast<const PrimitiveArray&>(right_);
    const auto& size_meta = dynamic_cast<const FixedWidthType&>(*left.type());
    const int byte_width = size_meta.bit_width() / 8;

    const uint8_t* left_data = nullptr;
    const uint8_t* right_data = nullptr;

    if (left.data()) { left_data = left.data()->data() + left.offset() * byte_width; }

    if (right.data()) { right_data = right.data()->data() + right.offset() * byte_width; }

    if (left.null_count() > 0) {
      for (int64_t i = 0; i < left.length(); ++i) {
        if (!left.IsNull(i) && memcmp(left_data, right_data, byte_width)) {
          return false;
        }
        left_data += byte_width;
        right_data += byte_width;
      }
      return true;
    } else {
      return memcmp(left_data, right_data,
                 static_cast<size_t>(byte_width * left.length())) == 0;
    }
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<PrimitiveArray, T>::value &&
                              !std::is_base_of<BooleanArray, T>::value,
      Status>::type
  Visit(const T& left) {
    result_ = IsEqualPrimitive(left);
    return Status::OK();
  }

  template <typename ArrayType>
  bool ValueOffsetsEqual(const ArrayType& left) {
    const auto& right = static_cast<const ArrayType&>(right_);

    if (left.offset() == 0 && right.offset() == 0) {
      return left.value_offsets()->Equals(
          *right.value_offsets(), (left.length() + 1) * sizeof(int32_t));
    } else {
      // One of the arrays is sliced; logic is more complicated because the
      // value offsets are not both 0-based
      auto left_offsets =
          reinterpret_cast<const int32_t*>(left.value_offsets()->data()) + left.offset();
      auto right_offsets =
          reinterpret_cast<const int32_t*>(right.value_offsets()->data()) +
          right.offset();

      for (int64_t i = 0; i < left.length() + 1; ++i) {
        if (left_offsets[i] - left_offsets[0] != right_offsets[i] - right_offsets[0]) {
          return false;
        }
      }
      return true;
    }
  }

  bool CompareBinary(const BinaryArray& left) {
    const auto& right = static_cast<const BinaryArray&>(right_);

    bool equal_offsets = ValueOffsetsEqual<BinaryArray>(left);
    if (!equal_offsets) { return false; }

    if (!left.data() && !(right.data())) { return true; }
    if (left.value_offset(left.length()) == 0) { return true; }

    const uint8_t* left_data = left.data()->data();
    const uint8_t* right_data = right.data()->data();

    if (left.null_count() == 0) {
      // Fast path for null count 0, single memcmp
      if (left.offset() == 0 && right.offset() == 0) {
        return std::memcmp(
                   left_data, right_data, left.raw_value_offsets()[left.length()]) == 0;
      } else {
        const int64_t total_bytes =
            left.value_offset(left.length()) - left.value_offset(0);
        return std::memcmp(left_data + left.value_offset(0),
                   right_data + right.value_offset(0),
                   static_cast<size_t>(total_bytes)) == 0;
      }
    } else {
      // ARROW-537: Only compare data in non-null slots
      const int32_t* left_offsets = left.raw_value_offsets();
      const int32_t* right_offsets = right.raw_value_offsets();
      for (int64_t i = 0; i < left.length(); ++i) {
        if (left.IsNull(i)) { continue; }
        if (std::memcmp(left_data + left_offsets[i], right_data + right_offsets[i],
                left.value_length(i))) {
          return false;
        }
      }
      return true;
    }
  }

  Status Visit(const BinaryArray& left) {
    result_ = CompareBinary(left);
    return Status::OK();
  }

  Status Visit(const ListArray& left) {
    const auto& right = static_cast<const ListArray&>(right_);
    bool equal_offsets = ValueOffsetsEqual<ListArray>(left);
    if (!equal_offsets) {
      result_ = false;
      return Status::OK();
    }

    if (left.offset() == 0 && right.offset() == 0) {
      result_ = left.values()->Equals(right.values());
    } else {
      // One of the arrays is sliced
      result_ = left.values()->RangeEquals(left.value_offset(0),
          left.value_offset(left.length()), right.value_offset(0), right.values());
    }

    return Status::OK();
  }

  Status Visit(const DictionaryArray& left) {
    const auto& right = static_cast<const DictionaryArray&>(right_);
    if (!left.dictionary()->Equals(right.dictionary())) {
      result_ = false;
    } else {
      result_ = left.indices()->Equals(right.indices());
    }
    return Status::OK();
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<NestedType, typename T::TypeClass>::value,
      Status>::type
  Visit(const T& left) {
    return RangeEqualsVisitor::Visit(left);
  }
};

template <typename TYPE>
inline bool FloatingApproxEquals(
    const NumericArray<TYPE>& left, const NumericArray<TYPE>& right) {
  using T = typename TYPE::c_type;

  const T* left_data = left.raw_data();
  const T* right_data = right.raw_data();

  static constexpr T EPSILON = static_cast<T>(1E-5);

  if (left.null_count() > 0) {
    for (int64_t i = 0; i < left.length(); ++i) {
      if (left.IsNull(i)) continue;
      if (fabs(left_data[i] - right_data[i]) > EPSILON) { return false; }
    }
  } else {
    for (int64_t i = 0; i < left.length(); ++i) {
      if (fabs(left_data[i] - right_data[i]) > EPSILON) { return false; }
    }
  }
  return true;
}

class ApproxEqualsVisitor : public ArrayEqualsVisitor {
 public:
  using ArrayEqualsVisitor::ArrayEqualsVisitor;
  using ArrayEqualsVisitor::Visit;

  Status Visit(const FloatArray& left) {
    result_ =
        FloatingApproxEquals<FloatType>(left, static_cast<const FloatArray&>(right_));
    return Status::OK();
  }

  Status Visit(const DoubleArray& left) {
    result_ =
        FloatingApproxEquals<DoubleType>(left, static_cast<const DoubleArray&>(right_));
    return Status::OK();
  }
};

static bool BaseDataEquals(const Array& left, const Array& right) {
  if (left.length() != right.length() || left.null_count() != right.null_count() ||
      left.type_id() != right.type_id()) {
    return false;
  }
  if (left.null_count() > 0) {
    return BitmapEquals(left.null_bitmap()->data(), left.offset(),
        right.null_bitmap()->data(), right.offset(), left.length());
  }
  return true;
}

template <typename VISITOR>
inline Status ArrayEqualsImpl(const Array& left, const Array& right, bool* are_equal) {
  // The arrays are the same object
  if (&left == &right) {
    *are_equal = true;
  } else if (!BaseDataEquals(left, right)) {
    *are_equal = false;
  } else if (left.length() == 0) {
    *are_equal = true;
  } else {
    VISITOR visitor(right);
    RETURN_NOT_OK(VisitArrayInline(left, &visitor));
    *are_equal = visitor.result();
  }
  return Status::OK();
}

Status ArrayEquals(const Array& left, const Array& right, bool* are_equal) {
  return ArrayEqualsImpl<ArrayEqualsVisitor>(left, right, are_equal);
}

Status ArrayApproxEquals(const Array& left, const Array& right, bool* are_equal) {
  return ArrayEqualsImpl<ApproxEqualsVisitor>(left, right, are_equal);
}

Status ArrayRangeEquals(const Array& left, const Array& right, int64_t left_start_idx,
    int64_t left_end_idx, int64_t right_start_idx, bool* are_equal) {
  if (&left == &right) {
    *are_equal = true;
  } else if (left.type_id() != right.type_id()) {
    *are_equal = false;
  } else if (left.length() == 0) {
    *are_equal = true;
  } else {
    RangeEqualsVisitor visitor(right, left_start_idx, left_end_idx, right_start_idx);
    RETURN_NOT_OK(VisitArrayInline(left, &visitor));
    *are_equal = visitor.result();
  }
  return Status::OK();
}

// ----------------------------------------------------------------------
// Implement TensorEquals

class TensorEqualsVisitor {
 public:
  explicit TensorEqualsVisitor(const Tensor& right) : right_(right) {}

  template <typename TensorType>
  Status Visit(const TensorType& left) {
    const auto& size_meta = dynamic_cast<const FixedWidthType&>(*left.type());
    const int byte_width = size_meta.bit_width() / 8;
    DCHECK_GT(byte_width, 0);

    const uint8_t* left_data = left.data()->data();
    const uint8_t* right_data = right_.data()->data();

    result_ =
        memcmp(left_data, right_data, static_cast<size_t>(byte_width * left.size())) == 0;
    return Status::OK();
  }

  bool result() const { return result_; }

 protected:
  const Tensor& right_;
  bool result_;
};

Status TensorEquals(const Tensor& left, const Tensor& right, bool* are_equal) {
  // The arrays are the same object
  if (&left == &right) {
    *are_equal = true;
  } else if (left.type_id() != right.type_id()) {
    *are_equal = false;
  } else if (left.size() == 0) {
    *are_equal = true;
  } else {
    if (!left.is_contiguous() || !right.is_contiguous()) {
      return Status::NotImplemented(
          "Comparison not implemented for non-contiguous tensors");
    }

    TensorEqualsVisitor visitor(right);
    RETURN_NOT_OK(VisitTensorInline(left, &visitor));
    *are_equal = visitor.result();
  }
  return Status::OK();
}

// ----------------------------------------------------------------------
// Implement TypeEquals

class TypeEqualsVisitor {
 public:
  explicit TypeEqualsVisitor(const DataType& right) : right_(right), result_(false) {}

  Status VisitChildren(const DataType& left) {
    if (left.num_children() != right_.num_children()) {
      result_ = false;
      return Status::OK();
    }

    for (int i = 0; i < left.num_children(); ++i) {
      if (!left.child(i)->Equals(right_.child(i))) {
        result_ = false;
        return Status::OK();
      }
    }
    result_ = true;
    return Status::OK();
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<NoExtraMeta, T>::value ||
                              std::is_base_of<PrimitiveCType, T>::value,
      Status>::type
  Visit(const T& type) {
    result_ = true;
    return Status::OK();
  }

  template <typename T>
  typename std::enable_if<std::is_base_of<TimeType, T>::value ||
                              std::is_base_of<DateType, T>::value,
      Status>::type
  Visit(const T& left) {
    const auto& right = static_cast<const T&>(right_);
    result_ = left.unit() == right.unit();
    return Status::OK();
  }

  Status Visit(const TimestampType& left) {
    const auto& right = static_cast<const TimestampType&>(right_);
    result_ = left.unit() == right.unit() && left.timezone() == right.timezone();
    return Status::OK();
  }

  Status Visit(const FixedSizeBinaryType& left) {
    const auto& right = static_cast<const FixedSizeBinaryType&>(right_);
    result_ = left.byte_width() == right.byte_width();
    return Status::OK();
  }

  Status Visit(const DecimalType& left) {
    const auto& right = static_cast<const DecimalType&>(right_);
    result_ = left.precision() == right.precision() && left.scale() == right.scale();
    return Status::OK();
  }

  Status Visit(const ListType& left) { return VisitChildren(left); }

  Status Visit(const StructType& left) { return VisitChildren(left); }

  Status Visit(const UnionType& left) {
    const auto& right = static_cast<const UnionType&>(right_);

    if (left.mode() != right.mode() ||
        left.type_codes().size() != right.type_codes().size()) {
      result_ = false;
      return Status::OK();
    }

    const std::vector<uint8_t>& left_codes = left.type_codes();
    const std::vector<uint8_t>& right_codes = right.type_codes();

    for (size_t i = 0; i < left_codes.size(); ++i) {
      if (left_codes[i] != right_codes[i]) {
        result_ = false;
        return Status::OK();
      }
    }

    for (int i = 0; i < left.num_children(); ++i) {
      if (!left.child(i)->Equals(right_.child(i))) {
        result_ = false;
        return Status::OK();
      }
    }

    result_ = true;
    return Status::OK();
  }

  Status Visit(const DictionaryType& left) {
    const auto& right = static_cast<const DictionaryType&>(right_);
    result_ = left.index_type()->Equals(right.index_type()) &&
              left.dictionary()->Equals(right.dictionary());
    return Status::OK();
  }

  bool result() const { return result_; }

 protected:
  const DataType& right_;
  bool result_;
};

Status TypeEquals(const DataType& left, const DataType& right, bool* are_equal) {
  // The arrays are the same object
  if (&left == &right) {
    *are_equal = true;
  } else if (left.id() != right.id()) {
    *are_equal = false;
  } else {
    TypeEqualsVisitor visitor(right);
    RETURN_NOT_OK(VisitTypeInline(left, &visitor));
    *are_equal = visitor.result();
  }
  return Status::OK();
}

}  // namespace arrow
