// engine_bench: synthetic order flow through the matching engine (one instrument).
//
// All random inputs are generated before the clock starts, so the timed loop measures only the
// book. The flow is shaped like a real market: most activity near the touch, many cancels,
// some marketable limits, IOCs and market orders, and a slowly drifting mid price.
//
//   engine_bench [ops=20000000] [seed=1] [max_orders=1048576]

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "lob/latency.hpp"
#include "lob/order_book.hpp"

using namespace lob;

namespace {

enum class Op : std::uint8_t { Limit, Cancel, Modify, Ioc, Market };

struct Input {
  Op op;
  Side side;
  std::int16_t offset;  // ticks from mid (positive = away from the touch for this side)
  Qty qty;
  std::uint32_t pick;   // which recent order to cancel/modify
  std::int32_t mid_step;
};

struct CountingListener {
  std::uint64_t trades = 0, volume = 0;
  void on_trade(const Trade& t) {
    ++trades;
    volume += t.qty;
  }
};

}  // namespace

int main(int argc, char** argv) {
  const std::size_t n = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 20000000;
  const std::uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 1;
  const std::size_t max_orders = argc > 3 ? std::strtoull(argv[3], nullptr, 10) : std::size_t(1) << 20;

  // ---- startup: allocate everything, generate inputs ----
  OrderStore store(max_orders, 1 << 20);
  LadderArena arena(1, 1 << 14);
  store.prefault();
  arena.prefault();
  OrderBook book(0, store, arena, 0, TickRule::uniform(1));

  std::mt19937_64 rng(seed);
  std::vector<Input> in(n);
  for (auto& x : in) {
    int r = int(rng() % 100);
    x.op = r < 46 ? Op::Limit : r < 86 ? Op::Cancel : r < 94 ? Op::Modify : r < 98 ? Op::Ioc : Op::Market;
    x.side = rng() & 1 ? Side::Buy : Side::Sell;
    // geometric-ish distance from the touch; a few percent are marketable (negative)
    std::int32_t d = std::int32_t(std::min<std::uint64_t>(rng() % 64, rng() % 64)) - 2;
    x.offset = std::int16_t(d);
    x.qty = Qty(1 + rng() % 100) * 100;
    x.pick = std::uint32_t(rng());
    x.mid_step = rng() % 200 == 0 ? std::int32_t(rng() % 3) - 1 : 0;
  }

  // Ids of orders we placed that may still rest. Cancels pick from here (and remove the id), so
  // the book reaches a steady state instead of growing forever. Filled orders stay in the list
  // until picked, which gives a realistic share of "too late to cancel" misses.
  std::vector<OrderId> live;
  live.reserve(n + 20000);

  const char* names[] = {"limit", "cancel", "modify", "ioc", "market"};
  LatencyHistogram hist[5], all;
  CountingListener listener;
  const double ghz = calibrate_tsc_ghz();

  Price mid = 100000;
  OrderId next_id = 1;
  std::uint64_t cancel_hits = 0;

  // warm-up pass to populate the book
  for (int i = 0; i < 20000; ++i) {
    Side s = i & 1 ? Side::Buy : Side::Sell;
    Price p = s == Side::Buy ? mid - 1 - Price(i % 50) : mid + 1 + Price(i % 50);
    book.limit(next_id, s, p, 100, listener);
    live.push_back(next_id++);
  }

  // ---- timed loop ----
  auto t0 = std::chrono::steady_clock::now();
  for (const Input& x : in) {
    mid = Price(std::int64_t(mid) + x.mid_step);
    std::uint64_t c0 = rdtsc();
    switch (x.op) {
      case Op::Limit:
      case Op::Ioc: {
        Price p = x.side == Side::Buy ? mid - x.offset : mid + x.offset;
        OrderId id = next_id++;
        if (x.op == Op::Limit) {
          if (book.limit(id, x.side, p, x.qty, listener).resting) live.push_back(id);
        } else {
          book.ioc(id, x.side, p, x.qty, listener);
        }
        break;
      }
      case Op::Cancel:
        if (!live.empty()) {
          std::size_t k = x.pick % live.size();
          cancel_hits += book.cancel(live[k]);
          live[k] = live.back();
          live.pop_back();
        }
        break;
      case Op::Modify: {
        if (live.empty()) break;
        OrderId id = live[x.pick % live.size()];
        Handle h = book.lookup(id);
        if (h != kNull) {
          const Order& o = book.order(h);
          // half the time shrink in place (keeps priority), otherwise reprice
          if (x.pick & 1)
            book.modify(id, o.price, o.qty > 100 ? o.qty - 100 : o.qty, listener);
          else
            book.modify(id, o.side == Side::Buy ? mid - x.offset : mid + x.offset, x.qty, listener);
        }
        break;
      }
      case Op::Market:
        book.market(next_id++, x.side, x.qty, listener);
        break;
    }
    std::uint64_t c1 = rdtsc();
    hist[int(x.op)].record(c1 - c0);
    all.record(c1 - c0);
  }
  double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

  auto ns = [&](std::uint64_t t) { return double(t) / ghz; };
  std::printf("engine_bench: %zu operations, seed %" PRIu64 ", order capacity %zu (id table %.1f MB)\n", n, seed,
              max_orders, double(store.ids.capacity()) * 4 / 1e6);
  std::printf("throughput        %.2f M ops/s  (%.1f ns/op incl. timing overhead)\n", double(n) / secs / 1e6,
              secs * 1e9 / double(n));
  std::printf("trades            %" PRIu64 "  volume %" PRIu64 "  cancels that hit %" PRIu64 "\n", listener.trades,
              listener.volume, cancel_hits);
  std::printf("resting at end    %zu orders, %zu bid levels, %zu ask levels\n", store.orders.live(),
              book.level_count(Side::Buy), book.level_count(Side::Sell));
  std::printf("\nlatency (ns)        count      p50     p90     p99   p99.9  p99.99      max\n");
  auto row = [&](const char* name, const LatencyHistogram& h) {
    std::printf("  %-10s %10" PRIu64 " %8.1f %7.1f %7.1f %7.1f %7.1f %8.1f\n", name, h.count(), ns(h.percentile(50)),
                ns(h.percentile(90)), ns(h.percentile(99)), ns(h.percentile(99.9)), ns(h.percentile(99.99)),
                ns(h.max()));
  };
  row("all", all);
  for (int i = 0; i < 5; ++i) row(names[i], hist[i]);
  std::string err;
  if (!book.check_invariants(&err)) {
    std::printf("INVARIANT FAILURE: %s\n", err.c_str());
    return 1;
  }
  std::printf("invariants OK\n");
  return 0;
}
