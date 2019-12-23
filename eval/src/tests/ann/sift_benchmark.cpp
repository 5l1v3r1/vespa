// Copyright 2019 Oath Inc. Licensed under the terms of the Apache 2.0 license. See LICENSE in the project root.

#include <vespa/vespalib/testkit/test_kit.h>
#include <vespa/vespalib/util/priority_queue.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <chrono>

#define NUM_DIMS 128
#define NUM_DOCS 1000000
#define NUM_Q 1000

#include "doc_vector_access.h"
#include "nns.h"
#include "for-sift-hit.h"
#include "for-sift-top-k.h"

std::vector<TopK> bruteforceResults;
std::vector<float> tmp_v(NUM_DIMS);

struct PointVector {
    float v[NUM_DIMS];
    using ConstArr = vespalib::ConstArrayRef<float>;
    operator ConstArr() const { return ConstArr(v, NUM_DIMS); }
};

static PointVector *generatedQueries =
    (PointVector *) malloc(NUM_Q * sizeof(PointVector));

static PointVector *generatedDocs =
    (PointVector *) malloc(NUM_DOCS * sizeof(PointVector));

struct DocVectorAdapter : public DocVectorAccess<float>
{
    vespalib::ConstArrayRef<float> get(uint32_t docid) const override {
        ASSERT_TRUE(docid < NUM_DOCS);
        return generatedDocs[docid];
    }
};

double computeDistance(const PointVector &query, uint32_t docid) {
    const PointVector &docvector = generatedDocs[docid];
    return l2distCalc.l2sq_dist(query, docvector, tmp_v);
}

void read_queries(std::string fn) {
    int fd = open(fn.c_str(), O_RDONLY);
    ASSERT_TRUE(fd > 0);
    int d;
    size_t rv;
    fprintf(stderr, "reading %u queries from %s\n", NUM_Q, fn.c_str());
    for (uint32_t qid = 0; qid < NUM_Q; ++qid) {
        rv = read(fd, &d, 4);
        ASSERT_EQUAL(rv, 4u);
        ASSERT_EQUAL(d, NUM_DIMS);
        rv = read(fd, &generatedQueries[qid].v, NUM_DIMS*sizeof(float));
        ASSERT_EQUAL(rv, sizeof(PointVector));
    }
    close(fd);
}

void read_docs(std::string fn) {
    int fd = open(fn.c_str(), O_RDONLY);
    ASSERT_TRUE(fd > 0);
    int d;
    size_t rv;
    fprintf(stderr, "reading %u doc vectors from %s\n", NUM_DOCS, fn.c_str());
    for (uint32_t docid = 0; docid < NUM_DOCS; ++docid) {
        rv = read(fd, &d, 4);
        ASSERT_EQUAL(rv, 4u);
        ASSERT_EQUAL(d, NUM_DIMS);
        rv = read(fd, &generatedDocs[docid].v, NUM_DIMS*sizeof(float));
        ASSERT_EQUAL(rv, sizeof(PointVector));
    }
    close(fd);
}

using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

double to_ms(Duration elapsed) {
    std::chrono::duration<double, std::milli> ms(elapsed);
    return ms.count();
}

void read_data(std::string dir) {
    TimePoint bef = std::chrono::steady_clock::now();
    read_queries(dir + "/sift_query.fvecs");
    TimePoint aft = std::chrono::steady_clock::now();
    fprintf(stderr, "read queries: %.3f ms\n", to_ms(aft - bef));
    bef = std::chrono::steady_clock::now();
    read_docs(dir + "/sift_base.fvecs");
    aft = std::chrono::steady_clock::now();
    fprintf(stderr, "read docs: %.3f ms\n", to_ms(aft - bef));
}


struct BfHitComparator {
    bool operator() (const Hit &lhs, const Hit& rhs) const {
        if (lhs.distance < rhs.distance) return false;
        if (lhs.distance > rhs.distance) return true;
        return (lhs.docid > rhs.docid);
    }
};

