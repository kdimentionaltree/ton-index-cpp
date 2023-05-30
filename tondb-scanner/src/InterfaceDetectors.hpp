#pragma once
#include "td/actor/actor.h"
#include "vm/cells/Cell.h"
#include "vm/stack.hpp"
#include "common/refcnt.hpp"
#include "smc-envelope/SmartContract.h"
#include "crypto/block/block-auto.h"
#include "td/utils/base64.h"
#include "IndexData.h"
#include "td/actor/MultiPromise.h"
#include "convert-utils.h"
#include "InsertManager.h"
#include "tokens.h"
#include "crypto/block/block-parse.h"
#include "parse_token_data.h"

enum SmcInterface {
  IT_JETTON_MASTER,
  IT_JETTON_WALLET,
  IT_NFT_COLLECTION,
  IT_NFT_ITEM
};

class InterfaceManager: public td::actor::Actor {
private:
  std::map<std::pair<vm::CellHash, SmcInterface>, bool> cache_{};
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
public:
  // Interfaces table will consist of 3 columns: code_hash, interface, has_interface
  InterfaceManager(td::actor::ActorId<InsertManagerInterface> insert_manager) : insert_manager_(insert_manager) {
  }

  void check_interface(vm::CellHash code_hash, SmcInterface interface, td::Promise<bool> promise) {
    if (cache_.count({code_hash, interface})) {
      promise.set_value(std::move(cache_[{code_hash, interface}]));
      return;
    }
    promise.set_error(td::Status::Error(ErrorCode::NOT_FOUND_ERROR, "Unknown code hash"));
  }

  void set_interface(vm::CellHash code_hash, SmcInterface interface, bool has, td::Promise<td::Unit> promise) {
    cache_[{code_hash, interface}] = has;
    promise.set_value(td::Unit());
  }
};

template <typename T>
class InterfaceDetector: public td::actor::Actor {
public:
  virtual void detect(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt, td::Promise<T> promise) = 0;
  virtual ~InterfaceDetector() = default;
};

template <class T>
class CacheManager {
public:
  std::map<std::string, T> cache_{};
  td::actor::ActorId<InsertManagerInterface> insert_manager_;

  CacheManager(td::actor::ActorId<InsertManagerInterface> insert_manager) 
    : insert_manager_(insert_manager) {
  }

  void check_cache(block::StdAddress address, td::Promise<T> promise) {
    auto it = cache_.find(convert::to_raw_address(address));
    if (it != cache_.end()) {
      auto res = it->second;
      promise.set_value(std::move(res));
      return;
    }

    td::actor::send_closure(insert_manager_, &InsertManagerInterface::get_entity<T>, convert::to_raw_address(address), 
                            promise.wrap([this, address](td::Result<T> r_data) mutable -> td::Result<T> {
      if (r_data.is_error()) {
        return r_data.move_as_error();
      }
      auto data = r_data.move_as_ok();
      cache_.emplace(convert::to_raw_address(address), data);
      return data;
    }));
  }

  td::Status add_to_cache(block::StdAddress address, T data) {
    cache_.emplace(convert::to_raw_address(address), data);
    
    auto P = td::PromiseCreator::lambda([this, address](td::Result<td::Unit> r_unit) mutable {
      if (r_unit.is_error()) {
        LOG(ERROR) << "Failed to add to db: " << r_unit.move_as_error();
        return;
      }
    });
    // You need to implement upsert_data() in the child class
    td::actor::send_closure(insert_manager_, &InsertManagerInterface::upsert_entity<T>, data, std::move(P));

    return td::Status::OK();
  }
};


