// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
#include "prune_session_cache_job.h"
#include <vespa/fastos/timestamp.h>
#include <chrono>

using fastos::TimeStamp;
using namespace std::chrono;

namespace proton {

using matching::ISessionCachePruner;

PruneSessionCacheJob::PruneSessionCacheJob(ISessionCachePruner &pruner, double jobInterval)
    : IMaintenanceJob("prune_session_cache", jobInterval, jobInterval),
      _pruner(pruner)
{
}

bool
PruneSessionCacheJob::run()
{
    TimeStamp now(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
    _pruner.pruneTimedOutSessions(now);
    return true;
}

} // namespace proton
