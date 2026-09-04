#pragma once

#include "common.hpp"
#include "threadpool.hpp"
#include <memory>
#include <string>
#include <vector>
#include <functional>

namespace edgemon {

class EmbeddingEngine : public std::enable_shared_from_this<EmbeddingEngine> {
public:
    using EmbeddingCallback = std::function<void(const std::string&, const std::vector<float>&)>;

    explicit EmbeddingEngine(std::shared_ptr<ThreadPool> pool);
    ~EmbeddingEngine();

    bool initialize(const std::string& modelPath = "");
    bool isInitialized() const { return initialized_; }

    std::vector<float> embed(const std::string& text);
    void embedAsync(const std::string& text, const std::string& id, EmbeddingCallback callback);

    void embedBatch(const std::vector<std::string>& texts, std::vector<std::vector<float>>& results);

    size_t getDimension() const { return dimension_; }
    std::string getModelName() const { return modelName_; }

    void setBatchSize(size_t size) { batchSize_ = size; }
    size_t getBatchSize() const { return batchSize_; }

private:
    std::vector<float> preprocessText(const std::string& text);

    std::shared_ptr<ThreadPool> pool_;
    std::atomic<bool> initialized_;
    
    size_t dimension_;
    size_t batchSize_;
    std::string modelName_;
    
    std::mutex inferenceMutex_;
};

}