/// @brief Detects Jetton Master according to TEP 74
/// Checks that get_jetton_data() returns (int total_supply, int mintable, slice admin_address, cell jetton_content, cell jetton_wallet_code)
class JettonMasterDetector: public InterfaceDetector<JettonMasterData>,
                            public CacheManager<JettonMasterData> {
private:
  td::actor::ActorId<InterfaceManager> interface_manager_;
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
public:
  JettonMasterDetector(td::actor::ActorId<InterfaceManager> interface_manager, td::actor::ActorId<InsertManagerInterface> insert_manager) 
    : CacheManager<JettonMasterData>(insert_manager)
    , interface_manager_(interface_manager)
    , insert_manager_(insert_manager) {
  }

  void detect(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt, td::Promise<JettonMasterData> promise) override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<bool> code_hash_is_master) mutable {
      if (code_hash_is_master.is_error()) {
        if (code_hash_is_master.error().code() == ErrorCode::NOT_FOUND_ERROR) { // check for this code hash was not performed
          td::actor::send_closure(SelfId, &JettonMasterDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
          return;
        }

        LOG(ERROR) << "Failed to get interfaces for " << convert::to_raw_address(address) << ": " << code_hash_is_master.error();
        promise.set_error(code_hash_is_master.move_as_error());
        return;
      }
      if (!code_hash_is_master.move_as_ok()) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Code hash is not a Jetton Master"));
        return;
      }

      td::actor::send_closure(SelfId, &JettonMasterDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    td::actor::send_closure(interface_manager_, &InterfaceManager::check_interface, code_cell->get_hash(), IT_JETTON_MASTER, std::move(P));
  }

  void detect_continue(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<JettonMasterData> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<JettonMasterData> cached_res) mutable {
      if (cached_res.is_ok()) {
        auto cached_data = cached_res.move_as_ok();
        if ((data_cell->get_hash() == cached_data.data_hash && code_cell->get_hash() == cached_data.code_hash) 
            || last_tx_lt < cached_data.last_transaction_lt) {
          promise.set_value(std::move(cached_data)); // data did not not changed from cached or is more actual than requested
          return;
        }
      }
      td::actor::send_closure(SelfId, &JettonMasterDetector::detect_impl, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    check_cache(address, std::move(P));
  }

  void detect_impl(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<JettonMasterData> promise) {
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(std::move(address));

    args.set_method_id("get_jetton_data");
    auto res = smc.run_get_method(args);

    const int return_stack_size = 5;
    const vm::StackEntry::Type return_types[return_stack_size] = {vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_int, 
      vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_cell, vm::StackEntry::Type::t_cell};

    if (!res.success || res.stack->depth() != return_stack_size) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_jetton_data failed"));
      return;
    }

    auto stack = res.stack->as_span();
    
    for (int i = 0; i < return_stack_size; i++) {
      if (stack[i].type() != return_types[i]) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_jetton_data failed"));
        return;
      }
    }

    JettonMasterData data;
    data.address = convert::to_raw_address(address);
    data.total_supply = stack[0].as_int()->to_long();
    data.mintable = stack[1].as_int()->to_long() != 0;
    auto admin_address = convert::to_raw_address(stack[2].as_slice());
    if (admin_address.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_jetton_data address parsing failed"));
    }
    data.admin_address = admin_address.move_as_ok();
    data.last_transaction_lt = last_tx_lt;
    data.data_hash = data_cell->get_hash();
    data.code_boc = td::base64_encode(vm::std_boc_serialize(code_cell).move_as_ok());
    data.data_boc = td::base64_encode(vm::std_boc_serialize(data_cell).move_as_ok());
    
    auto jetton_content = parse_token_data(stack[3].as_cell());
    if (jetton_content.is_ok()) {
      data.jetton_content = jetton_content.move_as_ok();
    } else {
      LOG(ERROR) << "Failed to parse jetton content for " << convert::to_raw_address(address) << ": " << jetton_content.error();
      LOG(ERROR) << convert::to_bytes(stack[3].as_cell()).move_as_ok().value();
    }
    data.jetton_wallet_code_hash = stack[4].as_cell()->get_hash();
    
    add_to_cache(address, data);

    promise.set_value(std::move(data));
  }

  void get_wallet_address(block::StdAddress master_address, block::StdAddress owner_address, td::Promise<block::StdAddress> promise) {
    auto P = td::PromiseCreator::lambda([this, master_address, owner_address, promise = std::move(promise)](td::Result<JettonMasterData> r) mutable {
      if (r.is_error()) {
        promise.set_error(r.move_as_error());
        return;
      }
      get_wallet_address_impl(r.move_as_ok(), master_address, owner_address, std::move(promise));
    });
    check_cache(master_address, std::move(P));
  }

  void get_wallet_address_impl(JettonMasterData data, block::StdAddress master_address, block::StdAddress owner_address, td::Promise<block::StdAddress> P) {
    auto code_cell = vm::std_boc_deserialize(td::base64_decode(data.code_boc).move_as_ok()).move_as_ok();
    auto data_cell = vm::std_boc_deserialize(td::base64_decode(data.data_boc).move_as_ok()).move_as_ok();
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;

    vm::CellBuilder anycast_cb;
    anycast_cb.store_bool_bool(false);
    auto anycast_cell = anycast_cb.finalize();
    td::Ref<vm::CellSlice> anycast_cs = vm::load_cell_slice_ref(anycast_cell);

    vm::CellBuilder cb;
    block::gen::t_MsgAddressInt.pack_addr_std(cb, anycast_cs, owner_address.workchain, owner_address.addr);
    auto owner_address_cell = cb.finalize();

    args.set_now(td::Time::now());
    args.set_address(master_address);
    args.set_stack({vm::StackEntry(vm::load_cell_slice_ref(owner_address_cell))});
    
    args.set_method_id("get_wallet_address");
    auto res = smc.run_get_method(args);

    if (!res.success || res.stack->depth() != 1) {
      P.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_wallet_address failed"));
      return;
    }

    auto stack = res.stack->as_span();
    if (stack[0].type() != vm::StackEntry::Type::t_slice) {
      P.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_wallet_address failed"));
      return;
    }

    auto wallet_address = convert::to_raw_address(stack[0].as_slice());
    if (wallet_address.is_error()) {
      P.set_error(wallet_address.move_as_error());
      return;
    }
    P.set_result(block::StdAddress::parse(wallet_address.move_as_ok()));
  }
};

