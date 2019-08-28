// Copyright 2017 Yahoo Holdings. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
package com.yahoo.vespa.config.server.tenant;

import com.yahoo.cloud.config.ConfigserverConfig;
import com.yahoo.component.Version;
import com.yahoo.config.ConfigInstance;
import com.yahoo.config.SimpletypesConfig;
import com.yahoo.config.application.api.ApplicationPackage;
import com.yahoo.config.model.NullConfigModelRegistry;
import com.yahoo.config.model.application.provider.BaseDeployLogger;
import com.yahoo.config.model.application.provider.DeployData;
import com.yahoo.config.model.application.provider.FilesApplicationPackage;
import com.yahoo.config.model.application.provider.MockFileRegistry;
import com.yahoo.config.provision.AllocatedHosts;
import com.yahoo.config.provision.ApplicationId;
import com.yahoo.config.provision.ApplicationName;
import com.yahoo.config.provision.TenantName;
import com.yahoo.io.IOUtils;
import com.yahoo.vespa.config.ConfigKey;
import com.yahoo.vespa.config.ConfigPayload;
import com.yahoo.vespa.config.GetConfigRequest;
import com.yahoo.vespa.config.protocol.ConfigResponse;
import com.yahoo.vespa.config.protocol.DefContent;
import com.yahoo.vespa.config.protocol.VespaVersion;
import com.yahoo.vespa.config.server.ReloadListener;
import com.yahoo.vespa.config.server.ServerCache;
import com.yahoo.vespa.config.server.TestComponentRegistry;
import com.yahoo.vespa.config.server.application.Application;
import com.yahoo.vespa.config.server.application.ApplicationSet;
import com.yahoo.vespa.config.server.deploy.ZooKeeperDeployer;
import com.yahoo.vespa.config.server.model.TestModelFactory;
import com.yahoo.vespa.config.server.modelfactory.ModelFactoryRegistry;
import com.yahoo.vespa.config.server.monitoring.MetricUpdater;
import com.yahoo.vespa.config.server.session.RemoteSession;
import com.yahoo.vespa.config.server.session.SessionZooKeeperClient;
import com.yahoo.vespa.curator.Curator;
import com.yahoo.vespa.curator.mock.MockCurator;
import com.yahoo.vespa.model.VespaModel;
import com.yahoo.vespa.model.VespaModelFactory;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;
import org.junit.rules.TemporaryFolder;
import org.xml.sax.SAXException;

import java.io.File;
import java.io.IOException;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.atomic.AtomicInteger;

import static com.yahoo.cloud.config.ConfigserverConfig.PayloadCompressionType.Enum.UNCOMPRESSED;
import static org.hamcrest.core.Is.is;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertThat;
import static org.junit.Assert.assertTrue;

/**
 * @author Ulf Lilleengen
 */
public class TenantRequestHandlerTest {

    private static final Version vespaVersion = new VespaModelFactory(new NullConfigModelRegistry()).version();
    private TenantRequestHandler server;
    private MockReloadListener listener = new MockReloadListener();
    private File app1 = new File("src/test/apps/cs1");
    private File app2 = new File("src/test/apps/cs2");
    private TenantName tenant = TenantName.from("mytenant");
    private TestComponentRegistry componentRegistry;
    private Curator curator;

    @Rule
    public TemporaryFolder tempFolder = new TemporaryFolder();

    private ApplicationId defaultApp() {
        return new ApplicationId.Builder().applicationName(ApplicationName.defaultName()).tenant(tenant).build();
    }

    @Before
    public void setUp() throws IOException {
        curator = new MockCurator();

        feedApp(app1, 1, defaultApp(), false);
        componentRegistry = new TestComponentRegistry.Builder()
                .curator(curator)
                .configServerConfig(new ConfigserverConfig(new ConfigserverConfig.Builder()
                                                                   .payloadCompressionType(UNCOMPRESSED)
                                                                   .configDefinitionsDir(tempFolder.newFolder().getAbsolutePath())
                                                                   .configServerDBDir(tempFolder.newFolder().getAbsolutePath())))
                .modelFactoryRegistry(createRegistry())
                .build();
        server = new TenantRequestHandler(componentRegistry.getMetrics(), tenant, List.of(listener), componentRegistry);
    }

