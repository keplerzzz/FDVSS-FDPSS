#pragma once

#include "fdvss/reference.hpp"

#include <chrono>

namespace fdvss::reference {

class Measurement {
  public:
    Measurement(Result& result, const Config& config)
        : result_(result), config_(config), start_(Clock::now()), previous_(start_) {}

    void stage(const std::string& name) {
        const auto now = Clock::now();
        StageSample sample;
        sample.protocol = result_.protocol;
        sample.mode = "connected_honest";
        sample.stage = name;
        sample.n = config_.n;
        sample.t = config_.t;
        sample.threads = 1;
        sample.milliseconds = std::chrono::duration<double, std::milli>(now - previous_).count();
        sample.communication.p2p_bytes = result_.communication.p2p_bytes - previous_bytes_.p2p_bytes;
        sample.communication.broadcast_native_bytes = result_.communication.broadcast_native_bytes - previous_bytes_.broadcast_native_bytes;
        sample.communication.broadcast_materialized_bytes = result_.communication.broadcast_materialized_bytes - previous_bytes_.broadcast_materialized_bytes;
        result_.stages.push_back(sample);
        previous_ = now;
        previous_bytes_ = result_.communication;
        result_.total_milliseconds = std::chrono::duration<double, std::milli>(now - start_).count();
    }

  private:
    using Clock = std::chrono::steady_clock;
    Result& result_;
    const Config& config_;
    Clock::time_point start_;
    Clock::time_point previous_;
    Communication previous_bytes_;
};

}  // namespace fdvss::reference