/// @brief Detects Jetton Wallet according to TEP 74
/// Checks that get_wallet_data() returns (int balance, slice owner, slice jetton, cell jetton_wallet_code) 
/// and corresponding jetton master recognizes this wallet
class JettonWalletDetector: public InterfaceDetector<JettonWalletData>,
                            public CacheManager<JettonWalletData> {
private:
  td::actor::ActorId<JettonMasterDetector> jetton_master_detector_;
  td::actor::ActorId<InterfaceManager> interface_manager_;
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
public:
  JettonWalletDetector(td::actor::ActorId<JettonMasterDetector> jetton_master_detector,
                       td::actor::ActorId<InterfaceManager> interface_manager,
                       td::actor::ActorId<InsertManagerInterface> insert_manager) 
    : CacheManager<JettonWalletData>(insert_manager)
    , jetton_master_detector_(jetton_master_detector)
    , interface_manager_(interface_manager)
    , insert_manager_(insert_manager) {
  }

  void detect(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt, td::Promise<JettonWalletData> promise) override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<bool> code_hash_is_wallet) mutable {
      if (code_hash_is_wallet.is_error()) {
        if (code_hash_is_wallet.error().code() == ErrorCode::NOT_FOUND_ERROR) { // check for code hash was not performed
          td::actor::send_closure(SelfId, &JettonWalletDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
          return;
        }

        LOG(ERROR) << "Failed to get interfaces for " << convert::to_raw_address(address) << ": " << code_hash_is_wallet.error();
        promise.set_error(code_hash_is_wallet.move_as_error());
        return;
      }
      if (!code_hash_is_wallet.move_as_ok()) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Code hash is not a Jetton Wallet"));
        return;
      }

      td::actor::send_closure(SelfId, &JettonWalletDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    td::actor::send_closure(interface_manager_, &InterfaceManager::check_interface, code_cell->get_hash(), IT_JETTON_WALLET, std::move(P));
  }

  void detect_continue(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<JettonWalletData> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<JettonWalletData> cached_res) mutable {
      if (cached_res.is_ok()) {
        auto cached_data = cached_res.move_as_ok();
        if ((data_cell->get_hash() == cached_data.data_hash && code_cell->get_hash() == cached_data.code_hash) 
            || last_tx_lt < cached_data.last_transaction_lt) {
          promise.set_value(std::move(cached_data)); // data did not not changed from cached or is more actual than requested
          return;
        }
      }
      td::actor::send_closure(SelfId, &JettonWalletDetector::detect_impl, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    check_cache(address, std::move(P));
  }

  void detect_impl(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<JettonWalletData> promise) {
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(std::move(address));

    args.set_method_id("get_wallet_data");
    auto res = smc.run_get_method(args);

    const int return_stack_size = 4;
    const vm::StackEntry::Type return_types[return_stack_size] = {vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_cell};

    if (!res.success || res.stack->depth() != return_stack_size) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_wallet_data failed"));
      return;
    }

    auto stack = res.stack->as_span();
    
    for (int i = 0; i < 4; i++) {
      if (stack[i].type() != return_types[i]) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_wallet_data failed"));
        return;
      }
    }

    JettonWalletData data;
    data.address = convert::to_raw_address(address);
    data.balance = stack[0].as_int()->to_long();
    auto owner = convert::to_raw_address(stack[1].as_slice());
    if (owner.is_error()) {
      promise.set_error(owner.move_as_error());
      return;
    }
    data.owner = owner.move_as_ok();
    auto jetton = convert::to_raw_address(stack[2].as_slice());
    if (jetton.is_error()) {
      promise.set_error(jetton.move_as_error());
      return;
    }
    data.jetton = jetton.move_as_ok();
    data.last_transaction_lt = last_tx_lt;
    data.code_hash = code_cell->get_hash();
    data.data_hash = data_cell->get_hash();

    // if (stack[3].as_cell()->get_hash() != code_cell->get_hash()) {
      // LOG(WARNING) << "Jetton Wallet code hash mismatch: " << stack[3].as_cell()->get_hash().to_hex() << " != " << code_cell->get_hash().to_hex();
    // }

    verify_belonging_to_master(std::move(data), std::move(promise));
  }

  void parse_transfer(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<JettonTransfer> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, transaction, cs = std::move(cs), promise = std::move(promise)](td::Result<JettonWalletData> R) mutable {
      if (R.is_error()) {
        if (R.error().code() == ErrorCode::NOT_FOUND_ERROR) {
          promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Jetton Wallet not found"));
          return;
        }
        promise.set_error(R.move_as_error());
        return;
      }
      td::actor::send_closure(SelfId, &JettonWalletDetector::parse_transfer_impl, transaction, std::move(cs), std::move(promise));
    });

    check_cache(block::StdAddress::parse(transaction.account).move_as_ok(), std::move(P));
  }

  void parse_transfer_impl(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<JettonTransfer> promise) {
    tokens::gen::InternalMsgBody::Record_transfer_jetton transfer_record;
    if (!tlb::csr_unpack(cs, transfer_record)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to unpack transfer"));
      return;
    }

    JettonTransfer transfer;
    transfer.transaction_hash = transaction.hash;
    transfer.query_id = transfer_record.query_id;
    transfer.amount = block::tlb::t_VarUInteger_16.as_integer(transfer_record.amount);
    if (transfer.amount.is_null()) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to unpack transfer amount"));
      return;
    }
    auto destination = convert::to_raw_address(transfer_record.destination);
    if (destination.is_error()) {
      promise.set_error(destination.move_as_error());
    }
    transfer.destination = destination.move_as_ok();
    auto response_destination = convert::to_raw_address(transfer_record.response_destination);
    if (response_destination.is_error()) {
      promise.set_error(response_destination.move_as_error());
    }
    transfer.response_destination = response_destination.move_as_ok();
    if (!transfer_record.custom_payload.write().fetch_maybe_ref(transfer.custom_payload)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch custom payload"));
      return;
    }
    transfer.forward_ton_amount = block::tlb::t_VarUInteger_16.as_integer(transfer_record.forward_ton_amount);
    if (!transfer_record.forward_payload.write().fetch_maybe_ref(transfer.forward_payload)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch forward payload"));
      return;
    }

    promise.set_value(std::move(transfer));
  }

  void parse_burn(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<JettonBurn> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, transaction, cs = std::move(cs), promise = std::move(promise)](td::Result<JettonWalletData> R) mutable {
      if (R.is_error()) {
        if (R.error().code() == ErrorCode::NOT_FOUND_ERROR) {
          promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Jetton Wallet not found"));
          return;
        }
        promise.set_error(R.move_as_error());
        return;
      }
      td::actor::send_closure(SelfId, &JettonWalletDetector::parse_burn_impl, transaction, std::move(cs), std::move(promise));
    });

    check_cache(block::StdAddress::parse(transaction.account).move_as_ok(), std::move(P));
  }

  void parse_burn_impl(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<JettonBurn> promise) {
    tokens::gen::InternalMsgBody::Record_burn burn_record;
    if (!tlb::csr_unpack(cs, burn_record)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to unpack burn"));
      return;
    }

    JettonBurn burn;
    burn.transaction_hash = transaction.hash;
    burn.query_id = burn_record.query_id;
    burn.amount = block::tlb::t_VarUInteger_16.as_integer(burn_record.amount);
    if (burn.amount.is_null()) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to unpack burn amount"));
      return;
    }
    auto response_destination = convert::to_raw_address(burn_record.response_destination);
    if (response_destination.is_error()) {
      promise.set_error(response_destination.move_as_error());
      return;
    }
    burn.response_destination = response_destination.move_as_ok();
    if (!burn_record.custom_payload.write().fetch_maybe_ref(burn.custom_payload)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch custom payload"));
      return;
    }

    promise.set_value(std::move(burn));
  }