    private void feedApp(File appDir, long sessionId, ApplicationId appId, boolean  internalRedeploy) throws IOException {
        SessionZooKeeperClient zkc = new SessionZooKeeperClient(curator, TenantRepository.getSessionsPath(tenant).append(String.valueOf(sessionId)));
        zkc.writeApplicationId(appId);
        File app = tempFolder.newFolder();
        IOUtils.copyDirectory(appDir, app);
        ZooKeeperDeployer deployer = zkc.createDeployer(new BaseDeployLogger());
        DeployData deployData = new DeployData("user",
                                               appDir.toString(),
                                               appId.application().toString(),
                                               0L,
                                               internalRedeploy,
                                               0L,
                                               0L);
        ApplicationPackage appPackage = FilesApplicationPackage.fromFileWithDeployData(appDir, deployData);
        deployer.deploy(appPackage,
                        Collections.singletonMap(vespaVersion, new MockFileRegistry()),
                        AllocatedHosts.withHosts(Collections.emptySet()));
    }

    private ApplicationSet reloadConfig(long sessionId) {
        return reloadConfig(sessionId, "default");
    }

    private ApplicationSet reloadConfig(long sessionId, String application) {
        SessionZooKeeperClient zkc = new SessionZooKeeperClient(curator, TenantRepository.getSessionsPath(tenant).append(String.valueOf(sessionId)));
        zkc.writeApplicationId(new ApplicationId.Builder().tenant(tenant).applicationName(application).build());
        RemoteSession session = new RemoteSession(tenant, sessionId, componentRegistry, zkc);
        return session.ensureApplicationLoaded();
    }

    private ModelFactoryRegistry createRegistry() {
        return new ModelFactoryRegistry(Arrays.asList(new TestModelFactory(vespaVersion),
                new TestModelFactory(new Version(3, 2, 1))));
    }

    @SuppressWarnings("unchecked")
    private <T extends ConfigInstance> T resolve(TenantRequestHandler tenantRequestHandler,
                                                 ApplicationId appId,
                                                 Version vespaVersion) {
        ConfigResponse response = getConfigResponse(tenantRequestHandler, appId, vespaVersion);
        return ConfigPayload.fromUtf8Array(response.getPayload()).toInstance((Class<T>) SimpletypesConfig.class, "");
    }

    private <T extends ConfigInstance> ConfigResponse getConfigResponse(TenantRequestHandler tenantRequestHandler,
                                                                        ApplicationId appId,
                                                                        Version vespaVersion) {
        return tenantRequestHandler.resolveConfig(appId, new GetConfigRequest() {
            @SuppressWarnings("unchecked")
            @Override
            public ConfigKey<T> getConfigKey() {
                return new ConfigKey<>((Class<T>) SimpletypesConfig.class, "");
            }

            @Override
            public DefContent getDefContent() {
                return DefContent.fromClass(SimpletypesConfig.class);
            }

            @Override
            public Optional<VespaVersion> getVespaVersion() {
                return Optional.of(VespaVersion.fromString(vespaVersion.toFullString()));
            }

            @Override
            public boolean noCache() {
                return false;
            }
        }, Optional.empty());
    }

    @Test
    public void testReloadConfig() throws IOException {
        ApplicationId applicationId = new ApplicationId.Builder().applicationName(ApplicationName.defaultName()).tenant(tenant).build();

        server.applications().createApplication(applicationId);
        server.applications().createPutTransaction(applicationId, 1).commit();
        server.reloadConfig(reloadConfig(1));
        assertThat(listener.reloaded.get(), is(1));
        // Using only payload list for this simple test
        SimpletypesConfig config = resolve(server, defaultApp(), vespaVersion);
        assertThat(config.intval(), is(1337));
        assertThat(server.getApplicationGeneration(applicationId, Optional.of(vespaVersion)), is(1L));

        server.reloadConfig(reloadConfig(1L));
        ConfigResponse configResponse = getConfigResponse(server, defaultApp(), vespaVersion);
        assertFalse(configResponse.isInternalRedeploy());
        config = resolve(server, defaultApp(), vespaVersion);
        assertThat(config.intval(), is(1337));
        assertThat(listener.reloaded.get(), is(2));
        assertThat(server.getApplicationGeneration(applicationId, Optional.of(vespaVersion)), is(1L));
        assertThat(listener.tenantHosts.size(), is(1));
        assertThat(server.resolveApplicationId("mytesthost"), is(applicationId));

        listener.reloaded.set(0);
        feedApp(app2, 2, defaultApp(), true);
        server.applications().createPutTransaction(applicationId, 2).commit();
        server.reloadConfig(reloadConfig(2L));
        configResponse = getConfigResponse(server, defaultApp(), vespaVersion);
        assertTrue(configResponse.isInternalRedeploy());
        config = resolve(server, defaultApp(), vespaVersion);
        assertThat(config.intval(), is(1330));
        assertThat(listener.reloaded.get(), is(1));
        assertThat(server.getApplicationGeneration(applicationId, Optional.of(vespaVersion)), is(2L));
    }

