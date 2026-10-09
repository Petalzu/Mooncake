#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <thread>
#include <vector>

struct RouteSnapshot {
    std::string peer;
    std::uint64_t generation;
    std::uint64_t expires;
};

class SnapshotCache {
   public:
    explicit SnapshotCache(std::size_t capacity) : capacity_(capacity) {
        assert(capacity > 0);
        table_ = std::make_shared<const Table>();
    }

    void publish(std::shared_ptr<const RouteSnapshot> route) {
        auto current = std::atomic_load(&table_);
        for (;;) {
            for (const auto& entry : *current)
                if (entry->peer == route->peer &&
                    entry->generation > route->generation)
                    return;
            auto next = std::make_shared<Table>(*current);
            next->erase(std::remove_if(next->begin(), next->end(),
                                       [&](const auto& entry) {
                                           return entry->peer == route->peer;
                                       }),
                        next->end());
            if (next->size() == capacity_) next->erase(next->begin());
            next->push_back(route);
            std::shared_ptr<const Table> immutable = next;
            if (std::atomic_compare_exchange_weak(&table_, &current, immutable))
                return;
        }
    }

    void invalidate(const std::string& peer) {
        auto current = std::atomic_load(&table_);
        for (;;) {
            auto next = std::make_shared<Table>(*current);
            next->erase(std::remove_if(next->begin(), next->end(),
                                       [&](const auto& entry) {
                                           return entry->peer == peer;
                                       }),
                        next->end());
            std::shared_ptr<const Table> immutable = next;
            if (std::atomic_compare_exchange_weak(&table_, &current, immutable))
                return;
        }
    }

    std::shared_ptr<const RouteSnapshot> tryPin(const std::string& peer,
                                                std::uint64_t generation,
                                                std::uint64_t now) const {
        const auto table = std::atomic_load(&table_);
        for (const auto& route : *table) {
            if (route->peer == peer && route->generation == generation &&
                now < route->expires)
                return route;
        }
        return {};
    }

   private:
    using Table = std::vector<std::shared_ptr<const RouteSnapshot>>;
    std::size_t capacity_;
    std::shared_ptr<const Table> table_;
};

struct Sample {
    double bytes;
    double elapsed;
    std::uint64_t timestamp;
};

struct AffineEstimate {
    double intercept = 0;
    double slope = 0;
    double min_bytes = 0;
    double max_bytes = 0;
    double mean_elapsed = 0;
    double max_residual = 0;
    bool valid = false;

    double predict(double bytes) const {
        if (bytes == 0) return 0;
        if (!valid || bytes < min_bytes || bytes > max_bytes)
            return std::numeric_limits<double>::infinity();
        return intercept + slope * bytes + max_residual;
    }
};

AffineEstimate fit(const std::vector<Sample>& samples, std::uint64_t now,
                   std::uint64_t max_age) {
    AffineEstimate estimate;
    if (samples.size() < 8) return estimate;
    double mean_bytes = 0;
    double mean_elapsed = 0;
    estimate.min_bytes = std::numeric_limits<double>::infinity();
    for (const auto& sample : samples) {
        if (!std::isfinite(sample.bytes) || !std::isfinite(sample.elapsed) ||
            sample.bytes <= 0 || sample.elapsed <= 0 ||
            sample.timestamp > now || now - sample.timestamp > max_age)
            return estimate;
        mean_bytes += sample.bytes;
        mean_elapsed += sample.elapsed;
        estimate.min_bytes = std::min(estimate.min_bytes, sample.bytes);
        estimate.max_bytes = std::max(estimate.max_bytes, sample.bytes);
    }
    mean_bytes /= samples.size();
    mean_elapsed /= samples.size();
    estimate.mean_elapsed = mean_elapsed;
    double variance = 0;
    double covariance = 0;
    for (const auto& sample : samples) {
        variance += (sample.bytes - mean_bytes) * (sample.bytes - mean_bytes);
        covariance +=
            (sample.bytes - mean_bytes) * (sample.elapsed - mean_elapsed);
    }
    if (variance <= 1e-12) return estimate;
    estimate.slope = covariance / variance;
    estimate.intercept = mean_elapsed - estimate.slope * mean_bytes;
    if (estimate.slope <= 0 || estimate.intercept < -1e-9) return estimate;
    estimate.intercept = std::max(0.0, estimate.intercept);
    for (const auto& sample : samples)
        estimate.max_residual =
            std::max(estimate.max_residual,
                     std::abs(sample.elapsed - estimate.intercept -
                              estimate.slope * sample.bytes));
    if (estimate.max_residual > 0.25 * mean_elapsed) return estimate;
    estimate.valid = true;
    return estimate;
}

struct Plan {
    std::vector<int> owners;
    double predicted = std::numeric_limits<double>::infinity();
    bool adapted = false;
};

