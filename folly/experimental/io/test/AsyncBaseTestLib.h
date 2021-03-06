/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/experimental/io/FsUtil.h>
#include <folly/portability/GTest.h>
#include <folly/portability/Sockets.h>
#include <folly/test/TestUtils.h>

#include <folly/experimental/io/AsyncBase.h>

namespace folly {
namespace test {

constexpr size_t kAlign = 4096; // align reads to 4096 B (for O_DIRECT)

struct TestSpec {
  off_t start;
  size_t size;
};

struct TestUtil {
  static void waitUntilReadable(int fd);
  static folly::Range<folly::AsyncBase::Op**> readerWait(
      folly::AsyncBase* reader);
  using ManagedBuffer = std::unique_ptr<char, void (*)(void*)>;
  static ManagedBuffer allocateAligned(size_t size);
};

// Temporary file that is NOT kept open but is deleted on exit.
// Generate random-looking but reproduceable data.
class TemporaryFile {
 public:
  explicit TemporaryFile(size_t size);
  ~TemporaryFile();

  const folly::fs::path path() const {
    return path_;
  }

  static TemporaryFile& getTempFile();

 private:
  folly::fs::path path_;
};

template <typename TAsync>
void testReadsSerially(
    const std::vector<TestSpec>& specs,
    folly::AsyncBase::PollMode pollMode) {
  TAsync aioReader(1, pollMode);
  typename TAsync::Op op;
  int fd =
      ::open(TemporaryFile::getTempFile().path().c_str(), O_DIRECT | O_RDONLY);
  SKIP_IF(fd == -1) << "Tempfile can't be opened with O_DIRECT: "
                    << folly::errnoStr(errno);
  SCOPE_EXIT {
    ::close(fd);
  };

  for (size_t i = 0; i < specs.size(); i++) {
    auto buf = TestUtil::allocateAligned(specs[i].size);
    op.pread(fd, buf.get(), specs[i].size, specs[i].start);
    aioReader.submit(&op);
    EXPECT_EQ((i + 1), aioReader.totalSubmits());
    EXPECT_EQ(aioReader.pending(), 1);
    auto ops = test::TestUtil::readerWait(&aioReader);
    EXPECT_EQ(1, ops.size());
    EXPECT_TRUE(ops[0] == &op);
    EXPECT_EQ(aioReader.pending(), 0);
    ssize_t res = op.result();
    EXPECT_LE(0, res) << folly::errnoStr(-res);
    EXPECT_EQ(specs[i].size, res);
    op.reset();
  }
}

template <typename TAsync>
void testReadsParallel(
    const std::vector<TestSpec>& specs,
    folly::AsyncBase::PollMode pollMode,
    bool multithreaded) {
  TAsync aioReader(specs.size(), pollMode);
  std::unique_ptr<typename TAsync::Op[]> ops(new
                                             typename TAsync::Op[specs.size()]);
  uintptr_t sizeOf = sizeof(typename TAsync::Op);
  std::vector<TestUtil::ManagedBuffer> bufs;
  bufs.reserve(specs.size());

  int fd =
      ::open(TemporaryFile::getTempFile().path().c_str(), O_DIRECT | O_RDONLY);
  SKIP_IF(fd == -1) << "Tempfile can't be opened with O_DIRECT: "
                    << folly::errnoStr(errno);
  SCOPE_EXIT {
    ::close(fd);
  };

  std::vector<std::thread> threads;
  if (multithreaded) {
    threads.reserve(specs.size());
  }
  for (size_t i = 0; i < specs.size(); i++) {
    bufs.push_back(TestUtil::allocateAligned(specs[i].size));
  }
  auto submit = [&](size_t i) {
    ops[i].pread(fd, bufs[i].get(), specs[i].size, specs[i].start);
    aioReader.submit(&ops[i]);
  };
  for (size_t i = 0; i < specs.size(); i++) {
    if (multithreaded) {
      threads.emplace_back([&submit, i] { submit(i); });
    } else {
      submit(i);
    }
  }
  for (auto& t : threads) {
    t.join();
  }
  std::vector<bool> pending(specs.size(), true);

  size_t remaining = specs.size();
  while (remaining != 0) {
    EXPECT_EQ(remaining, aioReader.pending());
    auto completed = test::TestUtil::readerWait(&aioReader);
    size_t nrRead = completed.size();
    EXPECT_NE(nrRead, 0);
    remaining -= nrRead;

    for (size_t i = 0; i < nrRead; i++) {
      int id = (reinterpret_cast<uintptr_t>(completed[i]) -
                reinterpret_cast<uintptr_t>(ops.get())) /
          sizeOf;
      EXPECT_GE(id, 0);
      EXPECT_LT(id, specs.size());
      EXPECT_TRUE(pending[id]);
      pending[id] = false;
      ssize_t res = ops[id].result();
      EXPECT_LE(0, res) << folly::errnoStr(-res);
      EXPECT_EQ(specs[id].size, res);
    }
  }
  EXPECT_EQ(specs.size(), aioReader.totalSubmits());

  EXPECT_EQ(aioReader.pending(), 0);
  for (size_t i = 0; i < pending.size(); i++) {
    EXPECT_FALSE(pending[i]);
  }
}

template <typename TAsync>
void testReadsQueued(
    const std::vector<TestSpec>& specs,
    folly::AsyncBase::PollMode pollMode) {
  size_t readerCapacity = std::max(specs.size() / 2, size_t(1));
  TAsync aioReader(readerCapacity, pollMode);
  folly::AsyncBaseQueue aioQueue(&aioReader);
  std::unique_ptr<typename TAsync::Op[]> ops(new
                                             typename TAsync::Op[specs.size()]);
  uintptr_t sizeOf = sizeof(typename TAsync::Op);
  std::vector<TestUtil::ManagedBuffer> bufs;

  int fd =
      ::open(TemporaryFile::getTempFile().path().c_str(), O_DIRECT | O_RDONLY);
  SKIP_IF(fd == -1) << "Tempfile can't be opened with O_DIRECT: "
                    << folly::errnoStr(errno);
  SCOPE_EXIT {
    ::close(fd);
  };
  for (size_t i = 0; i < specs.size(); i++) {
    bufs.push_back(TestUtil::allocateAligned(specs[i].size));
    ops[i].pread(fd, bufs[i].get(), specs[i].size, specs[i].start);
    aioQueue.submit(&ops[i]);
  }
  std::vector<bool> pending(specs.size(), true);

  size_t remaining = specs.size();
  while (remaining != 0) {
    if (remaining >= readerCapacity) {
      EXPECT_EQ(readerCapacity, aioReader.pending());
      EXPECT_EQ(remaining - readerCapacity, aioQueue.queued());
    } else {
      EXPECT_EQ(remaining, aioReader.pending());
      EXPECT_EQ(0, aioQueue.queued());
    }
    auto completed = test::TestUtil::readerWait(&aioReader);
    size_t nrRead = completed.size();
    EXPECT_NE(nrRead, 0);
    remaining -= nrRead;

    for (size_t i = 0; i < nrRead; i++) {
      int id = (reinterpret_cast<uintptr_t>(completed[i]) -
                reinterpret_cast<uintptr_t>(ops.get())) /
          sizeOf;
      EXPECT_GE(id, 0);
      EXPECT_LT(id, specs.size());
      EXPECT_TRUE(pending[id]);
      pending[id] = false;
      ssize_t res = ops[id].result();
      EXPECT_LE(0, res) << folly::errnoStr(-res);
      EXPECT_EQ(specs[id].size, res);
    }
  }
  EXPECT_EQ(specs.size(), aioReader.totalSubmits());
  EXPECT_EQ(aioReader.pending(), 0);
  EXPECT_EQ(aioQueue.queued(), 0);
  for (size_t i = 0; i < pending.size(); i++) {
    EXPECT_FALSE(pending[i]);
  }
}

template <typename TAsync>
void testReads(
    const std::vector<TestSpec>& specs,
    folly::AsyncBase::PollMode pollMode) {
  testReadsSerially<TAsync>(specs, pollMode);
  testReadsParallel<TAsync>(specs, pollMode, false);
  testReadsParallel<TAsync>(specs, pollMode, true);
  testReadsQueued<TAsync>(specs, pollMode);
}

template <typename T>
class AsyncTest : public ::testing::Test {};
TYPED_TEST_CASE_P(AsyncTest);

TYPED_TEST_P(AsyncTest, ZeroAsyncDataNotPollable) {
  test::testReads<TypeParam>({{0, 0}}, folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, ZeroAsyncDataPollable) {
  test::testReads<TypeParam>({{0, 0}}, folly::AsyncBase::POLLABLE);
}

TYPED_TEST_P(AsyncTest, SingleAsyncDataNotPollable) {
  test::testReads<TypeParam>(
      {{0, test::kAlign}}, folly::AsyncBase::NOT_POLLABLE);
  test::testReads<TypeParam>(
      {{0, test::kAlign}}, folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, SingleAsyncDataPollable) {
  test::testReads<TypeParam>({{0, test::kAlign}}, folly::AsyncBase::POLLABLE);
  test::testReads<TypeParam>({{0, test::kAlign}}, folly::AsyncBase::POLLABLE);
}

TYPED_TEST_P(AsyncTest, MultipleAsyncDataNotPollable) {
  test::testReads<TypeParam>(
      {{test::kAlign, 2 * test::kAlign},
       {test::kAlign, 2 * test::kAlign},
       {test::kAlign, 4 * test::kAlign}},
      folly::AsyncBase::NOT_POLLABLE);
  test::testReads<TypeParam>(
      {{test::kAlign, 2 * test::kAlign},
       {test::kAlign, 2 * test::kAlign},
       {test::kAlign, 4 * test::kAlign}},
      folly::AsyncBase::NOT_POLLABLE);

  test::testReads<TypeParam>(
      {{0, 5 * 1024 * 1024}, {test::kAlign, 5 * 1024 * 1024}},
      folly::AsyncBase::NOT_POLLABLE);

  test::testReads<TypeParam>(
      {
          {test::kAlign, 0},
          {test::kAlign, test::kAlign},
          {test::kAlign, 2 * test::kAlign},
          {test::kAlign, 20 * test::kAlign},
          {test::kAlign, 1024 * 1024},
      },
      folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, MultipleAsyncDataPollable) {
  test::testReads<TypeParam>(
      {{test::kAlign, 2 * test::kAlign},
       {test::kAlign, 2 * test::kAlign},
       {test::kAlign, 4 * test::kAlign}},
      folly::AsyncBase::POLLABLE);
  test::testReads<TypeParam>(
      {{test::kAlign, 2 * test::kAlign},
       {test::kAlign, 2 * test::kAlign},
       {test::kAlign, 4 * test::kAlign}},
      folly::AsyncBase::POLLABLE);

  test::testReads<TypeParam>(
      {{0, 5 * 1024 * 1024}, {test::kAlign, 5 * 1024 * 1024}},
      folly::AsyncBase::NOT_POLLABLE);

  test::testReads<TypeParam>(
      {
          {test::kAlign, 0},
          {test::kAlign, test::kAlign},
          {test::kAlign, 2 * test::kAlign},
          {test::kAlign, 20 * test::kAlign},
          {test::kAlign, 1024 * 1024},
      },
      folly::AsyncBase::NOT_POLLABLE);
}

TYPED_TEST_P(AsyncTest, ManyAsyncDataNotPollable) {
  {
    std::vector<test::TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back({off_t(test::kAlign * i), test::kAlign});
    }
    test::testReads<TypeParam>(v, folly::AsyncBase::NOT_POLLABLE);
  }
}

TYPED_TEST_P(AsyncTest, ManyAsyncDataPollable) {
  {
    std::vector<test::TestSpec> v;
    for (int i = 0; i < 1000; i++) {
      v.push_back({off_t(test::kAlign * i), test::kAlign});
    }
    test::testReads<TypeParam>(v, folly::AsyncBase::POLLABLE);
  }
}

TYPED_TEST_P(AsyncTest, NonBlockingWait) {
  TypeParam aioReader(1, folly::AsyncBase::NOT_POLLABLE);
  typename TypeParam::Op op;
  int fd = ::open(
      test::TemporaryFile::getTempFile().path().c_str(), O_DIRECT | O_RDONLY);
  SKIP_IF(fd == -1) << "Tempfile can't be opened with O_DIRECT: "
                    << folly::errnoStr(errno);
  SCOPE_EXIT {
    ::close(fd);
  };
  size_t size = 2 * test::kAlign;
  auto buf = test::TestUtil::allocateAligned(size);
  op.pread(fd, buf.get(), size, 0);
  aioReader.submit(&op);
  EXPECT_EQ(aioReader.pending(), 1);

  folly::Range<folly::AsyncBase::Op**> completed;
  while (completed.empty()) {
    // poll without blocking until the read request completes.
    completed = aioReader.wait(0);
  }
  EXPECT_EQ(completed.size(), 1);

  EXPECT_TRUE(completed[0] == &op);
  ssize_t res = op.result();
  EXPECT_LE(0, res) << folly::errnoStr(-res);
  EXPECT_EQ(size, res);
  EXPECT_EQ(aioReader.pending(), 0);
}

TYPED_TEST_P(AsyncTest, Cancel) {
  constexpr size_t kNumOpsBatch1 = 10;
  constexpr size_t kNumOpsBatch2 = 10;

  TypeParam aioReader(
      kNumOpsBatch1 + kNumOpsBatch2, folly::AsyncBase::NOT_POLLABLE);
  int fd = ::open(
      test::TemporaryFile::getTempFile().path().c_str(), O_DIRECT | O_RDONLY);
  SKIP_IF(fd == -1) << "Tempfile can't be opened with O_DIRECT: "
                    << folly::errnoStr(errno);
  SCOPE_EXIT {
    ::close(fd);
  };

  size_t completed = 0;

  std::vector<std::unique_ptr<folly::AsyncBase::Op>> ops;
  std::vector<test::TestUtil::ManagedBuffer> bufs;
  const auto schedule = [&](size_t n) {
    for (size_t i = 0; i < n; ++i) {
      const size_t size = 2 * test::kAlign;
      bufs.push_back(test::TestUtil::allocateAligned(size));

      ops.push_back(std::make_unique<typename TypeParam::Op>());
      auto& op = *ops.back();

      op.setNotificationCallback([&](folly::AsyncBaseOp*) { ++completed; });
      op.pread(fd, bufs.back().get(), size, 0);
      aioReader.submit(&op);
    }
  };

  // Mix completed and canceled operations for this test.
  // In order to achieve that, schedule in two batches and do partial
  // wait() after the first one.

  schedule(kNumOpsBatch1);
  EXPECT_EQ(aioReader.pending(), kNumOpsBatch1);
  EXPECT_EQ(completed, 0);

  auto result = aioReader.wait(1);
  EXPECT_GE(result.size(), 1);
  EXPECT_EQ(completed, result.size());
  EXPECT_EQ(aioReader.pending(), kNumOpsBatch1 - result.size());

  schedule(kNumOpsBatch2);
  EXPECT_EQ(aioReader.pending(), ops.size() - result.size());
  EXPECT_EQ(completed, result.size());

  auto canceled = aioReader.cancel();
  EXPECT_EQ(canceled.size(), ops.size() - result.size());
  EXPECT_EQ(aioReader.pending(), 0);
  EXPECT_EQ(completed, result.size());

  size_t foundCompleted = 0;
  for (auto& op : ops) {
    if (op->state() == folly::AsyncBaseOp::State::COMPLETED) {
      ++foundCompleted;
    } else {
      EXPECT_TRUE(op->state() == folly::AsyncBaseOp::State::CANCELED) << *op;
    }
  }
  EXPECT_EQ(foundCompleted, completed);
}

REGISTER_TYPED_TEST_CASE_P(
    AsyncTest,
    ZeroAsyncDataNotPollable,
    ZeroAsyncDataPollable,
    SingleAsyncDataNotPollable,
    SingleAsyncDataPollable,
    MultipleAsyncDataNotPollable,
    MultipleAsyncDataPollable,
    ManyAsyncDataNotPollable,
    ManyAsyncDataPollable,
    NonBlockingWait,
    Cancel);

} // namespace test
} // namespace folly
