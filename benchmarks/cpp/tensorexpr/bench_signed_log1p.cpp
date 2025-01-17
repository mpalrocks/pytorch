#include <benchmark/benchmark.h>

#include <torch/csrc/jit/jit_log.h>
#include <torch/csrc/jit/tensorexpr/ir.h>
#include <torch/csrc/jit/tensorexpr/ir_simplifier.h>
#include <torch/csrc/jit/tensorexpr/llvm_codegen.h>
#include <torch/csrc/jit/tensorexpr/loopnest.h>
#include <torch/csrc/jit/tensorexpr/tensor.h>
#include <torch/torch.h>

using namespace torch::jit::tensorexpr;

namespace {

class SignedLog1pBench : public benchmark::Fixture {
 public:
  void SetUp(const benchmark::State& state) override {
    input_size_ = {state.range(0), state.range(1)};
    input_size_int_ = {state.range(0), state.range(1)};
    input_ = torch::rand(input_size_);
    ref_ = signedLog1p(input_);
  }

  void TearDown(benchmark::State& state) override {
    TORCH_CHECK(at::allclose(ref_, output_));
    state.counters["GB/s"] = benchmark::Counter(
        uint64_t(state.iterations()) * 2 * output_.nbytes(),
        benchmark::Counter::kIsRate);
  }

  at::Tensor signedLog1p(const at::Tensor& inp) {
    auto sign = at::sign(inp);
    auto log1p = at::log1p(at::abs(inp));
    return sign * log1p;
  }

  void runATen(benchmark::State& state) {
    for (auto _ : state) {
      output_ = signedLog1p(input_);
    }
  }

  void runNNC(benchmark::State& state) {
    Placeholder input_ph(
        "input", kFloat, {input_size_int_[0], input_size_int_[1]});
    Tensor abs_result = Compute(
        "aten_abs",
        {{input_size_int_[0], "M"}, {input_size_int_[1], "N"}},
        [&](const VarHandle& m, const VarHandle& n) {
          return abs(input_ph.load(m, n));
        });
    Tensor log1p_result = Compute(
        "aten_log1p",
        {{input_size_int_[0], "M"}, {input_size_int_[1], "N"}},
        [&](const VarHandle& m, const VarHandle& n) {
          return log1p(abs_result.load(m, n));
        });
    Tensor sign = Compute(
        "aten_sign",
        {{input_size_int_[0], "M"}, {input_size_int_[1], "N"}},
        [&](const VarHandle& m, const VarHandle& n) {
          return CompareSelect::make(
              input_ph.load(m, n),
              ExprHandle(0.0f),
              ExprHandle(-1),
              ExprHandle(1),
              kLT);
        });
    Tensor output = Compute(
        "aten_mul",
        {{input_size_int_[0], "M"}, {input_size_int_[1], "N"}},
        [&](const VarHandle& m, const VarHandle& n) {
          return sign.load(m, n) * log1p_result.load(m, n);
        });
    LoopNest nest({output}, {abs_result, log1p_result, sign, output});
    GRAPH_DEBUG("Original Stmt: ", *nest.root_stmt());
    nest.inlineIntermediateBufs(true);
    nest.prepareForCodegen();
    nest.simplify();
    nest.vectorizeInnerLoops();
    nest.simplify();
    GRAPH_DEBUG("Final stmt: ", *nest.root_stmt());

    // StmtPtr s = IRSimplifier::simplify(nest.root_stmt());
    std::vector<CodeGen::BufferArg> buf_args;
    buf_args.push_back(input_ph);
    buf_args.push_back(output);
    LLVMCodeGen cg(nest.root_stmt(), buf_args);

    std::vector<CodeGen::CallArg> call_args;
    for (auto _ : state) {
      output_ = at::empty_like(ref_);
      call_args.clear();
      call_args.push_back(input_.data_ptr<float>());
      call_args.push_back(output_.data_ptr<float>());
      cg.call(call_args);
    }
  }

 private:
  std::vector<long> input_size_;
  std::vector<int> input_size_int_;
  at::Tensor input_;
  at::Tensor output_;
  at::Tensor ref_;
};

} // namespace

BENCHMARK_DEFINE_F(SignedLog1pBench, ATen)(benchmark::State& state) {
  runATen(state);
}

BENCHMARK_DEFINE_F(SignedLog1pBench, NNC)(benchmark::State& state) {
  runNNC(state);
}

BENCHMARK_REGISTER_F(SignedLog1pBench, ATen)->Args({10, 1467});

BENCHMARK_REGISTER_F(SignedLog1pBench, NNC)->Args({10, 1467});
