#pragma once

#include "InferenceContext.h"
#include <string>
#include <vector>
#include <memory>

namespace lrc {

/**
 * @brief Represents the capabilities of a specific model.
 */
struct ModelCapability {
    bool is_denoise = false;
    bool is_segment = false;
    bool is_upscale = false;
    bool is_color_correction = false;
};

/**
 * @brief Logic for loading and preparing ONNX inference sessions.
 */
class ModelLoader {
public:
    /**
     * @brief Load a model into memory and prepare the session.
     * @param path The path to the .onnx file.
     * @return A shared_ptr to the session wrapper.
     */
    static std::shared_ptr<class InferenceSession> load_model(const std::string& path);

    /**
     * @brief Static helper to determine the best execution provider.
     * Defaults to CUDA if available, otherwise fallback to CPU.
     */
    static std::vector<const char*> get_default_providers();
};

/**
 * @brief Wrapper for an ONNX Runtime Session.
 * Encapsulates the session object and its specific options.
 */
class InferenceSession {
public:
    InferenceSession(const std::string& model_path, const std::vector<const char*>& providers);

    // Core methods for inference
    void run_inference(const std::vector<float>& input_data, std::vector<float>& output_data);

    // Metadata
    const ModelCapability& get_capabilities() const { return m_capabilities; }

private:
    InferenceSession(const std::string& model_path); // Internal constructor for friend use

    std::vector<const char*> m_providers;
    ModelCapability m_capabilities;
    // ... internal ONNX session members ...
};

} // namespace lrc