    @Test
    public void testRemoveApplication() {
        ApplicationId appId = ApplicationId.from(tenant.value(), "default", "default");
        server.reloadConfig(reloadConfig(1));
        assertThat(listener.reloaded.get(), is(0));

        server.applications().createApplication(appId);
        server.applications().createPutTransaction(appId, 1).commit();
        server.reloadConfig(reloadConfig(1));
        assertThat(listener.reloaded.get(), is(1));

        assertThat(listener.removed.get(), is(0));

        server.removeApplication(appId);
        assertThat(listener.removed.get(), is(0));

        server.applications().createDeleteTransaction(appId).commit();
        server.removeApplication(appId);
        assertThat(listener.removed.get(), is(1));
    }

    @Test
    public void testResolveForAppId() {
        long id = 1L;
        ApplicationId appId = new ApplicationId.Builder()
                              .tenant(tenant)
                              .applicationName("myapp").instanceName("myinst").build();
        server.applications().createApplication(appId);
        server.applications().createPutTransaction(appId, 1).commit();
        SessionZooKeeperClient zkc = new SessionZooKeeperClient(curator, TenantRepository.getSessionsPath(tenant).append(String.valueOf(id)));
        zkc.writeApplicationId(appId);
        RemoteSession session = new RemoteSession(appId.tenant(), id, componentRegistry, zkc);
        server.reloadConfig(session.ensureApplicationLoaded());
        SimpletypesConfig config = resolve(server, appId, vespaVersion);
        assertThat(config.intval(), is(1337));
    }

    @Test
    public void testResolveMultipleApps() throws IOException {
        ApplicationId appId1 = new ApplicationId.Builder()
                              .tenant(tenant)
                              .applicationName("myapp1").instanceName("myinst1").build();
        ApplicationId appId2 = new ApplicationId.Builder()
                              .tenant(tenant)
                              .applicationName("myapp2").instanceName("myinst2").build();
        feedAndReloadApp(app1, 1, appId1);
        SimpletypesConfig config = resolve(server, appId1, vespaVersion);
        assertThat(config.intval(), is(1337));

        feedAndReloadApp(app2, 2, appId2);
        config = resolve(server, appId2, vespaVersion);
        assertThat(config.intval(), is(1330));
    }

    @Test
    public void testResolveMultipleVersions() throws IOException {
        ApplicationId appId = new ApplicationId.Builder()
                              .tenant(tenant)
                              .applicationName("myapp1").instanceName("myinst1").build();
        feedAndReloadApp(app1, 1, appId);
        SimpletypesConfig config = resolve(server, appId, vespaVersion);
        assertThat(config.intval(), is(1337));
        config = resolve(server, appId, new Version(3, 2, 1));
        assertThat(config.intval(), is(1337));
    }

    private void feedAndReloadApp(File appDir, long sessionId, ApplicationId appId) throws IOException {
        server.applications().createApplication(appId);
        server.applications().createPutTransaction(appId, sessionId).commit();
        feedApp(appDir, sessionId, appId, false);
        SessionZooKeeperClient zkc = new SessionZooKeeperClient(curator, TenantRepository.getSessionsPath(tenant).append(String.valueOf(sessionId)));
        zkc.writeApplicationId(appId);
        RemoteSession session = new RemoteSession(tenant, sessionId, componentRegistry, zkc);
        server.reloadConfig(session.ensureApplicationLoaded());
    }

