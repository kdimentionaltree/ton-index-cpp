#pragma once
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"


class TraceInserter: public td::actor::Actor {
private:
    sw::redis::Redis redis_;
    std::unique_ptr<Trace> trace_;
    td::Promise<td::Unit> promise_;

public:
    TraceInserter(std::string redis_dsn, std::unique_ptr<Trace> trace, td::Promise<td::Unit> promise) :
        redis_(sw::redis::Redis(redis_dsn)), trace_(std::move(trace)), promise_(std::move(promise)) {
    }

    void start_up() override;
    void delete_db_subtree(std::string key);
};