// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include "verify_ranksetup.h"
#include <vespa/config-attributes.h>
#include <vespa/config-imported-fields.h>
#include <vespa/config-indexschema.h>
#include <vespa/config-rank-profiles.h>
#include <vespa/config/config.h>
#include <vespa/config/helper/legacy.h>
#include <vespa/config/common/exceptions.h>
#include <vespa/eval/eval/tensor_spec.h>
#include <vespa/eval/eval/value_cache/constant_value.h>
#include <vespa/eval/tensor/default_tensor_engine.h>
#include <vespa/searchcommon/common/schemaconfigurer.h>
#include <vespa/searchcore/config/config-ranking-constants.h>
#include <vespa/searchcore/proton/matching/indexenvironment.h>
#include <vespa/searchlib/features/setup.h>
#include <vespa/searchlib/fef/fef.h>
#include <vespa/searchlib/fef/test/plugin/setup.h>

#include <vespa/log/log.h>
#include <vespa/vespalib/util/stringfmt.h>
#include <vespa/vespalib/stllike/asciistream.h>

LOG_SETUP(".verify_ranksetup");

using config::ConfigContext;
using config::ConfigHandle;
using config::ConfigRuntimeException;
using config::ConfigSubscriber;
using config::IConfigContext;
using config::InvalidConfigException;
using proton::matching::IConstantValueRepo;
using vespa::config::search::AttributesConfig;
using vespa::config::search::IndexschemaConfig;
using vespa::config::search::RankProfilesConfig;
using vespa::config::search::core::RankingConstantsConfig;
using vespalib::eval::ConstantValue;
using vespalib::eval::TensorSpec;
using vespalib::eval::ValueType;
using vespalib::tensor::DefaultTensorEngine;
using vespalib::eval::SimpleConstantValue;
using vespalib::eval::BadConstantValue;

namespace {

class VerifyRankSetup
{
private:
    std::vector<vespalib::string> _errors;
    bool verify(const search::index::Schema &schema,
                const search::fef::Properties &props,
                const IConstantValueRepo &repo);