class BfHitHeap {
private:
    size_t _size;
    vespalib::PriorityQueue<Hit, BfHitComparator> _priQ;
public:
    explicit BfHitHeap(size_t maxSize) : _size(maxSize), _priQ() {
        _priQ.reserve(maxSize);
    }
    ~BfHitHeap() {}
    void maybe_use(const Hit &hit) {
        if (_priQ.size() < _size) {
            _priQ.push(hit);
        } else if (hit.distance < _priQ.front().distance) {
            _priQ.front() = hit;
            _priQ.adjust();
        }
    }
    std::vector<Hit> bestHits() {
        std::vector<Hit> result;
        size_t i = _priQ.size();
        result.resize(i);
        while (i-- > 0) {
            result[i] = _priQ.front();
            _priQ.pop_front();
        }
        return result;
    }
};

TopK bruteforce_nns(const PointVector &query) {
    TopK result;
    BfHitHeap heap(result.K);
    for (uint32_t docid = 0; docid < NUM_DOCS; ++docid) {
        const PointVector &docvector = generatedDocs[docid];
        double d = l2distCalc.l2sq_dist(query, docvector, tmp_v);
        Hit h(docid, d);
        heap.maybe_use(h);
    }
    std::vector<Hit> best = heap.bestHits();
    for (size_t i = 0; i < result.K; ++i) {
        result.hits[i] = best[i];
    }
    return result;
}

void verifyBF(uint32_t qid) {
    const PointVector &query = generatedQueries[qid];
    TopK &result = bruteforceResults[qid];
    double min_distance = result.hits[0].distance;
    std::vector<double> all_c2;
    for (uint32_t i = 0; i < NUM_DOCS; ++i) {
        double dist = computeDistance(query, i);
        if (dist < min_distance) {
            fprintf(stderr, "WARN dist %.9g < mindist %.9g\n", dist, min_distance);
        }
        EXPECT_FALSE(dist+0.000001 < min_distance);
        if (qid == 6) all_c2.push_back(dist / min_distance);
    }
    if (all_c2.size() != NUM_DOCS) return;
    std::sort(all_c2.begin(), all_c2.end());
    for (uint32_t idx : { 1, 3, 10, 30, 100, 300, 1000, 3000, NUM_DOCS/2, NUM_DOCS-1}) {
        fprintf(stderr, "c2-factor[%u] = %.3f\n", idx, all_c2[idx]);
    }
}

TEST("require that brute force works") {
    TimePoint bef = std::chrono::steady_clock::now();
    bruteforceResults.reserve(NUM_Q);
    for (uint32_t cnt = 0; cnt < NUM_Q; ++cnt) {
        const PointVector &query = generatedQueries[cnt];
        bruteforceResults.emplace_back(bruteforce_nns(query));
    }
    TimePoint aft = std::chrono::steady_clock::now();
    fprintf(stderr, "timing for brute force: %.3f ms = %.3f ms per query\n",
            to_ms(aft - bef), to_ms(aft - bef)/NUM_Q);
    for (int cnt = 0; cnt < NUM_Q; cnt = (cnt+1)*2) {
        verifyBF(cnt);
    }
}

using NNS_API = NNS<float>;

TopK find_with_nns(uint32_t sk, NNS_API &nns, uint32_t qid) {
    TopK result;
    const PointVector &qv = generatedQueries[qid];
    vespalib::ConstArrayRef<float> query(qv.v, NUM_DIMS);
    auto rv = nns.topK(result.K, query, sk);
    for (size_t i = 0; i < result.K; ++i) {
        result.hits[i] = Hit(rv[i].docid, rv[i].sqDistance);
    }
    return result;
}

