#include <google/protobuf/text_format.h>
#include <gtest/gtest.h>
#include "caffe2/contrib/transform/graph.h"
#include "caffe2/core/net.h"
#include "caffe2/core/operator.h"

namespace caffe2 {

namespace {

using transform::Graph;

static std::atomic<int> counter;

class DummyOp final : public OperatorBase {
 public:
  using OperatorBase::OperatorBase;
  bool Run(int /* unused */) override {
    counter.fetch_add(1);
    return true;
  }
};

REGISTER_CPU_OPERATOR(DummyOp1, DummyOp);
REGISTER_CUDA_OPERATOR(DummyOp1, DummyOp);

OPERATOR_SCHEMA(DummyOp1)
    .NumInputs(0, INT_MAX)
    .NumOutputs(0, INT_MAX)
    .AllowInplace({{0, 0}, {1, 1}});

REGISTER_CPU_OPERATOR(DummyOp2, DummyOp);
REGISTER_CUDA_OPERATOR(DummyOp2, DummyOp);

OPERATOR_SCHEMA(DummyOp2)
    .NumInputs(0, INT_MAX)
    .NumOutputs(0, INT_MAX)
    .AllowInplace({{0, 0}, {1, 1}});

REGISTER_CPU_OPERATOR(DummyOp3, DummyOp);
REGISTER_CUDA_OPERATOR(DummyOp3, DummyOp);

OPERATOR_SCHEMA(DummyOp3)
    .NumInputs(0, INT_MAX)
    .NumOutputs(0, INT_MAX)
    .AllowInplace({{0, 0}, {1, 1}});

// Adds an operator def to a netdef.
// Returns the ptr, if you want to add anything extra (such as device_option)
OperatorDef* AddOp(
    NetDef* netdef_ptr,
    string op_type,
    std::vector<string> inputs,
    std::vector<string> outputs) {
  CHECK(netdef_ptr);
  auto& netdef = *netdef_ptr;
  auto op_ptr = netdef.add_op();
  auto& op = *op_ptr;
  op.set_type(op_type);
  for (const string& inp : inputs) {
    op.add_input(inp);
  }
  for (const string& outp : outputs) {
    op.add_output(outp);
  }
  return op_ptr;
}

// Checks if two netdefs are  in terms of type, input, and output.
void compare_netdefs(const NetDef& net_a, const NetDef& net_b) {
  EXPECT_EQ(net_a.op_size(), net_b.op_size());
  for (int i = 0; i < net_a.op_size(); i++) {
    EXPECT_EQ(net_a.op(i).type(), net_b.op(i).type());
    EXPECT_EQ(net_a.op(i).input_size(), net_b.op(i).input_size());
    for (int j = 0; j < net_a.op(i).input_size(); j++) {
      EXPECT_EQ(net_a.op(i).input(j), net_b.op(i).input(j));
    }
    EXPECT_EQ(net_a.op(i).output_size(), net_b.op(i).output_size());
    for (int j = 0; j < net_a.op(i).output_size(); j++) {
      EXPECT_EQ(net_a.op(i).output(j), net_b.op(i).output(j));
    }
  }
}

TEST(GraphTest, TestGenerateGraphChain) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef netdef;
  AddOp(&netdef, "DummyOp1", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"mid1"}, {"mid2"});
  AddOp(&netdef, "DummyOp1", {"mid2"}, {"mid3"});
  AddOp(&netdef, "DummyOp2", {"mid3"}, {"out"});
  Graph g(netdef);
  EXPECT_EQ(g.size(), 4);
  for (int i = 0; i < 4; i++) {
    if (i < 3) {
      EXPECT_EQ(g.node(i).children.size(), 1);
      EXPECT_TRUE(g.node(i).children.count(i + 1));
    }
    if (i > 0) {
      EXPECT_EQ(g.node(i).parents.size(), 1);
      EXPECT_TRUE(g.node(i).parents.count(i - 1));
    }
  }
  NetDef retrieved_net = g.GetNetDef();
  compare_netdefs(retrieved_net, netdef);
}

