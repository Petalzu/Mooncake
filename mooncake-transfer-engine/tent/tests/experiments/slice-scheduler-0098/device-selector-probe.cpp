#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>

#include "slice-plan.generated.h"
#include "tent/thirdparty/nlohmann/json.h"
#include "tent/transport/rdma/quota.h"

using mooncake::tent::DeviceSelector;
using mooncake::tent::NicLoadStats;
using mooncake::tent::planRdmaSlices;
using mooncake::tent::Topology;
using nlohmann::json;

constexpr uint64_t kBlock = 65536;

std::shared_ptr<Topology> twoNicTopology() {
    auto topology = std::make_shared<Topology>();
    for (int device = 0; device < 2; ++device) {
        Topology::NicEntry nic;
        nic.name = "probe_nic_" + std::to_string(device);
        nic.type = Topology::NIC_RDMA;
        nic.numa_node = 0;
        topology->nic_list_.push_back(nic);
    }
    Topology::MemEntry memory;
    memory.name = "cpu:0";
    memory.type = Topology::MEM_HOST;
    memory.numa_node = 0;
    memory.device_list[0] = {0, 1};
    topology->mem_list_.push_back(memory);
    return topology;
}

void configure(DeviceSelector& selector) {
    auto topology = twoNicTopology();
    assert(selector.loadTopology(topology).ok());
    DeviceSelector::SchedulingParams params;
    params.score_jitter_range = 0.0;
    params.enable_priority_filtering = false;
    selector.setSchedulingParams(params);
    assert(selector.setDeviceBandwidth(0, 80).ok());
    assert(selector.setDeviceBandwidth(1, 80).ok());
}

json observeAllocation(const char* name, double firstGbps, double secondGbps,
                       uint64_t secondQueue) {
    DeviceSelector selector;
    configure(selector);
    assert(selector.setDeviceBandwidth(0, firstGbps).ok());
    assert(selector.setDeviceBandwidth(1, secondGbps).ok());
    assert(selector.chargeDevice(1, secondQueue).ok());
    std::vector<int> devices;
    assert(selector.allocate(64 * kBlock, 64, kBlock, "cpu:0", devices).ok());
    std::array<int, 2> counts{0, 0};
    for (int device : devices) {
        ++counts[device];
        assert(selector.release(device, kBlock, 0.0).ok());
    }
    assert(selector.release(1, secondQueue, 0.0).ok());
    assert(selector.getInflightBytes(0) == 0);
    assert(selector.getInflightBytes(1) == 0);
    return json{{"name", name},
                {"allocation", counts},
                {"link_gbps", {firstGbps, secondGbps}},
                {"second_queue_bytes", secondQueue},
                {"balanced_after_release", true}};
}

struct PathEstimate {
    double rate;
    double queue;
    double startup;
};

double completion(const std::array<PathEstimate, 2>& paths,
                  const std::array<int, 2>& allocation) {
    double result = 0;
    for (size_t index = 0; index < paths.size(); ++index) {
        if (allocation[index] == 0) continue;
        result = std::max(result, paths[index].startup +
                                      (paths[index].queue + allocation[index]) /
                                          paths[index].rate);
    }
    return result;
}

std::array<int, 2> earliestFinish(const std::array<PathEstimate, 2>& paths,
                                  int sliceCount) {
    std::array<int, 2> allocation{0, 0};
    for (int slice = 0; slice < sliceCount; ++slice) {
        const double first =
            paths[0].startup +
            (paths[0].queue + allocation[0] + 1) / paths[0].rate;
        const double second =
            paths[1].startup +
            (paths[1].queue + allocation[1] + 1) / paths[1].rate;
        ++allocation[second < first ? 1 : 0];
    }
    return allocation;
}

