#pragma once
#include <sw/redis++/redis++.h>
#include "td/actor/actor.h"
#include "crypto/common/bitstring.h"
#include "TraceEmulator.h"


class ITaskResultInserter : public td::actor::Actor {
public:
    virtual void insert(td::Result<std::unique_ptr<Trace>> trace, td::Promise<td::Unit> promise) = 0;
};

class RedisTaskResultInsertManager: public ITaskResultInserter {
private:
    sw::redis::Redis redis_;

public:
    RedisTaskResultInsertManager(std::string redis_dsn) :
        redis_(sw::redis::Redis(redis_dsn)) {}

    void insert(td::Result<std::unique_ptr<Trace>> trace, td::Promise<td::Unit> promise);
};