    public static class MockReloadListener implements ReloadListener {
        AtomicInteger reloaded = new AtomicInteger(0);
        AtomicInteger removed = new AtomicInteger(0);
        Map<String, Collection<String>> tenantHosts = new LinkedHashMap<>();

        @Override
        public void configActivated(ApplicationSet application) {
            reloaded.incrementAndGet();
        }

        @Override
        public void hostsUpdated(TenantName tenant, Collection<String> newHosts) {
            tenantHosts.put(tenant.value(), newHosts);
        }

        @Override
        public void verifyHostsAreAvailable(TenantName tenant, Collection<String> newHosts) {
        }

        @Override
        public void applicationRemoved(ApplicationId applicationId) {
            removed.incrementAndGet();
        }
    }

    @Test
    public void testHasApplication() {
        assertdefaultAppNotFound();
        ApplicationId appId = ApplicationId.from(tenant.value(), "default", "default");
        server.applications().createApplication(appId);
        server.applications().createPutTransaction(appId, 1).commit();
        server.reloadConfig(reloadConfig(1));
        assertTrue(server.hasApplication(appId, Optional.of(vespaVersion)));
    }

    private void assertdefaultAppNotFound() {
        assertFalse(server.hasApplication(ApplicationId.defaultId(), Optional.of(vespaVersion)));
    }

    @Test
    public void testMultipleApplicationsReload() {
        ApplicationId appId = ApplicationId.from(tenant.value(), "foo", "default");
        assertdefaultAppNotFound();
        server.applications().createApplication(appId);
        server.applications().createPutTransaction(appId, 1).commit();
        server.reloadConfig(reloadConfig(1, "foo"));
        assertdefaultAppNotFound();
        assertTrue(server.hasApplication(appId,
                                         Optional.of(vespaVersion)));
        assertThat(server.resolveApplicationId("doesnotexist"), is(ApplicationId.defaultId()));
        assertThat(server.resolveApplicationId("mytesthost"), is(new ApplicationId.Builder()
                                                                 .tenant(tenant)
                                                                 .applicationName("foo").build())); // Host set in application package.
    }

    @Test
    public void testListConfigs() throws IOException, SAXException {
        assertdefaultAppNotFound();

        VespaModel model = new VespaModel(FilesApplicationPackage.fromFile(new File("src/test/apps/app")));
        server.applications().createApplication(ApplicationId.defaultId());
        server.applications().createPutTransaction(ApplicationId.defaultId(), 1).commit();
        server.reloadConfig(ApplicationSet.fromSingle(new Application(model,
                                                                      new ServerCache(),
                                                                      1,
                                                                      false,
                                                                      vespaVersion,
                                                                      MetricUpdater.createTestUpdater(),
                                                                      ApplicationId.defaultId())));
        Set<ConfigKey<?>> configNames = server.listConfigs(ApplicationId.defaultId(), Optional.of(vespaVersion), false);
        assertTrue(configNames.contains(new ConfigKey<>("sentinel", "hosts", "cloud.config")));

        configNames = server.listConfigs(ApplicationId.defaultId(), Optional.of(vespaVersion), true);
        System.out.println(configNames);
        assertTrue(configNames.contains(new ConfigKey<>("documentmanager", "container", "document.config")));
        assertTrue(configNames.contains(new ConfigKey<>("documentmanager", "", "document.config")));
        assertTrue(configNames.contains(new ConfigKey<>("documenttypes", "", "document")));
        assertTrue(configNames.contains(new ConfigKey<>("documentmanager", "container", "document.config")));
        assertTrue(configNames.contains(new ConfigKey<>("health-monitor", "container", "container.jdisc.config")));
        assertTrue(configNames.contains(new ConfigKey<>("specific", "container", "project")));
    }

    @Test
    public void testAppendIdsInNonRecursiveListing() {
        assertEquals(server.appendOneLevelOfId("search/music", "search/music/qrservers/default/qr.0"), "search/music/qrservers");
        assertEquals(server.appendOneLevelOfId("search", "search/music/qrservers/default/qr.0"), "search/music");
        assertEquals(server.appendOneLevelOfId("search/music/qrservers/default/qr.0", "search/music/qrservers/default/qr.0"), "search/music/qrservers/default/qr.0");
        assertEquals(server.appendOneLevelOfId("", "search/music/qrservers/default/qr.0"), "search");
    }
}
