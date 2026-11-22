#include "msime_scorer.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

#include "vendor/onnxruntime/onnxruntime_c_api.h"

namespace xmjd::msime {
namespace {
constexpr int64_t kLayers = 6;
constexpr int64_t kHeads = 4;
constexpr int64_t kHeadDim = 64;
constexpr int64_t kWidth = 256;
constexpr int32_t kVocabulary = 12288;
constexpr int32_t kBos = 2;
constexpr size_t kMaxContext = 65;
constexpr size_t kMaxCandidates = 32;
constexpr size_t kMaxLength = 16;
constexpr std::array<const char*, 10> kInputs{
    "cache_k", "cache_v", "cache_h", "cache_z", "nodes", "positions", "visible",
    "candidates", "steps", "lengths"};
constexpr std::array<const char*, 5> kOutputs{"scores", "next_k", "next_v", "next_h", "next_z"};

void CheckToken(int32_t token) {
    if (token < 0 || token >= kVocabulary) throw std::invalid_argument("MSIME token out of vocabulary");
}
}  // namespace

class Scorer::Impl {
  public:
    Impl(const std::filesystem::path& model_path, const std::filesystem::path& runtime_library) {
        try {
            if (!model_path.is_absolute() || !runtime_library.is_absolute())
                throw std::invalid_argument("MSIME requires absolute model and runtime paths");
            library_ = LoadLibraryExW(runtime_library.c_str(), nullptr,
                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
            if (!library_) throw std::runtime_error("MSIME ONNX Runtime library is unavailable");
            auto get_base = reinterpret_cast<const OrtApiBase* (ORT_API_CALL*)()>(
                GetProcAddress(library_, "OrtGetApiBase"));
            if (!get_base) throw std::runtime_error("MSIME OrtGetApiBase is missing");
            const OrtApiBase* base = get_base();
            if (!base) throw std::runtime_error("MSIME ONNX Runtime API base is null");
            api_ = base->GetApi(ORT_API_VERSION);
            if (!api_) throw std::runtime_error("MSIME requires ONNX Runtime C API 24 or newer");
            runtime_version_ = base->GetVersionString();
            Check(api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "xmjd-msime", &environment_));
            Check(api_->CreateSessionOptions(&options_));
            Check(api_->SetIntraOpNumThreads(options_, 1));
            Check(api_->SetInterOpNumThreads(options_, 1));
            Check(api_->SetSessionExecutionMode(options_, ORT_SEQUENTIAL));
            // Prefix counts vary per key. Do not keep shape-specific patterns or
            // large candidate-only arena allocations after a completed request.
            Check(api_->DisableMemPattern(options_));
            Check(api_->DisableCpuMemArena(options_));
            Check(api_->CreateSession(environment_, model_path.c_str(), options_, &session_));
            Check(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_));
            ValidateSessionNames();
        } catch (...) {
            Release();
            throw;
        }
    }

    ~Impl() { Release(); }