double score(const std::vector<std::uint64_t>& slices,
             const std::vector<int>& owners, const AffineEstimate& first,
             const AffineEstimate& second) {
    assert(slices.size() == owners.size());
    double first_bytes = 0;
    double second_bytes = 0;
    for (std::size_t index = 0; index < slices.size(); ++index) {
        assert(owners[index] == 0 || owners[index] == 1);
        (owners[index] == 0 ? first_bytes : second_bytes) += slices[index];
    }
    return std::max(first.predict(first_bytes), second.predict(second_bytes));
}

Plan choose(const std::vector<std::uint64_t>& slices,
            const std::vector<int>& baseline, const AffineEstimate& first,
            const AffineEstimate& second, bool independent_topology,
            double margin) {
    assert(!slices.empty() && slices.size() == baseline.size());
    assert(margin >= 0 && margin < 1);
    Plan result{baseline, score(slices, baseline, first, second), false};
    if (!independent_topology || !std::isfinite(result.predicted))
        return result;
    for (std::size_t index = 0; index + 1 < slices.size(); ++index)
        if (slices[index] != slices.front()) return result;
    const double baseline_score = result.predicted;
    for (std::size_t first_count = 0; first_count < slices.size();
         ++first_count) {
        for (int tail_owner : {0, 1}) {
            std::vector<int> owners(slices.size(), 1);
            std::fill_n(owners.begin(), first_count, 0);
            owners.back() = tail_owner;
            const double candidate = score(slices, owners, first, second);
            if (candidate < result.predicted - 1e-12) {
                result.owners = owners;
                result.predicted = candidate;
            }
        }
    }
    if (result.predicted < (1 - margin) * baseline_score - 1e-12) {
        result.adapted = result.owners != baseline;
    } else {
        result.owners = baseline;
        result.predicted = baseline_score;
    }
    return result;
}

std::vector<Sample> samples(double intercept, double slope, std::uint64_t now) {
    std::vector<Sample> result;
    for (double bytes : {1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0, 80.0})
        result.push_back({bytes, intercept + slope * bytes, now});
    return result;
}

class FeedbackWindow {
   public:
    bool observe(const Sample& sample) {
        const auto estimate = fit(window_, sample.timestamp, 50);
        const double prediction = estimate.predict(sample.bytes);
        const bool changed =
            std::isfinite(prediction) && std::abs(sample.elapsed - prediction) >
                                             std::max(1.0, 0.25 * prediction);
        if (changed) {
            window_.clear();
            ++invalidations_;
        }
        window_.push_back(sample);
        if (window_.size() > 32) window_.erase(window_.begin());
        return changed;
    }

    AffineEstimate estimate(std::uint64_t now) const {
        return fit(window_, now, 50);
    }
    std::size_t size() const { return window_.size(); }
    std::uint64_t invalidations() const { return invalidations_; }

   private:
    std::vector<Sample> window_;
    std::uint64_t invalidations_ = 0;
};

double actualTime(const std::vector<std::uint64_t>& slices,
                  const std::vector<int>& owners,
                  const std::array<double, 2>& intercepts,
                  const std::array<double, 2>& slopes) {
    std::array<double, 2> bytes{0, 0};
    for (std::size_t index = 0; index < slices.size(); ++index)
        bytes[owners[index]] += slices[index];
    double completion = 0;
    for (std::size_t path = 0; path < bytes.size(); ++path)
        if (bytes[path] > 0)
            completion = std::max(
                completion, intercepts[path] + slopes[path] * bytes[path]);
    return completion;
}

