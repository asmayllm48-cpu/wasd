#include <doctest/doctest.h>

#include <thread>

#include "core/bounded_queue.h"
#include "core/stats.h"

using namespace wasd;

TEST_CASE("nearest-rank percentiles") {
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(i);
    auto p = computePercentiles(v);
    CHECK(p.count == 100);
    CHECK(p.p50 == 50);
    CHECK(p.p95 == 95);
    CHECK(p.p99 == 99);
    CHECK(p.max == 100);
    CHECK(p.mean == doctest::Approx(50.5));
    CHECK(computePercentiles({}).count == 0);
}

TEST_CASE("sample window is bounded") {
    SampleWindow w(10);
    for (int i = 0; i < 25; ++i) w.add(i);
    auto s = w.snapshot();
    CHECK(s.size() == 10);
    CHECK(w.summary().max == 24);
    CHECK(w.summary().p50 >= 15);
}

TEST_CASE("rate counter counts the last second") {
    RateCounter r;
    for (int i = 0; i < 120; ++i) r.tick(i * 16667);
    CHECK(r.rate(119 * 16667) == doctest::Approx(60).epsilon(0.05));
}

TEST_CASE("latest slot drops older items") {
    LatestSlot<int> s;
    CHECK_FALSE(s.put(1).has_value());
    CHECK(s.put(2).value() == 1);
    CHECK(s.dropped() == 1);
    CHECK(s.take().value() == 2);
    CHECK_FALSE(s.take().has_value());
}

struct Msg {
    int pointer;
    bool move;
};

TEST_CASE("control queue coalesces moves and never drops non-droppable items") {
    ControlQueue<Msg> q(4);
    auto coalesce = [](const Msg& a, const Msg& b) { return a.move && b.move && a.pointer == b.pointer; };
    auto droppable = [](const Msg& m) { return m.move; };
    CHECK(q.push({1, false}, coalesce, droppable));
    for (int i = 0; i < 10; ++i) CHECK(q.push({1, true}, coalesce, droppable));
    CHECK(q.size() == 2);
    CHECK(q.coalesced() == 9);
    q.push({2, true}, coalesce, droppable);
    q.push({3, true}, coalesce, droppable);
    CHECK(q.size() == 4);
    CHECK_FALSE(q.push({4, true}, coalesce, droppable));  // full: droppable rejected
    CHECK(q.push({4, false}, coalesce, droppable));       // full: evicts a move
    CHECK(q.size() == 4);
    CHECK(q.dropped() == 2);
    int downs = 0;
    for (int i = 0; i < 4; ++i) downs += !q.pop()->move;
    CHECK(downs == 2);
}

TEST_CASE("coalescing never crosses a critical event on the same channel") {
    struct M {
        int pointer;
        bool move;
        int64_t t = 0;
    };
    ControlQueue<M> q(8);
    auto coalesce = [](const M& a, const M& b) { return a.move && b.move && a.pointer == b.pointer; };
    auto droppable = [](const M& m) { return m.move; };
    auto sameChan = [](const M& a, const M& b) { return a.pointer == b.pointer; };
    auto getTime = [](const M& m) { return m.t; };
    q.push({1, true, 100}, coalesce, droppable, sameChan, getTime, 100);
    q.push({1, false, 200}, coalesce, droppable, sameChan, getTime, 200);  // critical DOWN
    q.push({1, true, 300}, coalesce, droppable, sameChan, getTime, 300);   // must NOT merge with first move
    q.push({1, true, 400}, coalesce, droppable, sameChan, getTime, 400);   // merges with the move at index 2
    CHECK(q.size() == 3);
    CHECK(q.coalesced() == 1);
    CHECK(q.pop()->t == 100);
    CHECK(q.pop()->t == 200);   // critical event kept its place
    CHECK(q.pop()->t == 400);   // newest move survived
}

TEST_CASE("stale droppable items are pruned by age") {
    struct M {
        int pointer;
        bool move;
        int64_t t = 0;
    };
    ControlQueue<M> q(8, 50000);  // 50 ms stale window
    auto never = [](const M&, const M&) { return false; };
    auto droppable = [](const M& m) { return m.move; };
    auto getTime = [](const M& m) { return m.t; };
    q.push({1, true, 1000}, never, droppable, never, getTime, 1000);      // old move
    q.push({2, false, 2000}, never, droppable, never, getTime, 2000);     // critical
    q.push({3, true, 100000}, never, droppable, never, getTime, 100000);  // triggers prune
    CHECK(q.staleDropped() == 1);
    CHECK(q.pop()->move == false);  // critical event survived
    CHECK(q.pop()->t == 100000);
}

TEST_CASE("queue never blocks the producer: critical events overflow bounded") {
    struct M {
        int pointer;
        bool move;
        int64_t t = 0;
    };
    ControlQueue<M> q(2);
    auto never = [](const M&, const M&) { return false; };
    auto droppable = [](const M& m) { return m.move; };
    auto getTime = [](const M& m) { return m.t; };
    // Fill with critical events; capacity*4 = 8 allowed.
    for (int i = 0; i < 8; ++i) CHECK(q.push({i, false, 0}, never, droppable, never, getTime, 0));
    CHECK_FALSE(q.push({9, false, 0}, never, droppable, never, getTime, 0));  // hard cap reached
}

TEST_CASE("latest slot honors configured depth for smooth mode") {
    LatestSlot<int> s(2);
    s.put(1);
    s.put(2);
    CHECK(s.size() == 2);
    s.put(3);  // drops 1
    CHECK(s.dropped() == 1);
    CHECK(s.take().value() == 2);
    CHECK(s.take().value() == 3);
    s.setMaxDepth(1);
    CHECK(s.size() == 0);
}

TEST_CASE("control queue close unblocks consumers") {
    ControlQueue<Msg> q(2);
    std::thread t([&] { CHECK_FALSE(q.pop().has_value()); });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    q.close();
    t.join();
}
