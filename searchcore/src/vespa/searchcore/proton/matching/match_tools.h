// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#pragma once

#include "queryenvironment.h"
#include "isearchcontext.h"
#include "query.h"
#include "viewresolver.h"
#include <vespa/vespalib/util/doom.h>
#include "querylimiter.h"
#include "match_phase_limiter.h"
#include "handlerecorder.h"
#include "requestcontext.h"

#include <vespa/searchlib/queryeval/blueprint.h>
#include <vespa/searchlib/fef/fef.h>
#include <vespa/searchlib/common/idocumentmetastore.h>
#include <vespa/searchlib/attribute/diversity.h>
#include <vespa/vespalib/util/clock.h>

namespace proton::matching {

class MatchTools
{
private:
    using DiversityFilter = search::attribute::diversity::DiversityFilter;
    using IRequestContext = search::queryeval::IRequestContext;
    using SearchIterator = search::queryeval::SearchIterator;
    using Properties = search::fef::Properties;
    using MatchDataLayout = search::fef::MatchDataLayout;
    using RankSetup = search::fef::RankSetup;
    QueryLimiter                          &_queryLimiter;
    const vespalib::Doom                  &_softDoom;
    const vespalib::Doom                  &_hardDoom;
    const Query                           &_query;
    MaybeMatchPhaseLimiter                &_match_limiter;
    const QueryEnvironment                &_queryEnv;
    const RankSetup                       &_rankSetup;
    const Properties                      &_featureOverrides;
    bool                                   _useDiversityFilter;
    search::fef::MatchData::UP             _match_data;
    search::fef::RankProgram::UP           _rank_program;
    SearchIterator::UP                     _search;
    HandleRecorder::HandleSet              _used_handles;
    bool                                   _search_has_changed;
    void setup(search::fef::RankProgram::UP, double termwise_limit = 1.0);
public:
    typedef std::unique_ptr<MatchTools> UP;
    MatchTools(const MatchTools &) = delete;
    MatchTools & operator = (const MatchTools &) = delete;
    MatchTools(QueryLimiter & queryLimiter, const vespalib::Doom & softDoom, const vespalib::Doom & hardDoom,
               const Query &query, MaybeMatchPhaseLimiter &match_limiter_in, const QueryEnvironment &queryEnv,
               const MatchDataLayout &mdl, const RankSetup &rankSetup, const Properties &featureOverrides);
    ~MatchTools();
    MatchTools & useDiversityFilter(bool useDiversity) {
        _useDiversityFilter = useDiversity;
        return *this;
    }
    bool useDiversityFilter() const { return _useDiversityFilter; }
    const vespalib::Doom &getSoftDoom() const { return _softDoom; }
    const vespalib::Doom &getHardDoom() const { return _hardDoom; }
    QueryLimiter & getQueryLimiter() { return _queryLimiter; }
    MaybeMatchPhaseLimiter &match_limiter() { return _match_limiter; }
    bool has_second_phase_rank() const { return !_rankSetup.getSecondPhaseRank().empty(); }
    const search::fef::MatchData &match_data() const { return *_match_data; }
    search::fef::RankProgram &rank_program() { return *_rank_program; }
    SearchIterator &search() { return *_search; }
    SearchIterator::UP borrow_search() { return std::move(_search); }
    void give_back_search(SearchIterator::UP search_in) { _search = std::move(search_in); }
    void tag_search_as_changed() { _search_has_changed = true; }
    void setup_first_phase();
    void setup_second_phase();
    void setup_summary();
    void setup_dump();
};

class MatchToolsFactory : public vespalib::noncopyable
{
private:
    using DiversityFilter = search::attribute::diversity::DiversityFilter;
    using IAttributeContext = search::attribute::IAttributeContext;
    using Properties = search::fef::Properties;
    using MatchDataLayout = search::fef::MatchDataLayout;
    using RankSetup = search::fef::RankSetup;
    using IIndexEnvironment = search::fef::IIndexEnvironment;
    std::unique_ptr<DiversityFilter> _diversityFilter;
    QueryLimiter                   & _queryLimiter;
    RequestContext                   _requestContext;
    const vespalib::Doom             _hardDoom;
    Query                            _query;
    MaybeMatchPhaseLimiter::UP       _match_limiter;
    QueryEnvironment                 _queryEnv;
    MatchDataLayout                  _mdl;
    const RankSetup                & _rankSetup;
    const Properties               & _featureOverrides;
    bool                             _valid;
public:
    typedef std::unique_ptr<MatchToolsFactory> UP;

    MatchToolsFactory(QueryLimiter & queryLimiter, const vespalib::Doom & softDoom, const vespalib::Doom & hardDoom,
                      ISearchContext &searchContext, IAttributeContext &attributeContext,
                      const vespalib::stringref &queryStack, const vespalib::string &location,
                      const ViewResolver &viewResolver, const search::IDocumentMetaStore &metaStore,
                      const IIndexEnvironment &indexEnv, const RankSetup &rankSetup,
                      const Properties &rankProperties, const Properties &featureOverrides);
    ~MatchToolsFactory();
    bool valid() const { return _valid; }
    const MaybeMatchPhaseLimiter &match_limiter() const { return *_match_limiter; }
    DiversityFilter * getDiversityFilter() const { return _diversityFilter.get(); }
    MatchTools::UP createMatchTools() const;
    search::queryeval::Blueprint::HitEstimate estimate() const { return _query.estimate(); }
    bool has_first_phase_rank() const { return !_rankSetup.getFirstPhaseRank().empty(); }
};

}