    bool verifyConfig(const RankProfilesConfig &rankCfg,
                      const IndexschemaConfig &schemaCfg,
                      const AttributesConfig &attributeCfg,
                      const RankingConstantsConfig &constantsCfg);

public:
    bool verify(const std::string & configId);
    const std::vector<vespalib::string> & getMessages() const { return _errors; }
};

struct DummyConstantValueRepo : IConstantValueRepo {
    const RankingConstantsConfig &cfg;
    DummyConstantValueRepo(const RankingConstantsConfig &cfg_in) : cfg(cfg_in) {}
    vespalib::eval::ConstantValue::UP getConstant(const vespalib::string &name) const override {
        for (const auto &entry: cfg.constant) {
            if (entry.name == name) {
                const auto &engine = DefaultTensorEngine::ref();
                try {
                    auto tensor = engine.from_spec(TensorSpec(entry.type));
                    return std::make_unique<SimpleConstantValue>(std::move(tensor));
                } catch (std::exception &) {
                    return std::make_unique<BadConstantValue>();
                }
            }
        }
        return vespalib::eval::ConstantValue::UP(nullptr);
    }
};

bool
VerifyRankSetup::verify(const search::index::Schema &schema,
                        const search::fef::Properties &props,
                        const IConstantValueRepo &repo)
{
    proton::matching::IndexEnvironment indexEnv(schema, props, repo);
    search::fef::BlueprintFactory factory;
    search::features::setup_search_features(factory);
    search::fef::test::setup_fef_test_plugin(factory);

    search::fef::RankSetup rankSetup(factory, indexEnv);
    rankSetup.configure(); // reads config values from the property map
    bool ok = true;
    if (!rankSetup.getFirstPhaseRank().empty()) {
        ok = verifyFeature(factory, indexEnv, rankSetup.getFirstPhaseRank(), "first phase ranking", _errors) && ok;
    }
    if (!rankSetup.getSecondPhaseRank().empty()) {
        ok = verifyFeature(factory, indexEnv, rankSetup.getSecondPhaseRank(), "second phase ranking", _errors) && ok;
    }
    for (size_t i = 0; i < rankSetup.getSummaryFeatures().size(); ++i) {
        ok = verifyFeature(factory, indexEnv, rankSetup.getSummaryFeatures()[i], "summary features", _errors) && ok;
    }
    for (size_t i = 0; i < rankSetup.getDumpFeatures().size(); ++i) {
        ok = verifyFeature(factory, indexEnv, rankSetup.getDumpFeatures()[i], "dump features", _errors) && ok;
    }
    return ok;
}

bool
VerifyRankSetup::verifyConfig(const RankProfilesConfig &rankCfg, const IndexschemaConfig &schemaCfg,
                              const AttributesConfig &attributeCfg, const RankingConstantsConfig &constantsCfg)
{
    bool ok = true;
    search::index::Schema schema;
    search::index::SchemaBuilder::build(schemaCfg, schema);
    search::index::SchemaBuilder::build(attributeCfg, schema);
    DummyConstantValueRepo repo(constantsCfg);
    for(size_t i = 0; i < rankCfg.rankprofile.size(); i++) {
        search::fef::Properties properties;
        const RankProfilesConfig::Rankprofile &profile = rankCfg.rankprofile[i];
        for(size_t j = 0; j < profile.fef.property.size(); j++) {
            properties.add(profile.fef.property[j].name,
                           profile.fef.property[j].value);
        }
        if (verify(schema, properties, repo)) {
            vespalib::string msg = vespalib::make_string("rank profile '%s': pass", profile.name.c_str());
            LOG(info, "%s", msg.c_str());
            _errors.emplace_back(msg);
        } else {
            vespalib::string msg = vespalib::make_string("rank profile '%s': FAIL", profile.name.c_str());
            LOG(error, "%s", msg.c_str());
            _errors.emplace_back(msg);
            ok = false;
        }
    }
    return ok;
}

bool
VerifyRankSetup::verify(const std::string & configid)
{
    LOG(debug, "verifying rank setup for config id '%s' ...", configid.c_str());

    bool ok = false;
    try {
        IConfigContext::SP ctx(new ConfigContext(*config::legacyConfigId2Spec(configid)));
        vespalib::string cfgId(config::legacyConfigId2ConfigId(configid));
        ConfigSubscriber subscriber(ctx);
        ConfigHandle<RankProfilesConfig>::UP rankHandle = subscriber.subscribe<RankProfilesConfig>(cfgId);
        ConfigHandle<AttributesConfig>::UP attributesHandle = subscriber.subscribe<AttributesConfig>(cfgId);
        ConfigHandle<IndexschemaConfig>::UP schemaHandle = subscriber.subscribe<IndexschemaConfig>(cfgId);
        ConfigHandle<RankingConstantsConfig>::UP constantsHandle = subscriber.subscribe<RankingConstantsConfig>(cfgId);

        subscriber.nextConfig();
        ok = verifyConfig(*rankHandle->getConfig(), *schemaHandle->getConfig(),
                          *attributesHandle->getConfig(), *constantsHandle->getConfig());
    } catch (ConfigRuntimeException & e) {
        vespalib::string msg = vespalib::make_string("Unable to subscribe to config: %s", e.getMessage().c_str());
        LOG(error, "%s", msg.c_str());
        _errors.emplace_back(msg);
    } catch (InvalidConfigException & e) {
        vespalib::string msg = vespalib::make_string("Error getting config: %s", e.getMessage().c_str());
        LOG(error, "%s", msg.c_str());
        _errors.emplace_back(msg);
    }
    return ok;
}

}

bool
verifyRankSetup(const char * configId, std::string & messages) {
    VerifyRankSetup verifier;
    bool ok = verifier.verify(configId);
    vespalib::asciistream os;
    for (const auto & m : verifier.getMessages()) {
        os << m << "\n";
    }
    messages = os.str();
    return ok;
}