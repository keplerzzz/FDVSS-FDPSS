#include "fdvss/payload.hpp"
#include "fdvss/reference.hpp"

#include <stdexcept>

namespace {

void require(bool condition) {
    if (!condition) throw std::runtime_error("serialized payload accounting mismatch");
}

bool equal(const fdvss::Communication& a, const fdvss::Communication& b) {
    return a.p2p_bytes == b.p2p_bytes
        && a.broadcast_native_bytes == b.broadcast_native_bytes
        && a.broadcast_materialized_bytes == b.broadcast_materialized_bytes;
}

fdvss::Communication sum(const std::vector<fdvss::Communication>& stages) {
    fdvss::Communication result;
    for (const auto& stage : stages) {
        result.p2p_bytes += stage.p2p_bytes;
        result.broadcast_native_bytes += stage.broadcast_native_bytes;
        result.broadcast_materialized_bytes += stage.broadcast_materialized_bytes;
    }
    return result;
}

void compare(const std::vector<fdvss::Communication>& counted,
             const fdvss::reference::Result& executed) {
    require(counted.size() == executed.stages.size());
    for (std::size_t k = 0; k < counted.size(); ++k)
        require(equal(counted[k], executed.stages[k].communication));
    require(equal(sum(counted), executed.communication));
}

}  // namespace

void payload_tests() {
    using namespace fdvss;
    for (const std::size_t n : {4, 7, 8}) {
        Config config;
        config.n = n;
        config.t = (n - 1) / 3;
        compare(honest_payloads("fdvss1", n, config.t), reference::run_fdvss1(config, "honest"));
        const auto sharing = honest_payloads("fdvss2", n, config.t);
        compare(sharing, reference::run_fdvss2(config, "honest"));
        const auto counted = honest_payloads("fdpss2", n, config.t);
        const auto executed = reference::run_fdpss(config, "honest");
        require(counted.size() == 6 && executed.stages.size() == 3);
        require(equal(sum({counted[0], counted[1], counted[2], counted[3]}),
                      executed.stages[0].communication));
        require(equal(counted[4], executed.stages[1].communication));
        require(equal(counted[5], executed.stages[2].communication));
        require(equal(sum(counted), executed.communication));
        for (std::size_t k = 0; k < sharing.size(); ++k)
            require(equal(counted[k], payload_detail::scale(sharing[k], n)));
    }
    for (const auto& protocol : {"fdvss1", "fdvss2", "fdpss2"}) {
        const auto stages = honest_payloads(protocol, 128, 42);
        const auto total = sum(stages);
        require(total.p2p_bytes != 0);
        require(total.broadcast_materialized_bytes == 128 * total.broadcast_native_bytes);
    }
    bool rejected = false;
    try { (void)honest_payloads("fdvss2", 6, 2); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected);
}
