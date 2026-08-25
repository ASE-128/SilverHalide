#pragma once

#include <string>
#include <vector>
#include <memory>
#include <map>

// Forward declaration of ONNX Runtime types
namespace Ort = Ort_v1; // Standard alias for ONNX Runtime (depends on include path)

namespace lrc {

/**
 * @brief A singleton or shared context for ONNX Runtime.
 * Manages the environment and global settings.
 */
class InferenceContext {
public:
    InferenceContext();
    ~InferenceContext();

    // Prevent copying
    InferenceContext(const InferenceContext&) = delete;
    InferenceContext& operator=(const InferenceContext&) = delete;

    // Returns the raw env handle if needed by specific drivers
    Ort::Env* get_env() const { return m_env.get(); }

private:
    std::unique_ptr<Ort::Env> m_env;
};

} // namespace lrc
