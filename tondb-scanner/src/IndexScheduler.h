#pragma once
#include <queue>
#include "td/actor/actor.h"

#include "IndexData.h"
#include "DbScanner.h"
#include "EventProcessor.h"
#include "InsertManager.h"
#include "DataParser.h"


class IndexScheduler: public td::actor::Actor {
private:
  std::queue<std::uint32_t> queued_seqnos_;
  std::set<std::uint32_t> processing_seqnos_;
  std::set<std::uint32_t> existing_seqnos_;

  td::actor::ActorId<DbScanner> db_scanner_;
  td::actor::ActorId<InsertManagerInterface> insert_manager_;
  td::actor::ActorId<ParseManager> parse_manager_;
  td::actor::ActorOwn<EventProcessor> event_processor_;

  std::uint32_t max_active_tasks_{32};
  std::int32_t last_known_seqno_{0};
  std::int32_t last_indexed_seqno_{0};

  std::double_t avg_tps_{0};
  std::int64_t last_existing_seqno_count_{0};

  std::uint32_t max_queue_mc_blocks_{16384};
  std::uint32_t max_queue_blocks_{16384};
  std::uint32_t max_queue_txs_{524288};
  std::uint32_t max_queue_msgs_{524288};
  QueueStatus cur_queue_status_;
public:
  IndexScheduler(td::actor::ActorId<DbScanner> db_scanner, td::actor::ActorId<InsertManagerInterface> insert_manager,
    td::actor::ActorId<ParseManager> parse_manager, std::int32_t last_known_seqno = 0)
    : db_scanner_(db_scanner), insert_manager_(insert_manager), parse_manager_(parse_manager), last_known_seqno_(last_known_seqno) {};

  void start_up() override;
  void alarm() override;
  void run();
private:
  void schedule_next_seqnos();

  void schedule_seqno(std::uint32_t mc_seqno);
  void reschedule_seqno(std::uint32_t mc_seqno);
  void seqno_fetched(std::uint32_t mc_seqno, MasterchainBlockDataState block_data_state);
  void seqno_parsed(std::uint32_t mc_seqno, ParsedBlockPtr parsed_block);
  void seqno_queued_to_insert(std::uint32_t mc_seqno, QueueStatus status);
  void seqno_inserted(std::uint32_t mc_seqno, td::Unit result);

  void got_existing_seqnos(td::Result<std::vector<std::uint32_t>> R);
  void got_last_known_seqno(std::uint32_t last_known_seqno);

  void got_insert_queue_status(QueueStatus status);
};
