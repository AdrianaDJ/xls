// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/codegen/state_removal_pass.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "xls/common/status/matchers.h"
#include "xls/interpreter/channel_queue.h"
#include "xls/interpreter/proc_network_interpreter.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;
using ::testing::ElementsAre;

class StateRemovalPassTest : public IrTestBase {
 protected:
  absl::StatusOr<bool> Run(Proc* proc) {
    PassResults results;
    XLS_ASSIGN_OR_RETURN(bool changed, StateRemovalPass().RunOnProc(
                                           proc, PassOptions(), &results));
    return changed;
  }
};

TEST_F(StateRemovalPassTest, SimpleProc) {
  auto p = CreatePackage();
  ProcBuilder pb(TestName(), /*init_value=*/Value(UBits(42, 32)), "tkn", "st",
                 p.get());
  BValue add = pb.Add(pb.Literal(UBits(1, 32)), pb.GetStateParam());
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, pb.Build(pb.GetTokenParam(), add));

  EXPECT_EQ(proc->StateType(), p->GetBitsType(32));

  EXPECT_THAT(Run(proc), IsOkAndHolds(true));

  EXPECT_EQ(proc->StateType(), p->GetTupleType({}));

  XLS_ASSERT_OK_AND_ASSIGN(Channel * state_channel,
                           p->GetChannel(StateRemovalPass::kStateChannelName));
  EXPECT_EQ(state_channel->data_elements().size(), 1);
  EXPECT_EQ(state_channel->data_element(0).name, "st");
  EXPECT_EQ(state_channel->data_element(0).type, p->GetBitsType(32));
  EXPECT_EQ(state_channel->initial_values().size(), 1);
  EXPECT_THAT(state_channel->initial_values()[0],
              ElementsAre(Value(UBits(42, 32))));

  EXPECT_THAT(
      proc->NextToken(),
      m::AfterAll(m::Param(),
                  m::Send(m::TupleIndex(m::Receive(), /*index=*/0),
                          /*data=*/{m::Add(
                              m::Literal(),
                              m::TupleIndex(m::Receive(), /*index=*/1))})));
}

TEST_F(StateRemovalPassTest, ProcWithNilState) {
  auto p = CreatePackage();
  ProcBuilder pb(TestName(), /*init_value=*/Value::Tuple({}), "tkn", "st",
                 p.get());
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc,
                           pb.Build(pb.GetTokenParam(), pb.Tuple({})));

  EXPECT_THAT(Run(proc), IsOkAndHolds(false));
}

TEST_F(StateRemovalPassTest, InterpretAccumulatorProc) {
  std::string input = R"(
package test

chan in(data: bits[32], kind=streaming, id=0, ops=receive_only, metadata="")
chan out(data: bits[32], kind=streaming, id=1, ops=send_only, metadata="")

proc accumulator(tkn: token, accum: bits[32], init=100) {
  input_recv: (token, bits[32]) = receive(tkn, channel_id=0)
  input: bits[32] = tuple_index(input_recv, index=1)
  new_accum: bits[32] = add(input, accum)
  rcv_tkn: token = tuple_index(input_recv, index=0)
  out_send: token = send(rcv_tkn, data=[new_accum], channel_id=1)
  next (out_send, new_accum)
}
)";
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(input));
  Proc* proc = FindProc("accumulator", p.get());

  XLS_ASSERT_OK_AND_ASSIGN(Channel * input_channel, p->GetChannel("in"));
  XLS_ASSERT_OK_AND_ASSIGN(Channel * output_channel, p->GetChannel("out"));

  std::vector<ChannelData> inputs = {
      {Value(UBits(10, 32))}, {Value(UBits(20, 32))}, {Value(UBits(30, 32))}};
  {
    // Verify results before transformation.
    std::vector<std::unique_ptr<RxOnlyChannelQueue>> rx_only_queues;
    rx_only_queues.push_back(absl::make_unique<FixedRxOnlyChannelQueue>(
        input_channel, p.get(), inputs));
    XLS_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<ProcNetworkInterpreter> interpreter,
        ProcNetworkInterpreter::Create(p.get(), std::move(rx_only_queues)));

    XLS_ASSERT_OK(interpreter->Tick());
    XLS_ASSERT_OK(interpreter->Tick());
    XLS_ASSERT_OK(interpreter->Tick());

    ChannelQueue& output_queue =
        interpreter->queue_manager().GetQueue(output_channel);
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(110, 32)))));
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(130, 32)))));
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(160, 32)))));
  }

  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_EQ(proc->StateType(), p->GetTupleType({}));

  {
    // Verify results after transformation.
    std::vector<std::unique_ptr<RxOnlyChannelQueue>> rx_only_queues;
    rx_only_queues.push_back(absl::make_unique<FixedRxOnlyChannelQueue>(
        input_channel, p.get(), inputs));
    XLS_ASSERT_OK_AND_ASSIGN(
        std::unique_ptr<ProcNetworkInterpreter> interpreter,
        ProcNetworkInterpreter::Create(p.get(), std::move(rx_only_queues)));

    XLS_ASSERT_OK(interpreter->Tick());
    XLS_ASSERT_OK(interpreter->Tick());
    XLS_ASSERT_OK(interpreter->Tick());

    ChannelQueue& output_queue =
        interpreter->queue_manager().GetQueue(output_channel);
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(110, 32)))));
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(130, 32)))));
    EXPECT_THAT(output_queue.Dequeue(),
                IsOkAndHolds(ElementsAre(Value(UBits(160, 32)))));
  }
}

}  // namespace
}  // namespace xls
