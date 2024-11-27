#pragma once
#include <queue>
#include "td/actor/actor.h"
#include "DbScanner.h"
#include "OverlayListener.h"
#include "RedisListener.h"
#include "TraceEmulator.h"
#include "TraceInserter.h"


class TraceEmulatorScheduler : public td::actor::Actor {
  private: 
    td::actor::ActorId<DbScanner> db_scanner_;
    std::string global_config_path_;
    std::string inet_addr_;
    std::string redis_dsn_;
    std::string input_redis_queue_;
    std::function<void(std::unique_ptr<Trace>)> insert_trace_;

    ton::BlockSeqno last_known_seqno_{0};
    ton::BlockSeqno last_fetched_seqno_{0};
    ton::BlockSeqno last_emulated_seqno_{0};

    std::unordered_set<ton::BlockSeqno> seqnos_to_fetch_;
    std::map<ton::BlockSeqno, MasterchainBlockDataState> blocks_to_emulate_;

    td::actor::ActorOwn<OverlayListener> overlay_listener_;
    td::actor::ActorOwn<RedisListener> redis_listener_;

    void got_last_mc_seqno(ton::BlockSeqno last_known_seqno);
    void fetch_seqnos();
    void fetch_error(std::uint32_t seqno, td::Status error);
    void seqno_fetched(std::uint32_t seqno, MasterchainBlockDataState mc_data_state);
    void emulate_blocks();

    void alarm();

  public:
    TraceEmulatorScheduler(td::actor::ActorId<DbScanner> db_scanner, std::string global_config_path, std::string inet_addr, std::string redis_dsn, std::string input_redis_queue) :
        db_scanner_(db_scanner), global_config_path_(global_config_path), inet_addr_(inet_addr), redis_dsn_(redis_dsn), input_redis_queue_(input_redis_queue) {
      insert_trace_ = [redis_dsn](std::unique_ptr<Trace> trace) {
        auto P = td::PromiseCreator::lambda([trace_id = trace->id](td::Result<td::Unit> R) {
          if (R.is_error()) {
            LOG(ERROR) << "Failed to insert trace " << trace_id.to_hex() << ": " << R.move_as_error();
            return;
          }
          LOG(DEBUG) << "Successfully inserted trace " << trace_id.to_hex();
        });
        td::actor::create_actor<TraceInserter>("TraceInserter", redis_dsn, std::move(trace), std::move(P)).release();
      };
    };

    virtual void start_up() override;
};