    std::vector<float> Score(const std::vector<int32_t>& context,
                             const std::vector<std::vector<int32_t>>& candidates) {
        if (context.empty() || context.size() > kMaxContext || context.front() != kBos) {
            throw std::invalid_argument("MSIME context must contain BOS and at most 64 characters");
        }
        for (int32_t token : context) CheckToken(token);
        if (candidates.empty() || candidates.size() > kMaxCandidates) {
            throw std::invalid_argument("MSIME requires 1 to 32 candidates");
        }
        size_t width = 0;
        for (const auto& candidate : candidates) {
            if (candidate.empty() || candidate.size() > kMaxLength) {
                throw std::invalid_argument("MSIME candidates must contain 1 to 16 characters");
            }
            for (int32_t token : candidate) CheckToken(token);
            width = std::max(width, candidate.size());
        }
        if (context.size() + width > 256) throw std::invalid_argument("MSIME position window exceeded");
        if (context != context_ids_) Prefill(context);

        const int64_t n_context = static_cast<int64_t>(context.size());
        std::map<std::vector<int64_t>, int64_t> prefix_indices;
        std::vector<std::vector<int64_t>> prefixes;
        for (const auto& candidate : candidates) {
            std::vector<int64_t> prefix;
            for (size_t index = 0; index + 1 < candidate.size(); ++index) {
                prefix.push_back(candidate[index]);
                if (prefix_indices.find(prefix) == prefix_indices.end()) {
                    prefix_indices.emplace(prefix, n_context + static_cast<int64_t>(prefixes.size()));
                    prefixes.push_back(prefix);
                }
            }
        }
        const size_t node_count = prefixes.size();
        const size_t total_nodes = context.size() + node_count;
        std::vector<int64_t> nodes(node_count), positions(node_count);
        std::vector<uint8_t> visible(node_count * total_nodes, 0);
        for (size_t index = 0; index < node_count; ++index) {
            const auto& prefix = prefixes[index];
            nodes[index] = prefix.back();
            positions[index] = n_context + static_cast<int64_t>(prefix.size()) - 1;
            std::fill_n(visible.begin() + static_cast<std::ptrdiff_t>(index * total_nodes), context.size(), uint8_t{1});
            for (size_t other = 0; other < node_count; ++other) {
                const auto& ancestor = prefixes[other];
                if (ancestor.size() <= prefix.size() && std::equal(ancestor.begin(), ancestor.end(), prefix.begin())) {
                    visible[index * total_nodes + context.size() + other] = 1;
                }
            }
        }
        std::vector<int64_t> padded(candidates.size() * width, 0);
        std::vector<int64_t> steps(candidates.size() * width, 0), lengths;
        lengths.reserve(candidates.size());
        for (size_t index = 0; index < candidates.size(); ++index) {
            const auto& candidate = candidates[index];
            lengths.push_back(static_cast<int64_t>(candidate.size()));
            std::vector<int64_t> prefix;
            for (size_t token = 0; token < candidate.size(); ++token) {
                padded[index * width + token] = candidate[token];
                steps[index * width + token] = token == 0 ? n_context - 1 : prefix_indices.at(prefix);
                prefix.push_back(candidate[token]);
            }
        }
        Values inputs(api_), outputs(api_);
        for (OrtValue* cached : context_cache_) inputs.Borrow(cached);
        inputs.Add(Tensor(nodes, {static_cast<int64_t>(node_count)}));
        inputs.Add(Tensor(positions, {static_cast<int64_t>(node_count)}));
        inputs.Add(Tensor(visible, {static_cast<int64_t>(node_count), static_cast<int64_t>(total_nodes)}));
        inputs.Add(Tensor(padded, {static_cast<int64_t>(candidates.size()), static_cast<int64_t>(width)}));
        inputs.Add(Tensor(steps, {static_cast<int64_t>(candidates.size()), static_cast<int64_t>(width)}));
        inputs.Add(Tensor(lengths, {static_cast<int64_t>(candidates.size())}));
        Run(inputs, outputs);
        ValidateTensor(outputs.values[0], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT,
                       {static_cast<int64_t>(candidates.size())});
        float* data = nullptr;
        Check(api_->GetTensorMutableData(outputs.values[0], reinterpret_cast<void**>(&data)));
        std::vector<float> result(data, data + candidates.size());
        if (!std::all_of(result.begin(), result.end(), [](float value) { return std::isfinite(value); })) {
            throw std::runtime_error("MSIME returned non-finite candidate scores");
        }
        return result;
    }

    const std::string& RuntimeVersion() const { return runtime_version_; }

  private:
    struct Values {
        explicit Values(const OrtApi* value_api) : api(value_api) {}
        ~Values() {
            for (size_t index = borrowed; index < values.size(); ++index) {
                if (values[index]) api->ReleaseValue(values[index]);
            }
        }
        Values(const Values&) = delete;
        Values& operator=(const Values&) = delete;
        void Add(OrtValue* value) { values.push_back(value); }
        void Borrow(OrtValue* value) { values.push_back(value); ++borrowed; }
        const OrtApi* api;
        size_t borrowed = 0;
        std::vector<OrtValue*> values;
    };

    void Check(OrtStatus* status) const {
        if (!status) return;
        std::string message = api_->GetErrorMessage(status);
        api_->ReleaseStatus(status);
        throw std::runtime_error("MSIME ONNX Runtime: " + message);
    }

    void ValidateSessionNames() const {
        size_t count = 0;
        Check(api_->SessionGetInputCount(session_, &count));
        if (count != kInputs.size()) throw std::runtime_error("MSIME model input count mismatch");
        Check(api_->SessionGetOutputCount(session_, &count));
        if (count != kOutputs.size()) throw std::runtime_error("MSIME model output count mismatch");
        OrtAllocator* allocator = nullptr;
        Check(api_->GetAllocatorWithDefaultOptions(&allocator));
        for (size_t index = 0; index < kInputs.size() + kOutputs.size(); ++index) {
            char* name = nullptr;
            const bool input = index < kInputs.size();
            Check(input ? api_->SessionGetInputName(session_, index, allocator, &name)
                        : api_->SessionGetOutputName(session_, index - kInputs.size(), allocator, &name));
            const bool matches = name && std::string(name) ==
                (input ? kInputs[index] : kOutputs[index - kInputs.size()]);
            allocator->Free(allocator, name);
            if (!matches) throw std::runtime_error("MSIME model tensor names mismatch");
        }
    }