void verify_nns_quality(uint32_t sk, NNS_API &nns, uint32_t qid) {
    TopK perfect = bruteforceResults[qid];
    TopK result = find_with_nns(sk, nns, qid);
    int recall = perfect.recall(result);
    EXPECT_TRUE(recall > 40);
    double sum_error = 0.0;
    double c_factor = 1.0;
    for (size_t i = 0; i < result.K; ++i) {
        double factor = (result.hits[i].distance / perfect.hits[i].distance);
        sum_error += factor;
        c_factor = std::max(c_factor, factor);
    }
    EXPECT_TRUE(c_factor < 1.5);
    fprintf(stderr, "quality sk=%u: query %u: recall %d, c2-factor %.3f, avg c2: %.3f\n",
            sk, qid, recall, c_factor, sum_error / result.K);
    if (qid == 6) {
        for (size_t i = 0; i < 10; ++i) {
            fprintf(stderr, "topk[%zu] BF{%u %.3f} index{%u %.3f}\n",
                    i,
                    perfect.hits[i].docid, perfect.hits[i].distance,
                    result.hits[i].docid, result.hits[i].distance);
        }
    }
}

#if 0
TEST("require that Locality Sensitive Hashing mostly works") {
    TimePoint bef = std::chrono::steady_clock::now();
    DocVectorAdapter adapter;
    std::unique_ptr<NNS_API> nns = make_rplsh_nns(NUM_DIMS, adapter);
    for (uint32_t i = 0; i < NUM_DOCS; ++i) {
        nns->addDoc(i);
    }
    fprintf(stderr, "added %u documents...\n", NUM_DOCS);
    TimePoint aft = std::chrono::steady_clock::now();
    fprintf(stderr, "build RPLSH index: %.3f ms\n", to_ms(aft - bef));

    for (uint32_t search_k : { 200, 1000 }) {
        bef = std::chrono::steady_clock::now();
        for (int cnt = 0; cnt < NUM_Q; ++cnt) {
            find_with_nns(search_k, *nns, cnt);
        }
        aft = std::chrono::steady_clock::now();
        fprintf(stderr, "timing for RPLSH search_k=%u: %.3f ms = %.3f ms per query\n",
                search_k, to_ms(aft - bef), to_ms(aft - bef)/NUM_Q);
        for (int cnt = 0; cnt < NUM_Q; ++cnt) {
            verify_nns_quality(search_k, *nns, cnt);
        }
    }
}
#endif

TEST("require that Indexing via NNS api mostly works") {
    fprintf(stderr, "trying indexing...\n");
    TimePoint bef = std::chrono::steady_clock::now();
    DocVectorAdapter adapter;
    std::unique_ptr<NNS_API> nns = make_annoy_nns(NUM_DIMS, adapter);
    for (uint32_t i = 0; i < NUM_DOCS; ++i) {
        nns->addDoc(i);
    }
    fprintf(stderr, "added %u documents...\n", NUM_DOCS);
    nns->topK(1, adapter.get(0), 1);
    TimePoint aft = std::chrono::steady_clock::now();
    fprintf(stderr, "build annoy index: %.3f ms\n", to_ms(aft - bef));

    for (uint32_t search_k : { 10000, 20000 }) {
        bef = std::chrono::steady_clock::now();
        for (int cnt = 0; cnt < NUM_Q; ++cnt) {
            find_with_nns(search_k, *nns, cnt);
        }
        aft = std::chrono::steady_clock::now();
        fprintf(stderr, "timing for index search_k=%u: %.3f ms = %.3f ms per query\n",
                search_k, to_ms(aft - bef), to_ms(aft - bef)/NUM_Q);
        for (int cnt = 0; cnt < NUM_Q; ++cnt) {
            verify_nns_quality(search_k, *nns, cnt);
        }
    }
}

int main(int argc, char **argv) {
    TEST_MASTER.init(__FILE__);
    std::string sift_dir = ".";
    if (argc > 1) {
        sift_dir = argv[1];
    } else {
        char *home = getenv("HOME");
        if (home) {
            sift_dir = home;
            sift_dir += "/sift";
        }
    }
    read_data(sift_dir);
    TEST_RUN_ALL();
    return (TEST_MASTER.fini() ? 0 : 1);
}
