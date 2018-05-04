// Copyright 2018 The Crashpad Authors. All rights reserved.
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

#include "snapshot/fuchsia/process_reader_fuchsia.h"

#include <pthread.h>
#include <zircon/process.h>
#include <zircon/syscalls.h>

#include "gtest/gtest.h"
#include "test/multiprocess_exec.h"
#include "util/fuchsia/scoped_task_suspend.h"

namespace crashpad {
namespace test {
namespace {

TEST(ProcessReaderFuchsia, SelfBasic) {
  ProcessReaderFuchsia process_reader;
  ASSERT_TRUE(process_reader.Initialize(zx_process_self()));

  static constexpr char kTestMemory[] = "Some test memory";
  char buffer[arraysize(kTestMemory)];
  ASSERT_TRUE(process_reader.Memory()->Read(
      reinterpret_cast<zx_vaddr_t>(kTestMemory), sizeof(kTestMemory), &buffer));
  EXPECT_STREQ(kTestMemory, buffer);

  const auto& modules = process_reader.Modules();
  EXPECT_GT(modules.size(), 0u);
  for (const auto& module : modules) {
    EXPECT_FALSE(module.name.empty());
    EXPECT_NE(module.type, ModuleSnapshot::kModuleTypeUnknown);
  }

  const auto& threads = process_reader.Threads();
  EXPECT_GT(threads.size(), 0u);

  zx_info_handle_basic_t info;
  ASSERT_EQ(zx_object_get_info(zx_thread_self(),
                               ZX_INFO_HANDLE_BASIC,
                               &info,
                               sizeof(info),
                               nullptr,
                               nullptr),
            ZX_OK);
  EXPECT_EQ(threads[0].id, info.koid);
  EXPECT_EQ(threads[0].state, ZX_THREAD_STATE_RUNNING);
  EXPECT_EQ(threads[0].name, "initial-thread");
}

constexpr char kTestMemory[] = "Read me from another process";

CRASHPAD_CHILD_TEST_MAIN(ProcessReaderBasicChildTestMain) {
  zx_vaddr_t addr = reinterpret_cast<zx_vaddr_t>(kTestMemory);
  CheckedWriteFile(
      StdioFileHandle(StdioStream::kStandardOutput), &addr, sizeof(addr));
  CheckedReadFileAtEOF(StdioFileHandle(StdioStream::kStandardInput));
  return 0;
}

class BasicChildTest : public MultiprocessExec {
 public:
  BasicChildTest() : MultiprocessExec() {
    SetChildTestMainFunction("ProcessReaderBasicChildTestMain");
  }
  ~BasicChildTest() {}

 private:
  void MultiprocessParent() override {
    ProcessReaderFuchsia process_reader;
    ASSERT_TRUE(process_reader.Initialize(ChildProcess()));

    zx_vaddr_t addr;
    ASSERT_TRUE(ReadFileExactly(ReadPipeHandle(), &addr, sizeof(addr)));

    std::string read_string;
    ASSERT_TRUE(process_reader.Memory()->ReadCString(addr, &read_string));
    EXPECT_EQ(read_string, kTestMemory);
  }

  DISALLOW_COPY_AND_ASSIGN(BasicChildTest);
};

TEST(ProcessReaderFuchsia, ChildBasic) {
  BasicChildTest test;
  test.Run();
}

void* SignalAndSleep(void* arg) {
  zx_object_signal(*reinterpret_cast<zx_handle_t*>(arg), 0, ZX_EVENT_SIGNALED);
  zx_nanosleep(UINT64_MAX);
  return nullptr;
}

CRASHPAD_CHILD_TEST_MAIN(ProcessReaderChildThreadsTestMain) {
  // Create 5 threads with stack sizes of 4096, 8192, ...
  zx_handle_t events[5];
  zx_wait_item_t items[arraysize(events)];
  for (size_t i = 0; i < arraysize(events); ++i) {
    pthread_t thread;
    EXPECT_EQ(zx_event_create(0, &events[i]), ZX_OK);
    pthread_attr_t attr;
    EXPECT_EQ(pthread_attr_init(&attr), 0);
    EXPECT_EQ(pthread_attr_setstacksize(&attr, (i + 1) * 4096), 0);
    EXPECT_EQ(pthread_create(&thread, &attr, &SignalAndSleep, &events[i]), 0);
    items[i].waitfor = ZX_EVENT_SIGNALED;
    items[i].handle = events[i];
  }

  EXPECT_EQ(zx_object_wait_many(items, arraysize(items), ZX_TIME_INFINITE),
            ZX_OK);

  char c = ' ';
  CheckedWriteFile(
      StdioFileHandle(StdioStream::kStandardOutput), &c, sizeof(c));
  CheckedReadFileAtEOF(StdioFileHandle(StdioStream::kStandardInput));
  return 0;
}

class ThreadsChildTest : public MultiprocessExec {
 public:
  ThreadsChildTest() : MultiprocessExec() {
    SetChildTestMainFunction("ProcessReaderChildThreadsTestMain");
  }
  ~ThreadsChildTest() {}

 private:
  void MultiprocessParent() override {
    char c;
    ASSERT_TRUE(ReadFileExactly(ReadPipeHandle(), &c, 1));
    ASSERT_EQ(c, ' ');

    ScopedTaskSuspend suspend(ChildProcess());

    ProcessReaderFuchsia process_reader;
    ASSERT_TRUE(process_reader.Initialize(ChildProcess()));

    const auto& threads = process_reader.Threads();
    EXPECT_EQ(threads.size(), 6u);

    for (size_t i = 1; i < 6; ++i) {
      ASSERT_GT(threads[i].stack_regions.size(), 0u);
      EXPECT_EQ(threads[i].stack_regions[0].size(), i * 4096u);
    }
  }

  DISALLOW_COPY_AND_ASSIGN(ThreadsChildTest);
};

TEST(ProcessReaderFuchsia, ChildThreads) {
  ThreadsChildTest test;
  test.Run();
}

}  // namespace
}  // namespace test
}  // namespace crashpad