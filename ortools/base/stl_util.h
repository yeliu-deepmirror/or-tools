// Copyright 2010-2021 Google LLC
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OR_TOOLS_BASE_STL_UTIL_H_
#define OR_TOOLS_BASE_STL_UTIL_H_

#include <stddef.h>
#include <string.h>

#include <algorithm>
#include <cassert>
#include <deque>
#include <forward_list>
#include <functional>
#include <iterator>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include "ortools/base/integral_types.h"
#include "ortools/base/macros.h"

namespace gtl {
namespace internal {
template <typename LessFunc>
class Equiv {
 public:
  explicit Equiv(const LessFunc& f) : f_(f) {}
  template <typename T>
  bool operator()(const T& a, const T& b) const {
    return !f_(b, a) && !f_(a, b);
  }

 private:
  LessFunc f_;
};
}  // namespace internal

// Sorts and removes duplicates from a sequence container.
// If specified, the 'less_func' is used to compose an
// equivalence comparator for the sorting and uniqueness tests.
template <typename T, typename LessFunc>
inline void STLSortAndRemoveDuplicates(T* v, const LessFunc& less_func) {
  std::sort(v->begin(), v->end(), less_func);
  v->erase(std::unique(v->begin(), v->end(),
                       gtl::internal::Equiv<LessFunc>(less_func)),
           v->end());
}
template <typename T>
inline void STLSortAndRemoveDuplicates(T* v) {
  std::sort(v->begin(), v->end());
  v->erase(std::unique(v->begin(), v->end()), v->end());
}

// Stable sorts and removes duplicates from a sequence container, retaining
// the first equivalent element for each equivalence set.
// The 'less_func' is used to compose an equivalence comparator for the sorting
// and uniqueness tests.
template <typename T, typename LessFunc>
inline void STLStableSortAndRemoveDuplicates(T* v, const LessFunc& less_func) {
  std::stable_sort(v->begin(), v->end(), less_func);
  v->erase(std::unique(v->begin(), v->end(),
                       gtl::internal::Equiv<LessFunc>(less_func)),
           v->end());
}
// Stable sorts and removes duplicates from a sequence container, retaining
// the first equivalent element for each equivalence set, using < comparison and
// == equivalence testing.
template <typename T>
inline void STLStableSortAndRemoveDuplicates(T* v) {
  std::stable_sort(v->begin(), v->end());
  v->erase(std::unique(v->begin(), v->end()), v->end());
}

// Remove every occurrence of element e in v. See
//    http://en.wikipedia.org/wiki/Erase-remove_idiom.
template <typename T, typename E>
void STLEraseAllFromSequence(T* v, const E& e) {
  v->erase(std::remove(v->begin(), v->end(), e), v->end());
}
template <typename T, typename A, typename E>
void STLEraseAllFromSequence(std::list<T, A>* c, const E& e) {
  c->remove(e);
}
template <typename T, typename A, typename E>
void STLEraseAllFromSequence(std::forward_list<T, A>* c, const E& e) {
  c->remove(e);
}

// Remove each element e in v satisfying pred(e).
template <typename T, typename P>
void STLEraseAllFromSequenceIf(T* v, P pred) {
  v->erase(std::remove_if(v->begin(), v->end(), pred), v->end());
}
template <typename T, typename A, typename P>
void STLEraseAllFromSequenceIf(std::list<T, A>* c, P pred) {
  c->remove_if(pred);
}
template <typename T, typename A, typename P>
void STLEraseAllFromSequenceIf(std::forward_list<T, A>* c, P pred) {
  c->remove_if(pred);
}

// Clears internal memory of an STL object by swapping the argument with a new,
// empty object. STL clear()/reserve(0) does not always free internal memory
// allocated.
template <typename T>
void STLClearObject(T* obj) {
  T tmp;
  tmp.swap(*obj);
  // This reserve(0) is needed because "T tmp" sometimes allocates memory (arena
  // implementation?), even though this may not always work.
  obj->reserve(0);
}
// STLClearObject overload for deque, which is missing reserve().
template <typename T, typename A>
void STLClearObject(std::deque<T, A>* obj) {
  std::deque<T, A> tmp;
  tmp.swap(*obj);
}

// Calls STLClearObject() if the object is bigger than the specified limit,
// otherwise calls the object's clear() member. This can be useful if you want
// to allow the object to hold on to its allocated memory as long as it's not
// too much.
//
// Note: The name is misleading since the object is always cleared, regardless
// of its size.
template <typename T>
inline void STLClearIfBig(T* obj, size_t limit = 1 << 20) {
  if (obj->capacity() >= limit) {
    STLClearObject(obj);
  } else {
    obj->clear();
  }
}
// STLClearIfBig overload for deque, which is missing capacity().
template <typename T, typename A>
inline void STLClearIfBig(std::deque<T, A>* obj, size_t limit = 1 << 20) {
  if (obj->size() >= limit) {
    STLClearObject(obj);
  } else {
    obj->clear();
  }
}

// Removes all elements and reduces the number of buckets in a hash_set or
// hash_map back to the default if the current number of buckets is "limit" or
// more.
//
// Adding items to a hash container may add buckets, but removing items or
// calling clear() does not necessarily reduce the number of buckets. Having
// lots of buckets is good if you insert comparably many items in every
// iteration because you'll reduce collisions and table resizes. But having lots
// of buckets is bad if you insert few items in most subsequent iterations,
// because repeatedly clearing out all those buckets can get expensive.
//
// One solution is to call STLClearHashIfBig() with a "limit" value that is a
// small multiple of the typical number of items in your table. In the common
// case, this is equivalent to an ordinary clear. In the rare case where you
// insert a lot of items, the number of buckets is reset to the default to keep
// subsequent clear operations cheap. Note that the default number of buckets is
// 193 in the Gnu library implementation as of Jan '08.
template <typename T>
inline void STLClearHashIfBig(T* obj, size_t limit) {
  if (obj->bucket_count() >= limit) {
    T tmp;
    tmp.swap(*obj);
  } else {
    obj->clear();
  }
}

// Reserves space in the given string only if the existing capacity is not
// already enough. This is useful for strings because string::reserve() may
// *shrink* the capacity in some cases, which is usually not what users want.
// The behavior of this function is similar to that of vector::reserve() but for
// string.
inline void STLStringReserveIfNeeded(std::string* s, size_t min_capacity) {
  if (min_capacity > s->capacity()) s->reserve(min_capacity);
}

// Returns a mutable char* pointing to a string's internal buffer, which may not
// be null-terminated. Returns nullptr for an empty string. If not non-null,
// writing through this pointer will modify the string.
//
// string_as_array(&str)[i] is valid for 0 <= i < str.size() until the
// next call to a string method that invalidates iterators.
//
// In C++11 you may simply use &str[0] to get a mutable char*.
//
// Prior to C++11, there was no standard-blessed way of getting a mutable
// reference to a string's internal buffer. The requirement that string be
// contiguous is officially part of the C++11 standard [string.require]/5.
// According to Matt Austern, this should already work on all current C++98
// implementations.
inline char* string_as_array(std::string* str) {
  // DO NOT USE const_cast<char*>(str->data())! See the unittest for why.
  return str->empty() ? nullptr : &*str->begin();
}

// Tests two hash maps/sets for equality. This exists because operator== in the
// STL can return false when the maps/sets contain identical elements. This is
// because it compares the internal hash tables which may be different if the
// order of insertions and deletions differed.
template <typename HashSet>
inline bool HashSetEquality(const HashSet& set_a, const HashSet& set_b) {
  if (set_a.size() != set_b.size()) return false;
  for (typename HashSet::const_iterator i = set_a.begin(); i != set_a.end();
       ++i)
    if (set_b.find(*i) == set_b.end()) return false;
  return true;
}

// WARNING: Using HashMapEquality for multiple-associative containers like
// multimap and hash_multimap will result in wrong behavior.

template <typename HashMap, typename BinaryPredicate>
inline bool HashMapEquality(const HashMap& map_a, const HashMap& map_b,
                            BinaryPredicate mapped_type_equal) {
  if (map_a.size() != map_b.size()) return false;
  for (typename HashMap::const_iterator i = map_a.begin(); i != map_a.end();
       ++i) {
    typename HashMap::const_iterator j = map_b.find(i->first);
    if (j == map_b.end()) return false;
    if (!mapped_type_equal(i->second, j->second)) return false;
  }
  return true;
}

// We overload for 'map' without a specialized functor and simply use its
// operator== function.
template <typename K, typename V, typename C, typename A>
inline bool HashMapEquality(const std::map<K, V, C, A>& map_a,
                            const std::map<K, V, C, A>& map_b) {
  return map_a == map_b;
}

template <typename HashMap>
inline bool HashMapEquality(const HashMap& a, const HashMap& b) {
  using Mapped = typename HashMap::mapped_type;
  return HashMapEquality(a, b, std::equal_to<Mapped>());
}

// Calls delete (non-array version) on pointers in the range [begin, end).
//
// Note: If you're calling this on an entire container, you probably want to
// call STLDeleteElements(&container) instead (which also clears the container),
// or use an ElementDeleter.
template <typename ForwardIterator>
void STLDeleteContainerPointers(ForwardIterator begin, ForwardIterator end) {
  while (begin != end) {
    auto temp = begin;
    ++begin;
    delete *temp;
  }
}

// Calls delete (non-array version) on BOTH items (pointers) in each pair in the
// range [begin, end).
template <typename ForwardIterator>
void STLDeleteContainerPairPointers(ForwardIterator begin,
                                    ForwardIterator end) {
  while (begin != end) {
    auto temp = begin;
    ++begin;
    delete temp->first;
    delete temp->second;
  }
}

// Calls delete (non-array version) on the FIRST item (pointer) in each pair in
// the range [begin, end).
template <typename ForwardIterator>
void STLDeleteContainerPairFirstPointers(ForwardIterator begin,
                                         ForwardIterator end) {
  while (begin != end) {
    auto temp = begin;
    ++begin;
    delete temp->first;
  }
}

// Calls delete (non-array version) on the SECOND item (pointer) in each pair in
// the range [begin, end).
//
// Note: If you're calling this on an entire container, you probably want to
// call STLDeleteValues(&container) instead, or use ValueDeleter.
template <typename ForwardIterator>
void STLDeleteContainerPairSecondPointers(ForwardIterator begin,
                                          ForwardIterator end) {
  while (begin != end) {
    auto temp = begin;
    ++begin;
    delete temp->second;
  }
}

// Deletes all the elements in an STL container and clears the container. This
// function is suitable for use with a vector, set, hash_set, or any other STL
// container which defines sensible begin(), end(), and clear() methods.
//
// If container is nullptr, this function is a no-op.
//
// As an alternative to calling STLDeleteElements() directly, consider
// ElementDeleter (defined below), which ensures that your container's elements
// are deleted when the ElementDeleter goes out of scope.
template <typename T>
void STLDeleteElements(T* container) {
  if (!container) return;
  STLDeleteContainerPointers(container->begin(), container->end());
  container->clear();
}

// Given an STL container consisting of (key, value) pairs, STLDeleteValues
// deletes all the "value" components and clears the container. Does nothing in
// the case it's given a nullptr.
template <typename T>
void STLDeleteValues(T* v) {
  if (!v) return;
  (STLDeleteContainerPairSecondPointers)(v->begin(), v->end());
  v->clear();
}

// A very simple interface that simply provides a virtual destructor. It is used
// as a non-templated base class for the TemplatedElementDeleter and
// TemplatedValueDeleter classes.
//
// Clients should NOT use this class directly.
class BaseDeleter {
 public:
  virtual ~BaseDeleter() {}
  BaseDeleter(const BaseDeleter&) = delete;
  void operator=(const BaseDeleter&) = delete;