private:
  // checks belonging of address to Jetton Master by calling get_wallet_address
  void verify_belonging_to_master(JettonWalletData data, td::Promise<JettonWalletData> &&promise) {
    auto master_addr = block::StdAddress::parse(data.jetton);
    if (master_addr.is_error()) {
      promise.set_error(master_addr.move_as_error_prefix(PSLICE() << "Failed to parse jetton master address (" << data.jetton << "): "));
      return;
    }
    auto owner_addr = block::StdAddress::parse(data.owner);
    if (owner_addr.is_error()) {
      promise.set_error(owner_addr.move_as_error_prefix(PSLICE() << "Failed to parse jetton owner address (" << data.owner << "): "));
      return;
    }

    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, data, promise = std::move(promise)](td::Result<block::StdAddress> R) mutable {
      if (R.is_error()) {
        if (R.error().code() == ErrorCode::NOT_FOUND_ERROR) {
          // Jetton master is not available, so we can't verify address.
          // We will add it to cache and return it as is.
          // TODO: figure out what to do in this case properly
          add_to_cache(block::StdAddress::parse(data.address).move_as_ok(), data);
          promise.set_value(std::move(data));
        } else {
          LOG(ERROR) << "Failed to get wallet address from master: " << R.error();
          promise.set_error(R.move_as_error());
        }
      } else {
        auto address = R.move_as_ok();
        if (convert::to_raw_address(address) != data.address) {
          LOG(ERROR) << "Jetton Master returned wrong address: " << convert::to_raw_address(address);
          promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Couldn't verify Jetton Wallet. Possibly scam."));
        } else {
          add_to_cache(address, data);
          promise.set_value(std::move(data));
        }
      }
    });

    td::actor::send_closure(jetton_master_detector_, &JettonMasterDetector::get_wallet_address, master_addr.move_as_ok(), owner_addr.move_as_ok(), std::move(P));
  }
};


