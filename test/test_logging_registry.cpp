// Copyright 2026 Fumiya Ohnishi
//
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

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "rcutils/logging.h"

class TestLoggingRegistry : public ::testing::Test
{
protected:
  void SetUp() override
  {
    ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_shutdown());
    rcutils_reset_error();
  }

  void TearDown() override
  {
    rcutils_reset_error();
    EXPECT_EQ(RCUTILS_RET_OK, rcutils_logging_shutdown());
  }

  std::vector<std::string> names(const char * base = nullptr)
  {
    auto result = rcutils_get_zero_initialized_string_array();
    EXPECT_EQ(RCUTILS_RET_OK, rcutils_logging_get_logger_names(
        base, rcutils_get_default_allocator(), &result));
    std::vector<std::string> copy;
    for (size_t i = 0; i < result.size; ++i) {
      copy.emplace_back(result.data[i]);
    }
    EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&result));
    return copy;
  }
};

TEST_F(TestLoggingRegistry, names_are_independent_of_levels_and_log_calls)
{
  EXPECT_TRUE(names().empty());
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_set_logger_level("configured_only", 30));
  rcutils_log(nullptr, RCUTILS_LOG_SEVERITY_DEBUG, "logged_only", "not registered");
  EXPECT_TRUE(names().empty());

  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_set_logger_level("foo", 30));
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger("foo.child"));
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger("foo.child"));
  EXPECT_EQ(std::vector<std::string>({"foo.child"}), names());
  EXPECT_EQ(RCUTILS_LOG_SEVERITY_UNSET, rcutils_logging_get_logger_level("foo.child"));
  EXPECT_EQ(RCUTILS_LOG_SEVERITY_WARN, rcutils_logging_get_logger_effective_level("foo.child"));
  EXPECT_EQ(RCUTILS_LOG_SEVERITY_WARN, rcutils_logging_get_logger_level("foo"));
}

TEST_F(TestLoggingRegistry, hierarchy_filter_is_sorted_and_has_a_separator_boundary)
{
  for (const char * name : {"foobar", "foo.z", "foo.a.deep", "foo", "fo", "foo.a"}) {
    ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger(name));
  }
  EXPECT_EQ(std::vector<std::string>({"foo", "foo.a", "foo.a.deep", "foo.z"}), names("foo"));
  EXPECT_EQ(std::vector<std::string>({"foo.a", "foo.a.deep"}), names("foo.a"));
  EXPECT_EQ(std::vector<std::string>({"fo"}), names("fo"));
  EXPECT_TRUE(names("absent").empty());
}

TEST_F(TestLoggingRegistry, copies_names_and_returns_snapshots_that_survive_shutdown)
{
  char name[] = "retained";
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger(name));
  name[0] = 'X';
  auto snapshot = rcutils_get_zero_initialized_string_array();
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_get_logger_names(
      nullptr, rcutils_get_default_allocator(), &snapshot));
  ASSERT_EQ(1u, snapshot.size);
  snapshot.data[0][0] = 'Y';
  EXPECT_EQ(std::vector<std::string>({"retained"}), names());
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger("later"));
  EXPECT_EQ(1u, snapshot.size);
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_shutdown());
  EXPECT_STREQ("Yetained", snapshot.data[0]);
  EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&snapshot));
  EXPECT_TRUE(names().empty());
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger("new_session"));
  EXPECT_EQ(std::vector<std::string>({"new_session"}), names());
}

TEST_F(TestLoggingRegistry, rejects_invalid_arguments_without_changing_output)
{
  auto allocator = rcutils_get_default_allocator();
  auto result = rcutils_get_zero_initialized_string_array();
  EXPECT_EQ(RCUTILS_RET_INVALID_ARGUMENT, rcutils_logging_register_logger(nullptr));
  rcutils_reset_error();
  EXPECT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger(""));
  EXPECT_TRUE(names().empty());
  EXPECT_EQ(RCUTILS_RET_INVALID_ARGUMENT,
    rcutils_logging_get_logger_names("", allocator, &result));
  rcutils_reset_error();
  EXPECT_EQ(RCUTILS_RET_INVALID_ARGUMENT,
    rcutils_logging_get_logger_names(nullptr, allocator, nullptr));
  rcutils_reset_error();
  EXPECT_EQ(RCUTILS_RET_INVALID_ARGUMENT, rcutils_logging_get_logger_names(
      nullptr, rcutils_get_zero_initialized_allocator(), &result));
  rcutils_reset_error();
  ASSERT_EQ(RCUTILS_RET_OK, rcutils_string_array_init(&result, 1, &allocator));
  auto original_data = result.data;
  EXPECT_EQ(RCUTILS_RET_INVALID_ARGUMENT,
    rcutils_logging_get_logger_names(nullptr, allocator, &result));
  rcutils_reset_error();
  EXPECT_EQ(original_data, result.data);
  EXPECT_EQ(1u, result.size);
  EXPECT_EQ(RCUTILS_RET_OK, rcutils_string_array_fini(&result));
}

