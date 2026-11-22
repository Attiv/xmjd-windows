#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xmjd::msime {

// The caller serializes requests. Only committed-context tensors survive Score.
class Scorer final {
  public:
    Scorer(const std::filesystem::path& model_path, const std::filesystem::path& runtime_library);
    ~Scorer();
    Scorer(const Scorer&) = delete;
    Scorer& operator=(const Scorer&) = delete;

    std::vector<float> Score(const std::vector<int32_t>& context,
                             const std::vector<std::vector<int32_t>>& candidates);
    const std::string& RuntimeVersion() const;

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace xmjd::msime
