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

#ifndef OR_TOOLS_BASE_LOGGING_H_
#define OR_TOOLS_BASE_LOGGING_H_

#include "glog/logging.h"
#include "fmt/format.h"
#include <optional>

#define QCHECK CHECK
#define QCHECK_EQ CHECK_EQ
#define QCHECK_GE CHECK_GE
#define QCHECK_GT CHECK_GT
#define ABSL_DIE_IF_NULL CHECK_NOTNULL

#ifndef CHECK_OK
#define CHECK_OK(x) CHECK((x).ok())
#endif  //CHECK_OK

#define QCHECK_OK CHECK_OK

class OrToolsStatus {
public:
  static OrToolsStatus OK() { return OrToolsStatus(); }
  static OrToolsStatus Error(std::string message) { return OrToolsStatus(std::move(message)); }

  template <typename... Args>
  static OrToolsStatus FormatError(Args... args) {
    return Error(fmt::format(std::forward<Args>(args)...));
  }

  bool ok() const { return !error_message_.has_value(); }

  const std::string& message() const {
    return error_message();
  }

  const std::string& error_message() const {
    CHECK(error_message_.has_value());
    return error_message_.value();
  }

private:
  OrToolsStatus() {}
  explicit OrToolsStatus(std::string msg) : error_message_(std::move(msg)) {}

  std::optional<std::string> error_message_;
};

inline std::string StringJoin(const std::vector<int>& parts) {
  if (parts.empty()) return "";
  if (parts.size() < 2) return std::to_string(parts[0]);
  std::string result = std::to_string(parts[0]);
  for (size_t i = 1; i < parts.size(); i++) {
    result += " " + std::to_string(parts[i]);
  }
  return result;
}


#endif  // OR_TOOLS_BASE_LOGGING_H_