TEST(GraphTest, TestGenerateGraphChainInPlace) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef netdef;
  AddOp(&netdef, "DummyOp1", {"in"}, {"out"});
  AddOp(&netdef, "DummyOp2", {"out"}, {"out"});
  AddOp(&netdef, "DummyOp1", {"out"}, {"out"});
  AddOp(&netdef, "DummyOp2", {"out"}, {"out"});
  Graph g(netdef);
  EXPECT_EQ(g.size(), 4);
  for (int i = 0; i < 4; i++) {
    if (i < 3) {
      EXPECT_EQ(g.node(i).children.size(), 1);
      EXPECT_TRUE(g.node(i).children.count(i + 1));
    }
    if (i > 0) {
      EXPECT_EQ(g.node(i).parents.size(), 1);
      EXPECT_TRUE(g.node(i).parents.count(i - 1));
    }
  }
  NetDef retrieved_net = g.GetNetDef();
  compare_netdefs(retrieved_net, netdef);
}

// Diamond Graph
TEST(GraphTest, TestGenerateGraphBranch) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef netdef;

  AddOp(&netdef, "DummyOp1", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"mid1"}, {"mid2"});
  AddOp(&netdef, "DummyOp2", {"mid1"}, {"mid3"});
  AddOp(&netdef, "DummyOp3", {"mid2", "mid3"}, {"out"});

  Graph g(netdef);

  EXPECT_EQ(g.size(), 4);
  EXPECT_EQ(g.node(0).parents.size(), 0);
  EXPECT_EQ(g.node(0).children.size(), 2);
  EXPECT_EQ(g.node(1).parents.size(), 1);
  EXPECT_EQ(g.node(1).children.size(), 1);
  EXPECT_EQ(g.node(2).parents.size(), 1);
  EXPECT_EQ(g.node(2).children.size(), 1);
  EXPECT_EQ(g.node(3).parents.size(), 2);
  EXPECT_EQ(g.node(3).children.size(), 0);

  NetDef retrieved_net = g.GetNetDef();
  compare_netdefs(retrieved_net, netdef);
}

// Double Diamond Graph, reused names
TEST(GraphTest, TestReusedInputs) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef netdef;

  AddOp(&netdef, "DummyOp1", {"in"}, {"in"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid2"});
  AddOp(&netdef, "DummyOp3", {"mid1", "mid2"}, {"in"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid2"});
  AddOp(&netdef, "DummyOp3", {"mid1", "mid2"}, {"in"});

  Graph g(netdef);

  EXPECT_EQ(g.size(), 7);
  EXPECT_EQ(g.node(0).parents.size(), 0);
  EXPECT_EQ(g.node(0).children.size(), 2);
  EXPECT_EQ(g.node(1).parents.size(), 1);
  EXPECT_EQ(g.node(1).children.size(), 1);
  EXPECT_EQ(g.node(2).parents.size(), 1);
  EXPECT_EQ(g.node(2).children.size(), 1);
  EXPECT_EQ(g.node(3).parents.size(), 2);
  EXPECT_EQ(g.node(3).children.size(), 2);
  EXPECT_EQ(g.node(4).parents.size(), 1);
  EXPECT_EQ(g.node(4).children.size(), 1);
  EXPECT_EQ(g.node(5).parents.size(), 1);
  EXPECT_EQ(g.node(5).children.size(), 1);
  EXPECT_EQ(g.node(6).parents.size(), 2);
  EXPECT_EQ(g.node(6).children.size(), 0);

  NetDef retrieved_net = g.GetNetDef();
  compare_netdefs(retrieved_net, netdef);
}

TEST(GraphTest, TestGetPerimeter) {
  Workspace ws;
  ws.CreateBlob("in");
  NetDef netdef;

  AddOp(&netdef, "DummyOp1", {"in"}, {"in"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid2"});
  AddOp(&netdef, "DummyOp3", {"mid1", "mid2"}, {"in"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid1"});
  AddOp(&netdef, "DummyOp2", {"in"}, {"mid2"});
  AddOp(&netdef, "DummyOp1", {"mid1", "mid2"}, {"in"});

  Graph g(netdef);
  std::vector<int> subgraph = {3};

  auto subgraph_input = g.GetSubgraphInput(subgraph);
  EXPECT_EQ(subgraph_input.size(), 2);
  EXPECT_EQ(subgraph_input[0], std::make_pair(string("mid1"), 1));
  EXPECT_EQ(subgraph_input[1], std::make_pair(string("mid2"), 2));

  auto subgraph_output = g.GetSubgraphOutput(subgraph);
  EXPECT_EQ(subgraph_output.size(), 2);
  EXPECT_EQ(subgraph_output[0], std::make_pair(string("in"), 4));
  EXPECT_EQ(subgraph_output[1], std::make_pair(string("in"), 5));
}

} // namespace

} // namespace caffe2