int main() {
    json report;
    report["scope"] =
        "real upstream DeviceSelector, extracted slice planner, standalone "
        "policy prototype";
    report["rdma_hardware_used"] = false;
    report["upstream_allocations"] = json::array({
        observeAllocation("symmetric", 80, 80, 0),
        observeAllocation("bandwidth_2_to_1", 160, 80, 0),
        observeAllocation("one_path_queued", 80, 80, 32 * kBlock),
    });
    assert(report["upstream_allocations"][0]["allocation"] ==
           json::array({32, 32}));
    assert(report["upstream_allocations"][1]["allocation"] ==
           json::array({43, 21}));
    assert(report["upstream_allocations"][2]["allocation"] ==
           json::array({63, 1}));

    DeviceSelector selector;
    configure(selector);
    assert(selector.chargeDevice(0, kBlock).ok());
    assert(selector.release(0, kBlock, kBlock / 5e9).ok());
    std::vector<NicLoadStats> statistics;
    assert(selector.getNicLoadStats(statistics).ok());
    for (const auto& nic : statistics) {
        if (nic.device_name == "probe_nic_0") {
            assert(std::abs(nic.ewma_bandwidth_bps - 5.05e9) < 1);
            report["ewma_after_one_sample_bps"] = nic.ewma_bandwidth_bps;
        }
    }
    assert(report.contains("ewma_after_one_sample_bps"));

    uint64_t planChecks = 0;
    for (bool smart : {false, true}) {
        selector.setSmartSelection(smart);
        for (uint64_t cap : {32, 64}) {
            for (uint64_t blocks = 0; blocks <= 130; ++blocks) {
                for (uint64_t tail : {uint64_t{0}, uint64_t{1}, kBlock / 4 - 1,
                                      kBlock / 4, kBlock - 1}) {
                    const uint64_t length = blocks * kBlock + tail;
                    const auto plan = planRdmaSlices(length, kBlock, cap);
                    assert(plan.count >= 1 && plan.count <= cap);
                    assert(plan.block_size % kBlock == 0);
                    if (length > 0) {
                        std::vector<int> devices;
                        assert(selector
                                   .allocate(length, plan.count,
                                             plan.block_size, "cpu:0", devices)
                                   .ok());
                        assert(devices.size() == plan.count);
                        uint64_t offset = 0;
                        std::array<uint64_t, 2> charged{0, 0};
                        for (size_t index = 0; index < devices.size();
                             ++index) {
                            const uint64_t bytes = index + 1 == devices.size()
                                                       ? length - offset
                                                       : plan.block_size;
                            assert(bytes > 0);
                            offset += bytes;
                            charged[devices[index]] += bytes;
                        }
                        assert(offset == length);
                        for (int device = 0; device < 2; ++device) {
                            assert(selector.getInflightBytes(device) ==
                                   (smart ? charged[device] : 0));
                        }
                        offset = 0;
                        for (size_t index = 0; index < devices.size();
                             ++index) {
                            const uint64_t bytes = index + 1 == devices.size()
                                                       ? length - offset
                                                       : plan.block_size;
                            offset += bytes;
                            assert(selector.release(devices[index], bytes, 0.0)
                                       .ok());
                        }
                        assert(selector.getInflightBytes(0) == 0);
                        assert(selector.getInflightBytes(1) == 0);
                    }
                    ++planChecks;
                }
            }
        }
    }
    report["slice_plan_accounting_checks"] = planChecks;
    report["aggregation_thresholds"] = {
        {"host_read_one_byte_below_2_mib",
         planRdmaSlices(32 * kBlock - 1, kBlock, 64).count},
        {"host_write_one_byte_below_1_mib",
         planRdmaSlices(16 * kBlock - 1, kBlock, 32).count},
    };

    uint64_t oracleChecks = 0;
    for (double firstRate : {1, 2, 5})
        for (double secondRate : {1, 2, 5})
            for (double firstQueue : {0, 8, 32})
                for (double secondQueue : {0, 8, 32})
                    for (double startup : {0, 3})
                        for (int slices : {1, 2, 7, 16, 64}) {
                            const std::array<PathEstimate, 2> paths{
                                {{firstRate, firstQueue, startup},
                                 {secondRate, secondQueue, 0}}};
                            const auto allocation =
                                earliestFinish(paths, slices);
                            double optimum = completion(paths, {0, slices});
                            for (int first = 1; first <= slices; ++first)
                                optimum = std::min(
                                    optimum,
                                    completion(paths, {first, slices - first}));
                            assert(std::abs(completion(paths, allocation) -
                                            optimum) < 1e-9);
                            ++oracleChecks;
                        }
    report["cpp_policy_oracle_checks"] = oracleChecks;
    std::cout << report.dump(2) << std::endl;
}