 protected:
  BaseDeleter() {}
};

// Given a pointer to an STL container, this class will delete all the element
// pointers when it goes out of scope.
//
// Clients should NOT use this class directly. Use ElementDeleter instead.
template <typename STLContainer>
class TemplatedElementDeleter : public BaseDeleter {
 public:
  explicit TemplatedElementDeleter(STLContainer* ptr) : container_ptr_(ptr) {}

  virtual ~TemplatedElementDeleter() { STLDeleteElements(container_ptr_); }

  TemplatedElementDeleter(const TemplatedElementDeleter&) = delete;
  void operator=(const TemplatedElementDeleter&) = delete;

 private:
  STLContainer* container_ptr_;
};

// ElementDeleter is an RAII (go/raii) object that deletes the elements in the
// given container when it goes out of scope. This is similar to
// std::unique_ptr<> except that a container's elements will be deleted rather
// than the container itself.
//
// Example:
//   std::vector<MyProto*> tmp_proto;
//   ElementDeleter d(&tmp_proto);
//   if (...) return false;
//   ...
//   return success;
//
// Since C++11, consider using containers of std::unique_ptr instead.
class ElementDeleter {
 public:
  template <typename STLContainer>
  explicit ElementDeleter(STLContainer* ptr)
      : deleter_(new TemplatedElementDeleter<STLContainer>(ptr)) {}

