#pragma once
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"


class TraceInserter: public td::actor::Actor {
private:
    sw::redis::Transaction transaction_;
    std::unique_ptr<Trace> trace_;
    td::Promise<td::Unit> promise_;

public:
    TraceInserter(sw::redis::Transaction&& transaction, std::unique_ptr<Trace> trace, td::Promise<td::Unit> promise) :
        transaction_(std::move(transaction)), trace_(std::move(trace)), promise_(std::move(promise)) {
    }

    void start_up() override;
    void delete_db_subtree(std::string key, std::vector<std::string>& tx_keys, std::vector<std::pair<std::string, std::string>>& addr_keys);
};


class RedisInsertManager: public td::actor::Actor {
private:
    sw::redis::Redis redis_;

public:
    RedisInsertManager(std::string redis_dsn) :
        redis_(sw::redis::Redis(redis_dsn)) {}

    void insert(std::unique_ptr<Trace> trace, td::Promise<td::Unit> promise) {
        auto tx = redis_.transaction();
        td::actor::create_actor<TraceInserter>("TraceInserter", std::move(tx), std::move(trace), std::move(promise)).release();
    }
};