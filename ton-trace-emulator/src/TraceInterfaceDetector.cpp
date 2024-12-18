#include "TraceInterfaceDetector.h"
#include "smc-interfaces/InterfacesDetector.h"

void TraceInterfaceDetector::start_up() {
    td::MultiPromise mp;
    auto ig = mp.init_guard();
    auto P = td::PromiseCreator::lambda([&, SelfId=actor_id(this)](td::Result<td::Unit> res) mutable {
        td::actor::send_closure(SelfId, &TraceInterfaceDetector::finish, std::move(res));
    });
    ig.add_promise(std::move(P));

    for (auto& [address, account] : trace_.emulated_accounts) {
        trace_.interfaces[address] = {};
        td::actor::create_actor<Trace::Detector>
            ("InterfacesDetector", address, account.code, account.data, shard_states_, config_, 
            td::PromiseCreator::lambda([SelfId = actor_id(this), address, promise = ig.get_promise()](std::vector<typename Trace::Detector::DetectedInterface> interfaces) mutable {
                td::actor::send_closure(SelfId, &TraceInterfaceDetector::got_interfaces, address, std::move(interfaces), std::move(promise));
        })).release();
    }
    
}

void TraceInterfaceDetector::got_interfaces(block::StdAddress address, std::vector<typename Trace::Detector::DetectedInterface> interfaces, td::Promise<td::Unit> promise) {
    trace_.interfaces[address] = std::move(interfaces);
    promise.set_value(td::Unit());
}

void TraceInterfaceDetector::finish(td::Result<td::Unit> status) {
    if (status.is_error()) {
        promise_.set_error(status.move_as_error_prefix("Failed to detect interfaces: "));
    } else {
        promise_.set_value(std::move(trace_));
    }
    stop();
}