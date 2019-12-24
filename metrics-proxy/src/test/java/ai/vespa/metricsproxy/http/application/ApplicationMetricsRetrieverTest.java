/*
 * Copyright 2019 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.
 */

package ai.vespa.metricsproxy.http.application;

import com.github.tomakehurst.wiremock.junit.WireMockRule;
import org.junit.Before;
import org.junit.Rule;
import org.junit.Test;

import java.time.Duration;
import java.util.Arrays;
import java.util.concurrent.TimeoutException;

import static ai.vespa.metricsproxy.TestUtil.getFileContents;
import static com.github.tomakehurst.wiremock.client.WireMock.aResponse;
import static com.github.tomakehurst.wiremock.client.WireMock.get;
import static com.github.tomakehurst.wiremock.client.WireMock.urlPathEqualTo;
import static com.github.tomakehurst.wiremock.core.WireMockConfiguration.options;
import static java.util.stream.Collectors.toList;
import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

/**
 * @author gjoranv
 */
public class ApplicationMetricsRetrieverTest {

    private static final String TEST_FILE = "generic-sample.json";
    private static final String RESPONSE = getFileContents(TEST_FILE);
    private static final String HOST = "localhost";

    private static int port;

    @Rule
    public WireMockRule wireMockRule = new WireMockRule(options().dynamicPort(), false);

    @Before
    public void setup() {
        port = wireMockRule.port();
    }

    @Test
    public void metrics_can_be_retrieved() {
        var config = nodesConfig("/node0");
        Node node = new Node(config.node(0));

        verifyRetrievingMetricsFromSingleNode(config, node);
    }

    private void verifyRetrievingMetricsFromSingleNode(VespaNodesConfig config, Node node) {
        wireMockRule.stubFor(get(urlPathEqualTo(config.node(0).metricsPath()))
                                     .willReturn(aResponse().withBody(RESPONSE)));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        var metricsByNode = retriever.getMetrics();
        assertEquals(1, metricsByNode.size());
        assertEquals(4, metricsByNode.get(node).size());
    }

    @Test
    public void metrics_can_be_retrieved_from_multiple_nodes() {
        var config = nodesConfig("/node0", "/node1");
        Node node0 = new Node(config.node(0));
        Node node1 = new Node(config.node(1));

        wireMockRule.stubFor(get(urlPathEqualTo(config.node(0).metricsPath()))
                                     .willReturn(aResponse().withBody(RESPONSE)));
        wireMockRule.stubFor(get(urlPathEqualTo(config.node(1).metricsPath()))
                                     .willReturn(aResponse().withBody(RESPONSE)));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        var metricsByNode = retriever.getMetrics();
        assertEquals(2, metricsByNode.size());
        assertEquals(4, metricsByNode.get(node0).size());
        assertEquals(4, metricsByNode.get(node1).size());
    }

    @Test
    public void dead_node_yields_empty_metrics() {
        var config = nodesConfig("/non-existent");
        Node node = new Node(config.node(0));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        var metricsByNode = retriever.getMetrics();
        assertEquals(1, metricsByNode.size());
        assertEquals(0, metricsByNode.get(node).size());

    }

    @Test
    public void metrics_from_good_node_are_returned_even_if_another_node_is_dead() {
        var config = nodesConfig("/node0", "/node1");
        Node node0 = new Node(config.node(0));
        Node node1 = new Node(config.node(1));

        wireMockRule.stubFor(get(urlPathEqualTo(config.node(1).metricsPath()))
                                     .willReturn(aResponse().withBody(RESPONSE)));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        var metricsByNode = retriever.getMetrics();
        assertEquals(2, metricsByNode.size());
        assertEquals(0, metricsByNode.get(node0).size());
        assertEquals(4, metricsByNode.get(node1).size());
    }

    @Test
    public void an_exception_is_thrown_when_retrieving_times_out() {
        var config = nodesConfig("/node0");

        wireMockRule.stubFor(get(urlPathEqualTo(config.node(0).metricsPath()))
                                     .willReturn(aResponse()
                                                         .withBody(RESPONSE)
                                                         .withFixedDelay(10)));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        retriever.setTaskTimeout(Duration.ofMillis(1));

        try {
            retriever.getMetrics();
            fail("Did not get expected exception");
        } catch (ApplicationMetricsException expected) {
            assertTrue(expected.getCause() instanceof TimeoutException);
        }
    }

    @Test
    public void metrics_can_be_retrieved_after_previous_call_threw_an_exception() {
        var config = nodesConfig("/node0");
        Node node = new Node(config.node(0));

        var delayedStub = wireMockRule.stubFor(get(urlPathEqualTo(config.node(0).metricsPath()))
                                                       .willReturn(aResponse()
                                                                           .withBody(RESPONSE)
                                                                           .withFixedDelay(10)));

        ApplicationMetricsRetriever retriever = new ApplicationMetricsRetriever(config);
        retriever.setTaskTimeout(Duration.ofMillis(1));
        try {
            retriever.getMetrics();
            fail("Did not get expected exception");
        } catch (ApplicationMetricsException expected) {
        }

        // Verify successful retrieving
        wireMockRule.removeStubMapping(delayedStub);
        verifyRetrievingMetricsFromSingleNode(config, node);
    }

    private VespaNodesConfig nodesConfig(String... paths) {
        var nodes = Arrays.stream(paths)
                .map(this::nodeConfig)
                .collect(toList());
        return new VespaNodesConfig.Builder()
                .node(nodes)
                .build();
    }

    private VespaNodesConfig.Node.Builder nodeConfig(String path) {
        return new VespaNodesConfig.Node.Builder()
                .configId(path)
                .hostname(HOST)
                .metricsPath(path)
                .metricsPort(port);
    }

}
