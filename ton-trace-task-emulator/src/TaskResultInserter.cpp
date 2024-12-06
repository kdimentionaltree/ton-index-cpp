#include "TaskResultInserter.h"
#include "Serializer.hpp"

struct Result {
    std::string task_id;
    bool success;
    std::string error;
    std::unique_ptr<Trace> trace;
};

class TaskResultInserter: public td::actor::Actor {
private:
    sw::redis::Transaction transaction_;
    td::Result<std::unique_ptr<Trace>> result_;
    td::Promise<td::Unit> promise_;

    std::unique_ptr<Trace> trace_;

public:
    TaskResultInserter(sw::redis::Transaction&& transaction, td::Result<std::unique_ptr<Trace>> result, td::Promise<td::Unit> promise) :
        transaction_(std::move(transaction)), result_(std::move(result)), promise_(std::move(promise)) {
        trace_ = result_.move_as_ok();
    }

    void start_up() override {
        try {
            std::queue<std::reference_wrapper<Trace>> queue;
            std::unordered_map<block::StdAddress, typeof(trace_->interfaces), AddressHasher> addr_interfaces;
        
            std::vector<std::string> tx_keys_to_delete;
            std::vector<std::pair<std::string, std::string>> addr_keys_to_delete;
            std::vector<TraceNode> flattened_trace;

            queue.push(*trace_);

            while (!queue.empty()) {
                Trace& current = queue.front();

                for (auto& child : current.children) {
                    queue.push(*child);
                }

                auto tx_r = parse_tx(current.transaction_root, current.workchain);
                if (tx_r.is_error()) {
                    promise_.set_error(tx_r.move_as_error_prefix("Failed to parse transaction: "));
                    stop();
                    return;
                }
                auto tx = tx_r.move_as_ok();

                if (!current.emulated) {
                    delete_db_subtree(tx.in_msg.value().hash.to_hex(), tx_keys_to_delete, addr_keys_to_delete);
                }

                addr_interfaces[tx.account] = current.interfaces;

                flattened_trace.push_back(TraceNode{std::move(tx), current.emulated});

                queue.pop();
            }

            // delete previously emulated trace
            for (const auto& key : tx_keys_to_delete) {
                transaction_.hdel(trace_->id.to_hex(), key);
            }
            for (const auto& [addr, by_addr_key] : addr_keys_to_delete) {
                transaction_.zrem(addr, by_addr_key);
            }

            // insert new trace
            for (const auto& node : flattened_trace) {
                std::stringstream buffer;
                msgpack::pack(buffer, std::move(node));

                transaction_.hset(trace_->id.to_hex(), node.transaction.in_msg.value().hash.to_hex(), buffer.str());

                auto addr_raw = std::to_string(node.transaction.account.workchain) + ":" + node.transaction.account.addr.to_hex();
                auto by_addr_key = trace_->id.to_hex() + ":" + node.transaction.in_msg.value().hash.to_hex();
                transaction_.zadd(addr_raw, by_addr_key, node.transaction.lt);
            }

            // insert interfaces
            for (const auto& [addr, interfaces] : addr_interfaces) {
                auto interfaces_redis = parse_interfaces(interfaces);
                std::stringstream buffer;
                msgpack::pack(buffer, interfaces_redis);
                auto addr_raw = std::to_string(addr.workchain) + ":" + addr.addr.to_hex();
                transaction_.hset(trace_->id.to_hex(), addr_raw, buffer.str());
            }

            transaction_.publish("new_trace", trace_->id.to_hex());
            transaction_.exec();

            promise_.set_value(td::Unit());
        } catch (const vm::VmError &e) {
            promise_.set_error(td::Status::Error("Got VmError while inserting trace: " + std::string(e.get_msg())));
        } catch (const std::exception &e) {
            promise_.set_error(td::Status::Error("Got exception while inserting trace: " + std::string(e.what())));
        }
        stop();
    }
    void delete_db_subtree(std::string key, std::vector<std::string>& tx_keys, std::vector<std::pair<std::string, std::string>>& addr_keys) {
        auto emulated_in_db = transaction_.redis().hget(trace_->id.to_hex(), key);
        if (emulated_in_db) {
            auto serialized = emulated_in_db.value();
            TraceNode node;
            msgpack::unpacked result;
            msgpack::unpack(result, serialized.data(), serialized.size());
            result.get().convert(node);
            for (const auto& out_msg : node.transaction.out_msgs) {
                delete_db_subtree(out_msg.hash.to_hex(), tx_keys, addr_keys);
            }
            tx_keys.push_back(key);

            auto addr_raw = std::to_string(node.transaction.account.workchain) + ":" + node.transaction.account.addr.to_hex();
            auto by_addr_key = trace_->id.to_hex() + ":" + node.transaction.in_msg.value().hash.to_hex();
            addr_keys.push_back(std::make_pair(addr_raw, by_addr_key));
        }
    }
};

void RedisTaskResultInsertManager::insert(td::Result<std::unique_ptr<Trace>> result, td::Promise<td::Unit> promise) {
    td::actor::create_actor<TaskResultInserter>("TraceInserter", redis_.transaction(), std::move(result), std::move(promise)).release();
}