/// @brief Detects NFT Collection according to your specific standard
/// Checks that get_collection_data() returns (int next_item_index, cell collection_content, slice owner_address)
class NFTCollectionDetector: public InterfaceDetector<NFTCollectionData>,
                             public CacheManager<NFTCollectionData> {
private:
  td::actor::ActorId<InterfaceManager> interface_manager_;
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
public:
  NFTCollectionDetector(td::actor::ActorId<InterfaceManager> interface_manager, td::actor::ActorId<InsertManagerInterface> insert_manager) 
    : CacheManager<NFTCollectionData>(insert_manager)
    , interface_manager_(interface_manager)
    , insert_manager_(insert_manager) {
  }

  void get_from_cache(block::StdAddress address, td::Promise<NFTCollectionData> promise) {
    // TODO: fetch from shards
    check_cache(address, std::move(promise));
  }

  void detect(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt, td::Promise<NFTCollectionData> promise) override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<bool> code_hash_is_collection) mutable {
      if (code_hash_is_collection.is_error()) {
        if (code_hash_is_collection.error().code() == ErrorCode::NOT_FOUND_ERROR) { 
          td::actor::send_closure(SelfId, &NFTCollectionDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
          return;
        }

        LOG(ERROR) << "Failed to get interfaces for " << convert::to_raw_address(address) << ": " << code_hash_is_collection.error();
        promise.set_error(code_hash_is_collection.move_as_error());
        return;
      }
      if (!code_hash_is_collection.move_as_ok()) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Code hash is not a NFT Collection"));
        return;
      }

      td::actor::send_closure(SelfId, &NFTCollectionDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    td::actor::send_closure(interface_manager_, &InterfaceManager::check_interface, code_cell->get_hash(), IT_NFT_COLLECTION, std::move(P));
  }
private:
  void detect_continue(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<NFTCollectionData> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<NFTCollectionData> cached_res) mutable {
      if (cached_res.is_ok()) {
        auto cached_data = cached_res.move_as_ok();
        if ((data_cell->get_hash() == cached_data.data_hash && code_cell->get_hash() == cached_data.code_hash) 
            || last_tx_lt < cached_data.last_transaction_lt) {
          promise.set_value(std::move(cached_data)); // data did not not changed from cached or is more actual than requested
          return;
        }
      }
      td::actor::send_closure(SelfId, &NFTCollectionDetector::detect_impl, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    check_cache(address, std::move(P));
  }

  void detect_impl(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<NFTCollectionData> promise) {
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(std::move(address));

    args.set_method_id("get_collection_data");
    auto res = smc.run_get_method(args);

    const int return_stack_size = 3;
    const vm::StackEntry::Type return_types[return_stack_size] = {vm::StackEntry::Type::t_int, 
      vm::StackEntry::Type::t_cell, vm::StackEntry::Type::t_slice};

    if (!res.success || res.stack->depth() != return_stack_size) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_collection_data failed"));
      return;
    }

    auto stack = res.stack->as_span();
    
    for (int i = 0; i < return_stack_size; i++) {
      if (stack[i].type() != return_types[i]) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_collection_data failed"));
        return;
      }
    }

    NFTCollectionData data;
    data.address = convert::to_raw_address(address);
    data.next_item_index = stack[0].as_int();
    
    auto owner_address = convert::to_raw_address(stack[2].as_slice());
    if (owner_address.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_collection_data address parsing failed"));
    }
    data.owner_address = owner_address.move_as_ok();
    data.last_transaction_lt = last_tx_lt;
    data.data_hash = data_cell->get_hash();
    data.code_boc = td::base64_encode(vm::std_boc_serialize(code_cell).move_as_ok());
    data.data_boc = td::base64_encode(vm::std_boc_serialize(data_cell).move_as_ok());

    auto collection_content = parse_token_data(stack[1].as_cell());
    if (collection_content.is_ok()) {
      data.collection_content = collection_content.move_as_ok();
    } else {
      LOG(ERROR) << "Failed to parse collection content for " << convert::to_raw_address(address) << ": " << collection_content.error();
      LOG(ERROR) << convert::to_bytes(stack[1].as_cell()).move_as_ok().value();
    }
    
    add_to_cache(address, data);

    promise.set_value(std::move(data));
  }
};