  ~ElementDeleter() { delete deleter_; }

  ElementDeleter(const ElementDeleter&) = delete;
  void operator=(const ElementDeleter&) = delete;

 private:
  BaseDeleter* deleter_;
};

// Given a pointer to an STL container this class will delete all the value
// pointers when it goes out of scope.
//
// Clients should NOT use this class directly. Use ValueDeleter instead.
template <typename STLContainer>
class TemplatedValueDeleter : public BaseDeleter {
 public:
  explicit TemplatedValueDeleter(STLContainer* ptr) : container_ptr_(ptr) {}

  virtual ~TemplatedValueDeleter() { STLDeleteValues(container_ptr_); }

  TemplatedValueDeleter(const TemplatedValueDeleter&) = delete;
  void operator=(const TemplatedValueDeleter&) = delete;

 private:
  STLContainer* container_ptr_;
};

// ValueDeleter is an RAII (go/raii) object that deletes the 'second' member in
// the given container of std::pair<>s when it goes out of scope.
//
// Example:
//   std::map<std::string, Foo*> foo_map;
//   ValueDeleter d(&foo_map);
//   if (...) return false;
//   ...
//   return success;
class ValueDeleter {
 public:
  template <typename STLContainer>
  explicit ValueDeleter(STLContainer* ptr)
      : deleter_(new TemplatedValueDeleter<STLContainer>(ptr)) {}