std::uint64_t replayChecks() {
    std::uint64_t checks = 0;
    for (std::uint32_t seed : {17u, 29u, 53u, 97u, 193u}) {
        std::array<FeedbackWindow, 2> windows;
        for (const auto& sample : samples(0, 1, 100))
            windows[0].observe(sample);
        for (const auto& sample : samples(32, 1, 100))
            windows[1].observe(sample);
        auto random = seed;
        double candidate_total = 0;
        double baseline_total = 0;
        for (std::uint64_t request = 0; request < 200; ++request) {
            const std::uint64_t now = 101 + request;
            std::vector<std::uint64_t> slices(64, 1);
            std::vector<int> baseline(64, 0);
            const auto plan = choose(slices, baseline, windows[0].estimate(now),
                                     windows[1].estimate(now), true, 0.05);
            const std::array<double, 2> intercepts{0, 32};
            const std::array<double, 2> slopes{1, 1};
            const double actual =
                actualTime(slices, plan.owners, intercepts, slopes);
            candidate_total += actual;
            baseline_total += actualTime(slices, baseline, intercepts, slopes);
            assert(actual <= 64);
            std::array<double, 2> bytes{0, 0};
            for (int owner : plan.owners) ++bytes[owner];
            for (std::size_t path = 0; path < bytes.size(); ++path) {
                if (bytes[path] == 0) continue;
                random ^= random << 13;
                random ^= random >> 17;
                random ^= random << 5;
                const double noise =
                    (static_cast<double>(random % 2001) - 1000) / 100000.0;
                const double elapsed =
                    (intercepts[path] + slopes[path] * bytes[path]) *
                    (1 + noise);
                windows[path].observe({bytes[path], elapsed, now});
                assert(windows[path].size() <= 32);
            }
            ++checks;
        }
        assert(candidate_total <= baseline_total);
    }

    std::array<FeedbackWindow, 2> windows;
    for (const auto& sample : samples(0, 0.2, 100)) windows[0].observe(sample);
    for (const auto& sample : samples(0, 1, 100)) windows[1].observe(sample);
    const std::vector<std::uint64_t> slices(64, 1);
    std::vector<int> baseline(64, 1);
    std::fill_n(baseline.begin(), 11, 0);
    const auto stale = choose(slices, baseline, windows[0].estimate(101),
                              windows[1].estimate(101), true, 0.05);
    const std::array<double, 2> inverted_slopes{1, 0.2};
    const std::array<double, 2> no_startup{0, 0};
    const double stale_time =
        actualTime(slices, stale.owners, no_startup, inverted_slopes);
    const double original_time =
        actualTime(slices, baseline, no_startup, inverted_slopes);
    assert(stale.adapted && stale_time > original_time);
    const double first_bytes =
        std::count(stale.owners.begin(), stale.owners.end(), 0);
    assert(windows[0].observe({first_bytes, first_bytes, 101}));
    const double second_bytes = 64 - first_bytes;
    assert(windows[1].observe({second_bytes, 0.2 * second_bytes, 101}));
    const auto next = choose(slices, baseline, windows[0].estimate(102),
                             windows[1].estimate(102), true, 0.05);
    assert(!next.adapted && next.owners == baseline);
    assert(windows[0].invalidations() == 1 && windows[1].invalidations() == 1);
    const std::vector<double> exploration{1, 2, 4, 8, 16, 32, 64, 80};
    for (std::size_t index = 0; index < exploration.size(); ++index) {
        const double bytes = exploration[index];
        windows[0].observe({bytes, bytes, 103 + index});
        windows[1].observe({bytes, 0.2 * bytes, 103 + index});
    }
    const auto recovered =
        choose(slices, std::vector<int>(64, 0), windows[0].estimate(111),
               windows[1].estimate(111), true, 0.05);
    assert(recovered.adapted);
    assert(actualTime(slices, recovered.owners, no_startup, inverted_slopes) <=
           11);
    assert(!windows[0].estimate(200).valid && !windows[1].estimate(200).valid);
    checks += 4;
    return checks;
}