/// @brief Detects NFT Item according to your specific standard
/// Checks that get_nft_data() returns (int init?, int index, slice collection_address, slice owner_address, cell individual_content)
class NFTItemDetector: public InterfaceDetector<NFTItemData>,
                       public CacheManager<NFTItemData> {
private:
  td::actor::ActorId<InterfaceManager> interface_manager_;
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
  td::actor::ActorId<NFTCollectionDetector> collection_detector_;
public:
  NFTItemDetector(td::actor::ActorId<InterfaceManager> interface_manager, td::actor::ActorId<InsertManagerInterface> insert_manager, td::actor::ActorId<NFTCollectionDetector> collection_detector) 
    : CacheManager<NFTItemData>(insert_manager)
    , interface_manager_(interface_manager)
    , insert_manager_(insert_manager)
    , collection_detector_(collection_detector) {
  }

  void detect(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt, td::Promise<NFTItemData> promise) override {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<bool> code_hash_is_collection) mutable {
      if (code_hash_is_collection.is_error()) {
        if (code_hash_is_collection.error().code() == ErrorCode::NOT_FOUND_ERROR) { 
          td::actor::send_closure(SelfId, &NFTItemDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
          return;
        }

        LOG(ERROR) << "Failed to get interfaces for " << convert::to_raw_address(address) << ": " << code_hash_is_collection.error();
        promise.set_error(code_hash_is_collection.move_as_error());
        return;
      }
      if (!code_hash_is_collection.move_as_ok()) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Code hash is not a NFT Collection"));
        return;
      }

      td::actor::send_closure(SelfId, &NFTItemDetector::detect_continue, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    td::actor::send_closure(interface_manager_, &InterfaceManager::check_interface, code_cell->get_hash(), IT_NFT_COLLECTION, std::move(P));
  }

  void parse_transfer(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<NFTTransfer> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, transaction, cs = std::move(cs), promise = std::move(promise)](td::Result<NFTItemData> R) mutable {
      if (R.is_error()) {
        if (R.error().code() == ErrorCode::NOT_FOUND_ERROR) {
          promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Jetton Wallet not found"));
          return;
        }
        promise.set_error(R.move_as_error());
        return;
      }
      td::actor::send_closure(SelfId, &NFTItemDetector::parse_transfer_impl, transaction, std::move(cs), std::move(promise));
    });

    check_cache(block::StdAddress::parse(transaction.account).move_as_ok(), std::move(P));
  }

  void parse_transfer_impl(schema::Transaction transaction, td::Ref<vm::CellSlice> cs, td::Promise<NFTTransfer> promise) {
    tokens::gen::InternalMsgBody::Record_transfer_nft transfer_record;
    if (!tlb::csr_unpack(cs, transfer_record)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to unpack transfer"));
      return;
    }

    NFTTransfer transfer;
    transfer.transaction_hash = transaction.hash;
    transfer.query_id = transfer_record.query_id;
    transfer.nft_item = transaction.account;
    if (!transaction.in_msg_from) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch NFT old owner address"));
    }
    transfer.old_owner = transaction.in_msg_from.value();
    auto new_owner = convert::to_raw_address(transfer_record.new_owner);
    if (new_owner.is_error()) {
      promise.set_error(new_owner.move_as_error());
    }
    transfer.new_owner = new_owner.move_as_ok();
    auto response_destination = convert::to_raw_address(transfer_record.response_destination);
    if (response_destination.is_error()) {
      promise.set_error(response_destination.move_as_error());
    }
    transfer.response_destination = response_destination.move_as_ok();
    if (!transfer_record.custom_payload.write().fetch_maybe_ref(transfer.custom_payload)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch custom payload"));
      return;
    }
    transfer.forward_amount = block::tlb::t_VarUInteger_16.as_integer(transfer_record.forward_amount);
    if (!transfer_record.forward_payload.write().fetch_maybe_ref(transfer.forward_payload)) {
      promise.set_error(td::Status::Error(ErrorCode::EVENT_PARSE_ERROR, "Failed to fetch forward payload"));
      return;
    }

    promise.set_value(std::move(transfer));
  }