    OrtValue* TensorData(void* data, size_t bytes, const std::vector<int64_t>& shape,
                         ONNXTensorElementDataType type) const {
        OrtValue* value = nullptr;
        // ORT still expects a non-null address for a zero-element tensor.
        static uint64_t empty = 0;
        Check(api_->CreateTensorWithDataAsOrtValue(memory_, data ? data : &empty, bytes,
                                                   shape.data(), shape.size(), type, &value));
        return value;
    }
    OrtValue* Tensor(std::vector<int64_t>& data, std::vector<int64_t> shape) const {
        return TensorData(data.data(), data.size() * sizeof(int64_t), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    }
    OrtValue* Tensor(std::vector<uint8_t>& data, std::vector<int64_t> shape) const {
        return TensorData(data.data(), data.size(), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_BOOL);
    }
    OrtValue* Tensor(std::vector<float>& data, std::vector<int64_t> shape) const {
        return TensorData(data.data(), data.size() * sizeof(float), shape, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    }

    void ValidateTensor(OrtValue* value, ONNXTensorElementDataType type,
                        const std::vector<int64_t>& shape) const {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        Check(api_->GetTensorTypeAndShape(value, &info));
        try {
            size_t rank = 0;
            ONNXTensorElementDataType actual_type;
            Check(api_->GetDimensionsCount(info, &rank));
            Check(api_->GetTensorElementType(info, &actual_type));
            std::vector<int64_t> dimensions(rank);
            Check(api_->GetDimensions(info, dimensions.data(), rank));
            if (actual_type != type || dimensions != shape) throw std::runtime_error("MSIME output tensor shape mismatch");
        } catch (...) {
            api_->ReleaseTensorTypeAndShapeInfo(info);
            throw;
        }
        api_->ReleaseTensorTypeAndShapeInfo(info);
    }

    void Run(const Values& inputs, Values& outputs) const {
        std::array<const OrtValue*, 10> values{};
        if (inputs.values.size() != values.size()) throw std::logic_error("MSIME graph input count mismatch");
        std::copy(inputs.values.begin(), inputs.values.end(), values.begin());
        outputs.values.resize(kOutputs.size(), nullptr);
        Check(api_->Run(session_, nullptr, kInputs.data(), values.data(), values.size(),
                         kOutputs.data(), kOutputs.size(), outputs.values.data()));
    }

    void Prefill(const std::vector<int32_t>& context) {
        const int64_t length = static_cast<int64_t>(context.size());
        std::vector<int64_t> nodes(context.begin(), context.end()), positions(context.size());
        std::vector<int64_t> candidate{context.front()}, steps{0}, lengths{1};
        std::vector<uint8_t> visible(context.size() * context.size(), 0);
        for (size_t index = 0; index < context.size(); ++index) {
            positions[index] = static_cast<int64_t>(index);
            for (size_t prior = 0; prior <= index; ++prior) visible[index * context.size() + prior] = 1;
        }
        std::vector<float> empty;
        Values inputs(api_), outputs(api_);
        inputs.Add(Tensor(empty, {kLayers, kHeads, 0, kHeadDim}));
        inputs.Add(Tensor(empty, {kLayers, kHeads, 0, kHeadDim}));
        inputs.Add(Tensor(empty, {0, kWidth}));
        inputs.Add(Tensor(empty, {0}));
        inputs.Add(Tensor(nodes, {length}));
        inputs.Add(Tensor(positions, {length}));
        inputs.Add(Tensor(visible, {length, length}));
        inputs.Add(Tensor(candidate, {1, 1}));
        inputs.Add(Tensor(steps, {1, 1}));
        inputs.Add(Tensor(lengths, {1}));
        Run(inputs, outputs);
        ValidateTensor(outputs.values[1], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {kLayers, kHeads, length, kHeadDim});
        ValidateTensor(outputs.values[2], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {kLayers, kHeads, length, kHeadDim});
        ValidateTensor(outputs.values[3], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {length, kWidth});
        ValidateTensor(outputs.values[4], ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, {length});
        ClearContext();
        for (size_t index = 0; index < context_cache_.size(); ++index) {
            context_cache_[index] = outputs.values[index + 1];
            outputs.values[index + 1] = nullptr;
        }
        context_ids_ = context;
    }

    void ClearContext() {
        for (OrtValue*& value : context_cache_) {
            if (value) api_->ReleaseValue(value);
            value = nullptr;
        }
        context_ids_.clear();
    }
    void Release() {
        if (api_) {
            ClearContext();
            if (memory_) api_->ReleaseMemoryInfo(memory_);
            if (session_) api_->ReleaseSession(session_);
            if (options_) api_->ReleaseSessionOptions(options_);
            if (environment_) api_->ReleaseEnv(environment_);
        }
        memory_ = nullptr;
        session_ = nullptr;
        options_ = nullptr;
        environment_ = nullptr;
        api_ = nullptr;
        if (library_) FreeLibrary(library_);
        library_ = nullptr;
    }

    HMODULE library_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* environment_ = nullptr;
    OrtSessionOptions* options_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    std::array<OrtValue*, 4> context_cache_{};
    std::vector<int32_t> context_ids_;
    std::string runtime_version_;
};

Scorer::Scorer(const std::filesystem::path& model_path, const std::filesystem::path& runtime_library)
    : impl_(std::make_unique<Impl>(model_path, runtime_library)) {}
Scorer::~Scorer() = default;
std::vector<float> Scorer::Score(const std::vector<int32_t>& context,
                                const std::vector<std::vector<int32_t>>& candidates) {
    return impl_->Score(context, candidates);
}
const std::string& Scorer::RuntimeVersion() const { return impl_->RuntimeVersion(); }

}  // namespace xmjd::msime