struct AllocationState
{
  int remaining = -1;
  size_t outstanding = 0;

  bool fail()
  {
    if (remaining < 0) {
      return false;
    }
    if (remaining == 0) {
      return true;
    }
    --remaining;
    return false;
  }

  rcutils_allocator_t allocator()
  {
    auto result = rcutils_get_default_allocator();
    result.state = this;
    result.allocate = [](size_t size, void * opaque) -> void * {
        auto & state = *static_cast<AllocationState *>(opaque);
        if (state.fail()) {return nullptr;}
        void * pointer = std::malloc(size);
        if (pointer) {++state.outstanding;}
        return pointer;
      };
    result.zero_allocate = [](size_t count, size_t size, void * opaque) -> void * {
        auto & state = *static_cast<AllocationState *>(opaque);
        if (state.fail()) {return nullptr;}
        void * pointer = std::calloc(count, size);
        if (pointer) {++state.outstanding;}
        return pointer;
      };
    result.reallocate = [](void * pointer, size_t size, void * opaque) -> void * {
        auto & state = *static_cast<AllocationState *>(opaque);
        if (state.fail()) {return nullptr;}
        bool was_null = pointer == nullptr;
        void * replacement = std::realloc(pointer, size);
        if (replacement && was_null) {++state.outstanding;}
        return replacement;
      };
    result.deallocate = [](void * pointer, void * opaque) {
        auto & state = *static_cast<AllocationState *>(opaque);
        if (pointer) {--state.outstanding;}
        std::free(pointer);
      };
    return result;
  }
};

TEST_F(TestLoggingRegistry, frees_partial_snapshots_on_allocation_failure)
{
  for (const char * name : {"one", "two", "three"}) {
    ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger(name));
  }
  for (int allowance = 0; allowance < 4; ++allowance) {
    AllocationState state;
    state.remaining = allowance;
    auto result = rcutils_get_zero_initialized_string_array();
    EXPECT_EQ(RCUTILS_RET_BAD_ALLOC, rcutils_logging_get_logger_names(
        nullptr, state.allocator(), &result));
    rcutils_reset_error();
    EXPECT_EQ(nullptr, result.data);
    EXPECT_EQ(0u, result.size);
    EXPECT_EQ(0u, state.outstanding);
  }
  EXPECT_EQ(3u, names().size());
}

TEST_F(TestLoggingRegistry, registration_recovers_from_allocation_failure_without_leaks)
{
  for (int allowance = 0; allowance < 12; ++allowance) {
    AllocationState state;
    ASSERT_EQ(RCUTILS_RET_OK, rcutils_logging_initialize_with_allocator(state.allocator()));
    state.remaining = allowance;
    rcutils_ret_t ret = rcutils_logging_register_logger("retry");
    if (ret != RCUTILS_RET_OK) {
      rcutils_reset_error();
    }
    state.remaining = -1;
    EXPECT_EQ(RCUTILS_RET_OK, rcutils_logging_register_logger("retry"));
    EXPECT_EQ(std::vector<std::string>({"retry"}), names());
    EXPECT_EQ(RCUTILS_RET_OK, rcutils_logging_shutdown());
    EXPECT_EQ(0u, state.outstanding);
  }
}

TEST_F(TestLoggingRegistry, concurrent_registration_and_enumeration)
{
  std::atomic<bool> success{true};
  std::vector<std::thread> threads;
  for (int worker = 0; worker < 6; ++worker) {
    threads.emplace_back([worker, &success]() {
        for (int i = 0; i < 40; ++i) {
          std::string name = "parallel." + std::to_string(worker) + "." + std::to_string(i);
          if (rcutils_logging_register_logger(name.c_str()) != RCUTILS_RET_OK ||
          rcutils_logging_register_logger("parallel.shared") != RCUTILS_RET_OK)
          {
            success = false;
            return;
          }
          auto snapshot = rcutils_get_zero_initialized_string_array();
          if (rcutils_logging_get_logger_names(
              "parallel", rcutils_get_default_allocator(), &snapshot) != RCUTILS_RET_OK)
          {
            success = false;
            return;
          }
          for (size_t j = 1; j < snapshot.size; ++j) {
            if (std::string(snapshot.data[j - 1]) >= snapshot.data[j]) {success = false;}
          }
          if (rcutils_string_array_fini(&snapshot) != RCUTILS_RET_OK) {success = false;}
        }
      });
  }
  for (auto & thread : threads) {
    thread.join();
  }
  EXPECT_TRUE(success);
  EXPECT_EQ(241u, names("parallel").size());
}
