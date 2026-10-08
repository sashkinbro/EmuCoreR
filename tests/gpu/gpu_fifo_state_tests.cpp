// SPDX-FileCopyrightText: 2026 SBRO
// SPDX-License-Identifier: GPL-3.0-or-later
#include "common/state_wrapper.h"
#include <cstdio>

static int checks = 0;
static int failures = 0;
static void Check(bool passed, const char* name)
{
  checks++;
  failures += !passed;
  std::printf("%s %s\n", passed ? "PASS" : "FAIL", name);
}

int main()
{
  HeapFIFOQueue<uint64_t, 8> queue;
  queue.Push(99);
  auto malformed = ByteStream_CreateGrowableMemoryStream();
  StateWrapper bad_writer(malformed.get(), StateWrapper::Mode::Write, 58);
  uint32_t size = 9;
  bad_writer.Do(&size);
  uint64_t words[9] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  bad_writer.DoArray(words, 9);
  malformed->SeekAbsolute(0);
  StateWrapper bad_reader(malformed.get(), StateWrapper::Mode::Read, 58);
  bad_reader.Do(&queue);
  Check(bad_reader.HasError(), "reject a serialized GPU FIFO larger than its storage");
  Check(queue.GetSize() == 1 && queue.Peek() == 99, "reject invalid FIFO data without replacing the live queue");

  queue.Clear();
  for (uint64_t value = 1; value <= 8; value++) queue.Push(value);
  for (uint64_t value = 1; value <= 3; value++) queue.Pop();
  for (uint64_t value = 9; value <= 11; value++) queue.Push(value);
  auto valid = ByteStream_CreateGrowableMemoryStream();
  StateWrapper writer(valid.get(), StateWrapper::Mode::Write, 58);
  writer.Do(&queue);
  Check(!writer.HasError(), "save a full wrapped FIFO");
  queue.Clear();
  valid->SeekAbsolute(0);
  StateWrapper reader(valid.get(), StateWrapper::Mode::Read, 58);
  reader.Do(&queue);
  Check(!reader.HasError() && queue.GetSize() == 8, "restore a full wrapped FIFO");
  bool ordered = true;
  for (uint64_t expected = 4; expected <= 11; expected++) ordered &= queue.Pop() == expected;
  Check(ordered, "preserve wrapped FIFO command order");

  queue.Push(77);
  auto truncated = ByteStream_CreateGrowableMemoryStream();
  StateWrapper truncated_writer(truncated.get(), StateWrapper::Mode::Write, 58);
  size = 2;
  truncated_writer.Do(&size);
  truncated_writer.Do(&words[0]);
  truncated->SeekAbsolute(0);
  StateWrapper truncated_reader(truncated.get(), StateWrapper::Mode::Read, 58);
  truncated_reader.Do(&queue);
  Check(truncated_reader.HasError(), "reject truncated FIFO command data");
  Check(queue.GetSize() == 1 && queue.Peek() == 77, "preserve the live FIFO after a truncated load");

  std::printf("GPU FIFO: %d checks, %d failures\n", checks, failures);
  return failures ? 1 : 0;
}
