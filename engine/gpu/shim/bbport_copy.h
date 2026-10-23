#pragma once
#include <cstddef>
#include <functional>

namespace BbCopy {

void ParallelFor(std::size_t count, const std::function<void(std::size_t)>& task);

void SetGpuSideQuery(bool (*query)());

bool ForGpuSide();

bool Enabled();

void Async(std::function<void()> task);

struct Item {
    void (*run)(const Item&);
    void* context;
    unsigned long long source, destination, size, extra;
};

void QueueCopy(const Item& item);

void FlushBatch();

void WaitAsync();

bool HelpOne();

void AfterCopies(std::function<void()> callback);

}