int main() {
    SnapshotCache cache(2);
    std::uint64_t cache_checks = 0;
    auto check_cache = [&](bool condition) {
        assert(condition);
        ++cache_checks;
    };
    check_cache(!cache.tryPin("peer", 1, 1));
    std::shared_ptr<const RouteSnapshot> pinned;
    std::thread publisher([&] {
        cache.publish(
            std::make_shared<RouteSnapshot>(RouteSnapshot{"peer", 1, 100}));
    });
    publisher.join();
    std::thread reader([&] { pinned = cache.tryPin("peer", 1, 1); });
    reader.join();
    check_cache(pinned && pinned->generation == 1);
    check_cache(!cache.tryPin("peer", 2, 1));
    check_cache(!cache.tryPin("peer", 1, 100));
    cache.publish(
        std::make_shared<RouteSnapshot>(RouteSnapshot{"peer", 2, 200}));
    check_cache(pinned->generation == 1);
    check_cache(!cache.tryPin("peer", 1, 1));
    cache.publish(
        std::make_shared<RouteSnapshot>(RouteSnapshot{"peer", 1, 200}));
    check_cache(!!cache.tryPin("peer", 2, 1));
    cache.invalidate("peer");
    check_cache(!cache.tryPin("peer", 2, 1));
    check_cache(pinned->generation == 1);
    std::thread first_publisher([&] {
        cache.publish(
            std::make_shared<RouteSnapshot>(RouteSnapshot{"first", 1, 200}));
    });
    std::thread second_publisher([&] {
        cache.publish(
            std::make_shared<RouteSnapshot>(RouteSnapshot{"second", 1, 200}));
    });
    first_publisher.join();
    second_publisher.join();
    check_cache(cache.tryPin("first", 1, 1) && cache.tryPin("second", 1, 1));
    cache.publish(
        std::make_shared<RouteSnapshot>(RouteSnapshot{"third", 1, 200}));
    const int remaining =
        !!cache.tryPin("first", 1, 1) + !!cache.tryPin("second", 1, 1);
    check_cache(remaining == 1 && cache.tryPin("third", 1, 1));

    std::uint64_t oracle_checks = 0;
    for (double first_intercept : {0.0, 3.0, 8.0})
        for (double second_intercept : {0.0, 3.0, 8.0})
            for (double first_slope : {0.5, 1.0, 2.0})
                for (double second_slope : {0.5, 1.0, 2.0}) {
                    const auto first = fit(
                        samples(first_intercept, first_slope, 100), 100, 50);
                    const auto second = fit(
                        samples(second_intercept, second_slope, 100), 100, 50);
                    assert(first.valid && second.valid);
                    assert(std::abs(first.intercept - first_intercept) < 1e-9);
                    assert(std::abs(first.slope - first_slope) < 1e-9);
                    for (std::size_t count : {1u, 2u, 3u, 7u, 10u})
                        for (std::uint64_t tail : {1u, 3u, 4u, 5u}) {
                            std::vector<std::uint64_t> slices(count, 4);
                            slices.back() = tail;
                            std::vector<int> baseline(count, 0);
                            for (std::size_t index = 0; index < count; ++index)
                                baseline[index] = index % 2;
                            const auto plan = choose(slices, baseline, first,
                                                     second, true, 0);
                            double optimum =
                                std::numeric_limits<double>::infinity();
                            for (std::uint64_t mask = 0; mask < (1ULL << count);
                                 ++mask) {
                                std::vector<int> owners(count);
                                for (std::size_t index = 0; index < count;
                                     ++index)
                                    owners[index] = (mask >> index) & 1;
                                optimum = std::min(
                                    optimum,
                                    score(slices, owners, first, second));
                            }
                            assert(std::abs(plan.predicted - optimum) < 1e-9);
                            ++oracle_checks;
                        }
                }

    const auto good = fit(samples(0, 1, 100), 100, 50);
    const std::vector<std::uint64_t> slices(64, 1);
    const std::vector<int> original(64, 0);
    std::uint64_t fallback_checks = 0;
    auto check_fallback = [&](const AffineEstimate& estimate,
                              bool independent) {
        const auto plan =
            choose(slices, original, estimate, good, independent, 0.05);
        assert(plan.owners == original && !plan.adapted);
        ++fallback_checks;
    };
    check_fallback(fit({}, 100, 50), true);
    check_fallback(fit(samples(0, 1, 100), 151, 50), true);
    check_fallback(fit(samples(0, 1, 100), 99, 50), true);
    check_fallback(fit(std::vector<Sample>(8, Sample{4, 4, 100}), 100, 50),
                   true);
    check_fallback(fit(samples(100, -1, 100), 100, 50), true);
    check_fallback(fit(samples(-0.25, 1, 100), 100, 50), true);
    auto nonfinite = samples(0, 1, 100);
    nonfinite[0].elapsed = std::numeric_limits<double>::quiet_NaN();
    check_fallback(fit(nonfinite, 100, 50), true);
    auto nonpositive = samples(0, 1, 100);
    nonpositive[0].elapsed = 0;
    check_fallback(fit(nonpositive, 100, 50), true);
    auto noisy = samples(0, 1, 100);
    noisy[4].elapsed = 1000;
    check_fallback(fit(noisy, 100, 50), true);
    check_fallback(good, false);
    auto unsupported = good;
    unsupported.max_bytes = 32;
    check_fallback(unsupported, true);
    const auto queued = fit(samples(32, 1, 100), 100, 50);
    auto adapted = choose(slices, original, good, queued, true, 0.05);
    assert(std::count(adapted.owners.begin(), adapted.owners.end(), 0) == 48);
    assert(std::abs(adapted.predicted - 48) < 1e-9 && adapted.adapted);
    auto symmetric =
        choose(slices, std::vector<int>(64, 0), good, good, true, 0.6);
    assert(!symmetric.adapted && symmetric.owners == original);
    ++fallback_checks;
    const auto replay_checks = replayChecks();

    std::cout << "{\n"
              << "  \"scope\": "
                 "\"standalone_mechanism_and_synthetic_feedback_not_TENT_"
                 "integration\",\n"
              << "  \"rdma_hardware_used\": false,\n"
              << "  \"cache_invariant_checks\": " << cache_checks << ",\n"
              << "  \"affine_plan_oracle_checks\": " << oracle_checks << ",\n"
              << "  \"fallback_checks\": " << fallback_checks << ",\n"
              << "  \"synthetic_replay_checks\": " << replay_checks << ",\n"
              << "  \"replay_seeds\": [17, 29, 53, 97, 193],\n"
              << "  \"bandwidth_reversal_first_request_can_regress\": true,\n"
              << "  \"recovery_after_eight_varied_probe_observations\": true,\n"
              << "  \"queued_allocation\": [48, 16],\n"
              << "  \"queued_predicted_time\": 48\n"
              << "}\n";
}