private:
  void detect_continue(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<NFTItemData> promise) {
    auto P = td::PromiseCreator::lambda([SelfId = actor_id(this), this, address, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<NFTItemData> cached_res) mutable {
      if (cached_res.is_ok()) {
        auto cached_data = cached_res.move_as_ok();
        if ((data_cell->get_hash() == cached_data.data_hash && code_cell->get_hash() == cached_data.code_hash) 
            || last_tx_lt < cached_data.last_transaction_lt) {
          promise.set_value(std::move(cached_data)); // data did not not changed from cached or is more actual than requested
          return;
        }
      }
      td::actor::send_closure(SelfId, &NFTItemDetector::detect_impl, address, code_cell, data_cell, last_tx_lt, std::move(promise));
    });

    check_cache(address, std::move(P));
  }

  void detect_impl(block::StdAddress address, td::Ref<vm::Cell> code_cell, td::Ref<vm::Cell> data_cell, uint64_t last_tx_lt,  td::Promise<NFTItemData> promise) {
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(std::move(address));

    args.set_method_id("get_nft_data");
    auto res = smc.run_get_method(args);

    const int return_stack_size = 5;
    const vm::StackEntry::Type return_types[return_stack_size] = {vm::StackEntry::Type::t_int, 
      vm::StackEntry::Type::t_int, vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_slice, vm::StackEntry::Type::t_cell};

    if (!res.success || res.stack->depth() != return_stack_size) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_data failed"));
      return;
    }

    auto stack = res.stack->as_span();

    for (int i = 0; i < return_stack_size; i++) {
      if (stack[i].type() != return_types[i]) {
        promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_data failed"));
        return;
      }
    }

    NFTItemData data;
    data.address = convert::to_raw_address(address);
    data.init = stack[0].as_int()->to_long() != 0;
    data.index = stack[1].as_int();
    
    auto collection_address = convert::to_raw_address(stack[2].as_slice());
    if (collection_address.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_data collection address parsing failed"));  
    }
    data.collection_address = collection_address.move_as_ok();

    auto owner_address = convert::to_raw_address(stack[3].as_slice());
    if (owner_address.is_error()) {
      promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_data address parsing failed"));
    }
    data.owner_address = owner_address.move_as_ok();
    data.last_transaction_lt = last_tx_lt;
    data.code_hash = code_cell->get_hash();
    data.data_hash = data_cell->get_hash();
    
    if (data.collection_address == "addr_none") {
      auto content = parse_token_data(stack[4].as_cell());
      if (content.is_ok()) {
        data.content = content.move_as_ok();
      } else {
        LOG(ERROR) << "Failed to parse content for " << convert::to_raw_address(address) << ": " << content.error();
        LOG(ERROR) << convert::to_bytes(stack[4].as_cell()).move_as_ok().value();
      }
      add_to_cache(address, data);
      promise.set_value(std::move(data));
    } else {
      auto ind_content = stack[4].as_cell();
      auto collection_address = block::StdAddress::parse(data.collection_address);
      if (collection_address.is_error()) {
        LOG(ERROR) << "Failed to parse collection address for " << convert::to_raw_address(address) << ": " << collection_address.error();
        promise.set_error(collection_address.move_as_error());
        return;
      }
      td::actor::send_closure(collection_detector_, &NFTCollectionDetector::get_from_cache, collection_address.move_as_ok(), 
                              td::PromiseCreator::lambda([this, ind_content, address, data, code_cell, data_cell, last_tx_lt, promise = std::move(promise)](td::Result<NFTCollectionData> collection_res) mutable {
        if (collection_res.is_error()) {
          LOG(ERROR) << "Failed to get collection for " << convert::to_raw_address(address) << ": " << collection_res.error();
          if (collection_res.error().code() == ErrorCode::NOT_FOUND_ERROR) {
            promise.set_error(td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "Collection was not indexed yet"));
          } else {
            promise.set_error(collection_res.move_as_error());
          }
          return;
        }

        auto collection_data = collection_res.move_as_ok();
        auto content = get_content(data.index, ind_content, collection_data);
        if (content.is_error()) {
          LOG(ERROR) << "Failed to parse content for " << convert::to_raw_address(address) << ": " << content.error();
          LOG(ERROR) << convert::to_bytes(ind_content).move_as_ok().value();
        } else {
          data.content = content.move_as_ok();
        }

        auto verify_r = verify_belonging_to_collection(data, collection_data);
        if (verify_r.is_error()) {
          LOG(ERROR) << "Failed to verify belonging to collection for " << convert::to_raw_address(address);
          promise.set_error(verify_r.move_as_error_prefix(PSLICE() << "Failed to verify belonging to collection for " << convert::to_raw_address(address)));
        }

        add_to_cache(address, data);

        promise.set_value(std::move(data));
      }));
    }
  }

  td::Status verify_belonging_to_collection(const NFTItemData& item_data, const NFTCollectionData& collection_data) {
    auto code_cell = vm::std_boc_deserialize(td::base64_decode(collection_data.code_boc).move_as_ok()).move_as_ok();
    auto data_cell = vm::std_boc_deserialize(td::base64_decode(collection_data.data_boc).move_as_ok()).move_as_ok();
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(block::StdAddress::parse(collection_data.address).move_as_ok());
    args.set_stack({vm::StackEntry(item_data.index)});
    args.set_method_id("get_nft_address_by_index");
    auto res = smc.run_get_method(args);

    if (!res.success) {
      return td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_address_by_index failed");
    }

    auto stack = res.stack->as_span();
    if (stack.size() != 1 || stack[0].type() != vm::StackEntry::Type::t_slice) {
      return td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_address_by_index failed");
    }
    auto nft_address = convert::to_raw_address(stack[0].as_slice());
    if (nft_address.is_error()) {
      return td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_address_by_index parse address failed");
    }

    return nft_address.move_as_ok() == item_data.address ? td::Status::OK() : td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "NFT Item doesn't belong to the referred collection");
  }

  td::Result<std::map<std::string, std::string>> get_content(const td::RefInt256 index, td::Ref<vm::Cell> ind_content, const NFTCollectionData& collection_data) {
    auto code_cell = vm::std_boc_deserialize(td::base64_decode(collection_data.code_boc).move_as_ok()).move_as_ok();
    auto data_cell = vm::std_boc_deserialize(td::base64_decode(collection_data.data_boc).move_as_ok()).move_as_ok();
    ton::SmartContract smc({code_cell, data_cell});
    ton::SmartContract::Args args;
    args.set_now(td::Time::now());
    args.set_address(block::StdAddress::parse(collection_data.address).move_as_ok());
    args.set_stack({vm::StackEntry(index), vm::StackEntry(ind_content)});
    args.set_method_id("get_nft_content");
    auto res = smc.run_get_method(args);

    if (!res.success) {
      return td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_content failed");
    }

    auto stack = res.stack->as_span();
    if (stack.size() != 1 || stack[0].type() != vm::StackEntry::Type::t_cell) {
      return td::Status::Error(ErrorCode::SMC_INTERFACE_PARSE_ERROR, "get_nft_content failed");
    }

    return parse_token_data(stack[0].as_cell());
  }
};