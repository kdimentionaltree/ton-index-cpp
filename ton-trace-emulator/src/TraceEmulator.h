#pragma once
#include <any>
#include <td/actor/actor.h>
#include <emulator/transaction-emulator.h>
#include "DbScanner.h"

#include "smc-interfaces/InterfacesDetector.h"

using TraceId = td::Bits256;


struct TraceNode {
    td::Bits256 node_id; // hash of cur tx in msg
    block::StdAddress address;
    std::vector<std::unique_ptr<TraceNode>> children;
    td::Ref<vm::Cell> transaction_root;
    bool emulated;

    int depth() const {
        int res = 0;
        for (const auto& child : children) {
            res = std::max(res, child->depth());
        }
        return res + 1;
    }

    int transactions_count() const {
        int res = transaction_root.not_null() ? 1 : 0;
        for (const auto& child : children) {
            res += child->transactions_count();
        }
        return res;
    }

    std::string to_string(int tabs = 0) const {
        std::stringstream ss;
        ss << std::endl;
        for (int i = 0; i < tabs; i++) {
            ss << "--";
        }
        // print account, lt, status, bounced, in_msg opcode
        block::gen::Transaction::Record trans;
        block::gen::TransactionDescr::Record_trans_ord descr;
        tlb::unpack_cell(transaction_root, trans);
        tlb::unpack_cell(trans.description, descr);
        ss << "TX acc=" << trans.account_addr.to_hex() << " lt=" << trans.lt << " outmsg_cnt=" << trans.outmsg_cnt << " aborted=" << descr.aborted << std::endl;

        for (const auto& child : children) {
            ss << child->to_string(tabs + 1);
        }
        return ss.str();
    }
};

struct Trace {
    TraceId id;
    std::unique_ptr<TraceNode> root;

    using Detector = InterfacesDetector<JettonWalletDetectorR, JettonMasterDetectorR, 
                                        NftItemDetectorR, NftCollectionDetectorR,
                                        GetGemsNftFixPriceSale, GetGemsNftAuction>;

    std::unordered_map<block::StdAddress, block::Account> emulated_accounts;
    std::unordered_map<block::StdAddress, std::vector<typename Detector::DetectedInterface>> interfaces;

    int depth() const {
        return root->depth();
    }

    int transactions_count() const {
        return root->transactions_count();
    }

    std::string to_string() const {
        return root->to_string();
    }
};

td::Result<block::StdAddress> fetch_msg_dest_address(td::Ref<vm::Cell> msg, int& type);

class TraceEmulatorImpl: public td::actor::Actor {
private:
    std::shared_ptr<emulator::TransactionEmulator> emulator_;
    std::vector<td::Ref<vm::Cell>> shard_states_;
    std::unordered_map<block::StdAddress, block::Account>& emulated_accounts_;
    std::mutex& emulated_accounts_mutex_;
    std::unordered_map<block::StdAddress, td::actor::ActorOwn<TraceEmulatorImpl>>& emulator_actors_;
    std::unordered_map<TraceNode *, td::Promise<TraceNode *>> result_promises_;

    uint32_t utime_{0};
public:
    TraceEmulatorImpl(std::shared_ptr<emulator::TransactionEmulator> emulator, std::vector<td::Ref<vm::Cell>> shard_states, 
                  std::unordered_map<block::StdAddress, block::Account>& emulated_accounts, std::mutex& emulated_accounts_mutex,
                  std::unordered_map<block::StdAddress, td::actor::ActorOwn<TraceEmulatorImpl>>& emulator_actors)
        : emulator_(std::move(emulator)), shard_states_(std::move(shard_states)), 
          emulated_accounts_(emulated_accounts), emulated_accounts_mutex_(emulated_accounts_mutex), emulator_actors_(emulator_actors) {
    }

    void emulate(td::Ref<vm::Cell> in_msg, block::StdAddress address, size_t depth, td::Promise<TraceNode *> promise);

private:
    td::Result<block::Account> unpack_account(vm::AugmentedDictionary& accounts_dict, const block::StdAddress& account_addr, uint32_t utime);
    void emulate_transaction(block::Account account, block::StdAddress address,
                            td::Ref<vm::Cell> in_msg, size_t depth, td::Promise<TraceNode *> promise);
    void child_emulated(TraceNode *trace, TraceNode *child, size_t ind);
    void child_error(TraceNode *trace, td::Status error);
};

// Emulates whole trace, in_msg is external inbound message
class TraceEmulator: public td::actor::Actor {
private:
    MasterchainBlockDataState mc_data_state_;
    td::Ref<vm::Cell> in_msg_;
    bool ignore_chksig_;
    td::Promise<Trace> promise_;

    std::shared_ptr<emulator::TransactionEmulator> emulator_;
    std::unordered_map<block::StdAddress, block::Account> emulated_accounts_;
    std::mutex emulated_accounts_mutex_;
    std::unordered_map<block::StdAddress, td::actor::ActorOwn<TraceEmulatorImpl>> emulator_actors_;
public:
    TraceEmulator(MasterchainBlockDataState mc_data_state, td::Ref<vm::Cell> in_msg, bool ignore_chksig, td::Promise<Trace> promise)
        : mc_data_state_(std::move(mc_data_state)), in_msg_(std::move(in_msg)), ignore_chksig_(ignore_chksig), promise_(std::move(promise)) {
    }

    void start_up() override;
    void finish(td::Result<TraceNode *> root);
};