  ~ValueDeleter() { delete deleter_; }

  ValueDeleter(const ValueDeleter&) = delete;
  void operator=(const ValueDeleter&) = delete;

 private:
  BaseDeleter* deleter_;
};

// RAII (go/raii) object that deletes elements in the given container when it
// goes out of scope. Like ElementDeleter (above) except that this class is
// templated and doesn't have a virtual destructor.
//
// New code should prefer ElementDeleter.
template <typename STLContainer>
class STLElementDeleter {
 public:
  STLElementDeleter(STLContainer* ptr) : container_ptr_(ptr) {}
  ~STLElementDeleter() { STLDeleteElements(container_ptr_); }

 private:
  STLContainer* container_ptr_;
};

// RAII (go/raii) object that deletes the values in the given container of
// std::pair<>s when it goes out of scope. Like ValueDeleter (above) except that
// this class is templated and doesn't have a virtual destructor.
//
// New code should prefer ValueDeleter.
template <typename STLContainer>
class STLValueDeleter {
 public:
  STLValueDeleter(STLContainer* ptr) : container_ptr_(ptr) {}
  ~STLValueDeleter() { STLDeleteValues(container_ptr_); }

 private:
  STLContainer* container_ptr_;
};

// Sets the referenced pointer to nullptr and returns its original value. This
// can be a convenient way to remove a pointer from a container to avoid the
// eventual deletion by an ElementDeleter.
//
// Example:
//
//   std::vector<Foo*> v{new Foo, new Foo, new Foo};
//   ElementDeleter d(&v);
//   Foo* safe = release_ptr(&v[1]);
//   // v[1] is now nullptr and the Foo it previously pointed to is now
//   // stored in "safe"
template <typename T>
T* release_ptr(T** ptr) {
  assert(ptr);
  T* tmp = *ptr;
  *ptr = nullptr;
  return tmp;
}

namespace stl_util_internal {

// Like std::less, but allows heterogeneous arguments.
struct TransparentLess {
  template <typename T>
  bool operator()(const T& a, const T& b) const {
    // std::less is better than '<' here, because it can order pointers.
    return std::less<T>()(a, b);
  }
  template <typename T1, typename T2>
  bool operator()(const T1& a, const T2& b) const {
    return a < b;
  }
};

}  // namespace stl_util_internal

// SortedRangesHaveIntersection:
//
//     bool SortedRangesHaveIntersection(begin1, end1, begin2, end2);
//     bool SortedRangesHaveIntersection(begin1, end1, begin2, end2,
//                                       comparator);
//
// Returns true iff any element in the sorted range [begin1, end1) is
// equivalent to any element in the sorted range [begin2, end2). The iterators
// themselves do not have to be the same type, but the value types must be
// sorted either by the specified comparator, or by '<' if no comparator is
// given.
// [Two elements a,b are considered equivalent if !(a < b) && !(b < a) ].
template <typename InputIterator1, typename InputIterator2, typename Comp>
bool SortedRangesHaveIntersection(InputIterator1 begin1, InputIterator1 end1,
                                  InputIterator2 begin2, InputIterator2 end2,
                                  Comp comparator) {
  assert(std::is_sorted(begin1, end1, comparator));
  assert(std::is_sorted(begin2, end2, comparator));
  while (begin1 != end1 && begin2 != end2) {
    if (comparator(*begin1, *begin2)) {
      ++begin1;
      continue;
    }
    if (comparator(*begin2, *begin1)) {
      ++begin2;
      continue;
    }
    return true;
  }
  return false;
}
template <typename InputIterator1, typename InputIterator2>
bool SortedRangesHaveIntersection(InputIterator1 begin1, InputIterator1 end1,
                                  InputIterator2 begin2, InputIterator2 end2) {
  return SortedRangesHaveIntersection(
      begin1, end1, begin2, end2, gtl::stl_util_internal::TransparentLess());
}

// Returns true iff the ordered containers 'in1' and 'in2' have a non-empty
// intersection. The container elements do not have to be the same type, but the
// elements must be sorted either by the specified comparator, or by '<' if no
// comparator is given.
template <typename In1, typename In2, typename Comp>
bool SortedContainersHaveIntersection(const In1& in1, const In2& in2,
                                      Comp comparator) {
  return SortedRangesHaveIntersection(in1.begin(), in1.end(), in2.begin(),
                                      in2.end(), comparator);
}
template <typename In1, typename In2>
bool SortedContainersHaveIntersection(const In1& in1, const In2& in2) {
  return SortedContainersHaveIntersection(
      in1, in2, gtl::stl_util_internal::TransparentLess());
}

}  // namespace gtl
#endif  // OR_TOOLS_BASE_STL_UTIL_H_
