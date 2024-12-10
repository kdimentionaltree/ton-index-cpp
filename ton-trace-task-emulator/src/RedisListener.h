#pragma once
#include <td/actor/actor.h>
#include <validator/impl/external-message.hpp>
#include <emulator/transaction-emulator.h>
#include <sw/redis++/redis++.h>
#include "IndexData.h"
#include "TraceEmulator.h"
#include "TraceInterfaceDetector.h"

struct TraceEmulationResult {
  std::string task_id;
  td::Result<std::unique_ptr<Trace>> trace;
  ton::BlockId mc_block_id;
};

class RedisListener : public td::actor::Actor {
private:
  sw::redis::Redis redis_;
  std::string queue_name_;
  std::function<void(TraceEmulationResult, td::Promise<td::Unit>)> trace_processor_;

  std::vector<td::Ref<vm::Cell>> shard_states_;
  std::shared_ptr<emulator::TransactionEmulator> emulator_;
  ton::BlockId current_mc_block_id_;
  std::shared_ptr<block::ConfigInfo> config_;

  std::unordered_set<td::Bits256, BitArrayHasher> known_ext_msgs_; // this set grows infinitely. TODO: remove old messages

public:
  RedisListener(std::string redis_dsn, std::string queue_name, typeof(trace_processor_) trace_processor)
      : redis_(sw::redis::Redis(redis_dsn)), queue_name_(queue_name), trace_processor_(std::move(trace_processor)) {};

  virtual void start_up() override;

  void alarm() override;
  void set_mc_data_state(MasterchainBlockDataState mc_data_state);

private:
  void trace_error(std::string task_id, ton::BlockId mc_block_id, td::Status error);
  void trace_received(std::string task_id, ton::BlockId mc_block_id, Trace *trace);
  void trace_interfaces_error(std::string task_id, ton::BlockId mc_block_id, td::Status error);
  void finish_processing(std::string task_id, ton::BlockId mc_block_id, std::unique_ptr<Trace> trace);
};