#include "TaskResultInserter.h"
#include "Serializer.hpp"


class TaskResultInserter: public td::actor::Actor {
private:
    sw::redis::Transaction transaction_;
    TraceEmulationResult result_;
    td::Promise<td::Unit> promise_;

public:
    TaskResultInserter(sw::redis::Transaction&& transaction, TraceEmulationResult result, td::Promise<td::Unit> promise) :
        transaction_(std::move(transaction)), result_(std::move(result)), promise_(std::move(promise)) {
    }

    void start_up() override {
        auto result_channel = "result_channel_" + result_.task_id;
        try {
            if (result_.trace.is_error()) {
                transaction_.set("error_channel_" + result_.task_id, result_.trace.error().message().str());
                transaction_.publish(result_channel, "error");
                transaction_.exec();
                promise_.set_value(td::Unit());
                stop();
                return;
            }

            std::queue<std::reference_wrapper<TraceNode>> queue;
        
            std::vector<std::string> tx_keys_to_delete;
            std::vector<std::pair<std::string, std::string>> addr_keys_to_delete;
            std::vector<RedisTraceNode> flattened_trace;

            queue.push(*result_.trace.ok().root);

            while (!queue.empty()) {
                TraceNode& current = queue.front();

                for (auto& child : current.children) {
                    queue.push(*child);
                }

                auto tx_r = parse_tx(current.transaction_root, current.address.workchain);
                if (tx_r.is_error()) {
                    promise_.set_error(tx_r.move_as_error_prefix("Failed to parse transaction: "));
                    stop();
                    return;
                }
                auto tx = tx_r.move_as_ok();

                flattened_trace.push_back(RedisTraceNode{std::move(tx), current.emulated});

                queue.pop();
            }


            // insert new trace
            for (const auto& node : flattened_trace) {
                std::stringstream buffer;
                msgpack::pack(buffer, std::move(node));

                transaction_.hset("result_hset_" + result_.task_id, node.transaction.in_msg.value().hash.to_hex(), buffer.str());
            }
            transaction_.hset("result_hset_" + result_.task_id, "root_node", result_.trace.ok().id.to_hex());
            transaction_.hset("result_hset_" + result_.task_id, "mc_block_id", result_.mc_block_id.to_str());

            // // insert interfaces
            // for (const auto& [addr, interfaces] : addr_interfaces) {
            //     auto interfaces_redis = parse_interfaces(interfaces);
            //     std::stringstream buffer;
            //     msgpack::pack(buffer, interfaces_redis);
            //     auto addr_raw = std::to_string(addr.workchain) + ":" + addr.addr.to_hex();
            //     transaction_.hset(trace_->id.to_hex(), addr_raw, buffer.str());
            // }

            transaction_.publish(result_channel, "success");
            transaction_.exec();

            promise_.set_value(td::Unit());
        } catch (const vm::VmError &e) {
            promise_.set_error(td::Status::Error("Got VmError while inserting trace: " + std::string(e.get_msg())));
        } catch (const std::exception &e) {
            promise_.set_error(td::Status::Error("Got exception while inserting trace: " + std::string(e.what())));
        }
        stop();
    }

};

void RedisTaskResultInsertManager::insert(TraceEmulationResult result, td::Promise<td::Unit> promise) {
    td::actor::create_actor<TaskResultInserter>("TraceInserter", redis_.transaction(), std::move(result), std::move(promise)).release();
}