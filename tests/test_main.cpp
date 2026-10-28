// Self-contained tests (no framework needed): unit tests for every component, plus a randomized
// differential test that runs the fast book and a deliberately naive reference book side by side
// on millions of random operations and demands identical trades and identical depth.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>
#include <memory>
#include <map>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "itch/book_builder.hpp"
#include "itch/itch.hpp"
#include "lob/bitset.hpp"
#include "lob/latency.hpp"
#include "lob/order_book.hpp"

using namespace lob;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("  CHECK FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

struct TradeLog {
  std::vector<Trade> trades;
  void on_trade(const Trade& t) { trades.push_back(t); }
};

// A single book with its own store/arena, for engine tests.
struct Engine {
  OrderStore store;
  LadderArena arena;
  OrderBook book;
  Engine(std::uint32_t window = 1024, TickRule rule = TickRule::uniform(1), std::size_t max_orders = 1 << 16)
      : store(max_orders, max_orders), arena(1, window), book(0, store, arena, 0, rule) {}
};

// ------------------------------------------------------------------------------------------------
// Reference engine: std::map of std::list, the textbook implementation. Same semantics.
// ------------------------------------------------------------------------------------------------
class RefBook {
 public:
  struct O {
    OrderId id;
    Side side;
    Price price;
    Qty qty;
  };

  SubmitResult submit(OrderId id, Side side, OrderType type, TimeInForce tif, Price price, Qty qty,
                      std::vector<Trade>& out) {
    SubmitResult r;
    bool market = type == OrderType::Market;
    if (qty == 0 || index_.count(id)) {
      r.status = SubmitStatus::Rejected;
      return r;
    }
    Qty rem = qty;
    auto& opp = side == Side::Buy ? asks_ : bids_;
    while (rem > 0 && !opp.empty()) {
      auto lvl = side == Side::Buy ? opp.begin() : std::prev(opp.end());
      if (!market && (side == Side::Buy ? lvl->first > price : lvl->first < price)) break;
      auto& q = lvl->second;
      while (rem > 0 && !q.empty()) {
        O& m = q.front();
        Qty f = std::min(rem, m.qty);
        out.push_back(Trade{id, m.id, m.price, f, side});
        rem -= f;
        m.qty -= f;
        if (m.qty == 0) {
          index_.erase(m.id);
          q.pop_front();
        }
      }
      if (q.empty()) opp.erase(lvl);
    }
    r.filled = qty - rem;
    if (rem) {
      if (!market && tif == TimeInForce::GTC) {
        add(id, side, price, rem);
        r.resting = rem;
      } else {
        r.cancelled = rem;
      }
    }
    return r;
  }

  bool cancel(OrderId id) {
    auto it = index_.find(id);
    if (it == index_.end()) return false;
    erase(it);
    return true;
  }

  SubmitResult modify(OrderId id, Price price, Qty qty, std::vector<Trade>& out) {
    SubmitResult r;
    auto it = index_.find(id);
    if (it == index_.end()) {
      r.status = SubmitStatus::Rejected;
      return r;
    }
    O& o = *it->second;
    if (qty == 0) {
      r.cancelled = o.qty;
      erase(it);
      return r;
    }
    if (price == o.price && qty <= o.qty) {
      o.qty = qty;
      r.resting = qty;
      return r;
    }
    Side s = o.side;
    erase(it);
    return submit(id, s, OrderType::Limit, TimeInForce::GTC, price, qty, out);
  }

  std::vector<std::pair<Price, std::uint64_t>> depth(Side s) const {
    std::vector<std::pair<Price, std::uint64_t>> v;
    auto agg = [](const std::list<O>& q) {
      std::uint64_t t = 0;
      for (auto& o : q) t += o.qty;
      return t;
    };
    if (s == Side::Buy)
      for (auto it = bids_.rbegin(); it != bids_.rend(); ++it) v.emplace_back(it->first, agg(it->second));
    else
      for (auto& kv : asks_) v.emplace_back(kv.first, agg(kv.second));
    return v;
  }
  bool has(OrderId id) const { return index_.count(id) != 0; }
  std::size_t size() const { return index_.size(); }

 private:
  using Map = std::map<Price, std::list<O>>;
  void add(OrderId id, Side s, Price p, Qty q) {
    auto& m = s == Side::Buy ? bids_ : asks_;
    auto& lst = m[p];
    lst.push_back(O{id, s, p, q});
    index_[id] = std::prev(lst.end());
  }
  void erase(std::unordered_map<OrderId, std::list<O>::iterator>::iterator it) {
    O& o = *it->second;
    auto& m = o.side == Side::Buy ? bids_ : asks_;
    auto lvl = m.find(o.price);
    lvl->second.erase(it->second);
    if (lvl->second.empty()) m.erase(lvl);
    index_.erase(it);
  }
  Map bids_, asks_;
  std::unordered_map<OrderId, std::list<O>::iterator> index_;
};

// ------------------------------------------------------------------------------------------------

static void test_bitset() {
  std::printf("bitset\n");
  std::mt19937_64 rng(1);
  for (std::size_t bits : {64u, 128u, 4096u, 4160u, 65536u}) {
    std::vector<std::uint64_t> mem(HierBitset::words_needed(bits));
    HierBitset b(mem.data(), bits);
    std::vector<bool> ref(bits);
    for (int it = 0; it < 20000; ++it) {
      std::size_t i = rng() % bits;
      if (rng() % 3) {
        b.set(i);
        ref[i] = true;
      } else {
        b.clear(i);
        ref[i] = false;
      }
      std::size_t q = rng() % bits;
      std::int64_t p = -1, n = -1;
      for (std::int64_t k = std::int64_t(q); k >= 0; --k)
        if (ref[k]) {
          p = k;
          break;
        }
      for (std::size_t k = q; k < bits; ++k)
        if (ref[k]) {
          n = std::int64_t(k);
          break;
        }
      CHECK_EQ(b.find_prev(q), p);
      CHECK_EQ(b.find_next(q), n);
      if (g_failures) return;
    }
  }
}

static void test_id_map(IdHash hash) {
  std::printf("order id map (%s hash)\n", hash == IdHash::Mix ? "mix" : "identity");
  Pool<Order> pool(100000);
  OrderIdMap map(100000, pool, hash);
  std::unordered_map<OrderId, Handle> ref;
  std::mt19937_64 rng(2);
  std::vector<OrderId> live;
  for (int it = 0; it < 500000; ++it) {
    if (live.empty() || (rng() % 2 && live.size() < 90000)) {
      // mix of sequential (ITCH-like) and random ids
      OrderId id = (rng() % 2) ? OrderId(it) * 4 + 1 : rng();
      if (ref.count(id)) continue;
      Handle h = pool.alloc();
      pool[h].id = id;
      CHECK(map.insert_unique(id, h));
      CHECK(!map.insert_unique(id, h));  // duplicate rejected
      ref[id] = h;
      live.push_back(id);
    } else {
      std::size_t k = rng() % live.size();
      OrderId id = live[k];
      live[k] = live.back();
      live.pop_back();
      CHECK(map.erase(id));
      pool.free(ref[id]);
      ref.erase(id);
      CHECK_EQ(map.find(id), kNull);
    }
    if (it % 50000 == 0)
      for (auto& kv : ref) CHECK_EQ(map.find(kv.first), kv.second);
    if (g_failures) return;
  }
  CHECK_EQ(map.size(), ref.size());
}

static void test_tick_rule() {
  std::printf("tick rule\n");
  TickRule us = TickRule::us_equities();
  std::uint64_t k;
  CHECK(us.key(9999, k) && k == 9999);     // $0.9999 -> fine ticks
  CHECK(us.key(10000, k) && k == 10000);   // $1.00
  CHECK(us.key(10100, k) && k == 10001);   // $1.01
  CHECK(!us.key(10050, k));                // $1.005 sub-penny above $1: off grid
  CHECK(us.key(1500000, k) && k == 10000 + 14900);  // $150.00
  CHECK(us.key(1999999900u, k));           // max ITCH price $199,999.99
  TickRule u = TickRule::uniform(25);
  CHECK(u.key(100, k) && k == 4);
  CHECK(!u.key(101, k));
  // exactness of the multiply-high division across the whole 32-bit range (sampled)
  std::mt19937 rng(3);
  for (int i = 0; i < 1000000; ++i) {
    std::uint32_t p = rng();
    if (p < 10000) continue;
    bool on = us.key(p, k);
    CHECK_EQ(on, (p - 10000) % 100 == 0);
    if (on) CHECK_EQ(k, 10000 + (p - 10000) / 100);
    if (g_failures) return;
  }
}

static void test_engine_basics() {
  std::printf("engine: price-time priority, partial fills, market, IOC, cancel, modify\n");
  {
    Engine e;
    TradeLog log;
    e.book.limit(1, Side::Sell, 101, 100, log);
    e.book.limit(2, Side::Sell, 100, 50, log);
    e.book.limit(3, Side::Sell, 100, 70, log);  // same price, later -> behind order 2
    CHECK_EQ(e.book.top().ask_price, 100u);
    CHECK_EQ(e.book.top().ask_qty, 120u);
    auto r = e.book.limit(10, Side::Buy, 101, 150, log);  // sweeps 100 (2 then 3) then part of 101
    CHECK_EQ(r.filled, 150u);
    CHECK_EQ(r.resting, 0u);
    CHECK_EQ(log.trades.size(), 3u);
    CHECK(log.trades[0] == (Trade{10, 2, 100, 50, Side::Buy}));
    CHECK(log.trades[1] == (Trade{10, 3, 100, 70, Side::Buy}));
    CHECK(log.trades[2] == (Trade{10, 1, 101, 30, Side::Buy}));
    CHECK_EQ(e.book.top().ask_price, 101u);
    CHECK_EQ(e.book.top().ask_qty, 70u);
    std::string err;
    CHECK(e.book.check_invariants(&err));
  }
  {
    Engine e;
    TradeLog log;
    e.book.limit(1, Side::Buy, 99, 10, log);
    e.book.limit(2, Side::Buy, 98, 10, log);
    // limit sell below the bid: trades at the resting (maker) price, rest posts
    auto r = e.book.limit(3, Side::Sell, 97, 25, log);
    CHECK_EQ(r.filled, 20u);
    CHECK_EQ(r.resting, 5u);
    CHECK(log.trades[0] == (Trade{3, 1, 99, 10, Side::Sell}));
    CHECK(log.trades[1] == (Trade{3, 2, 98, 10, Side::Sell}));
    CHECK_EQ(e.book.top().bid_qty, 0u);
    CHECK_EQ(e.book.top().ask_price, 97u);
  }
  {
    Engine e;
    TradeLog log;
    e.book.limit(1, Side::Sell, 100, 10, log);
    auto r = e.book.ioc(2, Side::Buy, 100, 25, log);  // IOC: fill 10, cancel 15, never rests
    CHECK_EQ(r.filled, 10u);
    CHECK_EQ(r.cancelled, 15u);
    CHECK_EQ(e.book.top().bid_qty, 0u);
    CHECK(e.book.lookup(2) == kNull);
    r = e.book.ioc(3, Side::Buy, 100, 5, log);  // nothing to hit
    CHECK_EQ(r.filled, 0u);
    CHECK_EQ(r.cancelled, 5u);
  }
  {
    Engine e;
    TradeLog log;
    e.book.limit(1, Side::Sell, 100, 10, log);
    e.book.limit(2, Side::Sell, 500, 10, log);
    auto r = e.book.market(3, Side::Buy, 15, log);  // market ignores price
    CHECK_EQ(r.filled, 15u);
    CHECK(log.trades[1] == (Trade{3, 2, 500, 5, Side::Buy}));
    r = e.book.market(4, Side::Buy, 100, log);  // only 5 left; remainder cancelled
    CHECK_EQ(r.filled, 5u);
    CHECK_EQ(r.cancelled, 95u);
    CHECK_EQ(e.book.top().ask_qty, 0u);
  }
  {
    Engine e;
    TradeLog log;
    e.book.limit(1, Side::Buy, 100, 10, log);
    e.book.limit(2, Side::Buy, 100, 10, log);
    e.book.limit(3, Side::Buy, 100, 10, log);
    CHECK(e.book.cancel(2));  // middle of the queue
    CHECK(!e.book.cancel(2));
    CHECK(!e.book.cancel(999));
    CHECK_EQ(e.book.top().bid_qty, 20u);
    // reduce qty in place keeps priority: 1 stays ahead of 3
    e.book.modify(1, 100, 4, log);
    e.book.market(9, Side::Sell, 5, log);
    CHECK(log.trades[0] == (Trade{9, 1, 100, 4, Side::Sell}));
    CHECK(log.trades[1] == (Trade{9, 3, 100, 1, Side::Sell}));
    // qty increase loses priority
    log.trades.clear();
    e.book.limit(4, Side::Buy, 100, 10, log);  // queue: 3(9), 4(10)
    e.book.modify(3, 100, 20, log);           // queue: 4(10), 3(20)
    e.book.market(10, Side::Sell, 1, log);
    CHECK_EQ(log.trades[0].maker, 4u);
    // price change that crosses trades immediately
    log.trades.clear();
    e.book.limit(5, Side::Sell, 105, 10, log);
    auto r = e.book.modify(5, 100, 10, log);
    CHECK_EQ(r.filled, 10u);
    CHECK_EQ(log.trades[0].maker, 4u);
    std::string err;
    CHECK(e.book.check_invariants(&err));
  }
  {
    Engine e;
    TradeLog log;
    CHECK(e.book.limit(1, Side::Buy, 100, 10, log).status == SubmitStatus::Accepted);
    CHECK(e.book.limit(1, Side::Buy, 100, 10, log).status == SubmitStatus::Rejected);  // dup id
    CHECK(e.book.limit(2, Side::Buy, 100, 0, log).status == SubmitStatus::Rejected);   // zero qty
    Engine e2(1024, TickRule::uniform(5));
    CHECK(e2.book.limit(1, Side::Buy, 101, 10, log).status == SubmitStatus::Rejected);  // off tick
  }
  {
    // pool exhaustion is reported, not a crash
    Engine e(1024, TickRule::uniform(1), 4);
    TradeLog log;
    for (OrderId i = 1; i <= 4; ++i) CHECK_EQ(e.book.limit(i, Side::Buy, 100 + Price(i), 1, log).resting, 1u);
    auto r = e.book.limit(5, Side::Buy, 50, 1, log);
    CHECK_EQ(r.resting, 0u);
    CHECK_EQ(r.cancelled, 1u);
  }
}

static void test_overflow_and_recenter() {
  std::printf("window overflow and recentering\n");
  Engine e(64);  // tiny window: almost everything is in the overflow
  TradeLog log;
  e.book.limit(1, Side::Buy, 1000, 10, log);  // window recentred on 1000: [968, 1032)
  e.book.limit(2, Side::Buy, 5000, 10, log);  // overflow, and the new best bid
  e.book.limit(3, Side::Buy, 10, 10, log);    // overflow, below
  e.book.limit(4, Side::Sell, 6000, 10, log);
  CHECK_EQ(e.book.top().bid_price, 5000u);
  CHECK(e.book.check_invariants());
  e.book.cancel(2);
  CHECK_EQ(e.book.top().bid_price, 1000u);  // falls back into the window
  e.book.cancel(1);
  CHECK_EQ(e.book.top().bid_price, 10u);  // back into the overflow
  e.book.cancel(3);
  e.book.cancel(4);
  CHECK_EQ(e.book.level_count(Side::Buy) + e.book.level_count(Side::Sell), 0u);
  e.book.limit(5, Side::Sell, 7777, 10, log);  // empty book: window re-anchors here
  CHECK_EQ(e.book.overflow_size(Side::Sell), 0u);
  CHECK(e.book.check_invariants());

  // The market drifts out of the window: once both touches are outside but close together, the
  // window follows them.
  Engine m(64);
  m.book.limit(1, Side::Buy, 100, 10, log);   // anchors window at [68, 132)
  m.book.limit(2, Side::Sell, 130, 10, log);
  m.book.cancel(2);
  m.book.limit(3, Side::Sell, 300, 10, log);  // ask outside, spread 200 > 32: stays put
  CHECK_EQ(m.book.recenters(), 0u);
  CHECK_EQ(m.book.overflow_size(Side::Sell), 1u);
  m.book.cancel(1);
  m.book.limit(4, Side::Buy, 290, 10, log);   // both touches ~295: recenter to [263, 327)
  CHECK_EQ(m.book.recenters(), 1u);
  CHECK_EQ(m.book.overflow_size(Side::Buy) + m.book.overflow_size(Side::Sell), 0u);
  CHECK(m.book.check_invariants());
}

// Randomized differential test against RefBook.
static void test_differential(std::uint32_t window, TickRule rule, Price tick, Price lo, Price hi, int ops,
                              std::uint64_t seed) {
  std::printf("differential: window %u, %d ops, seed %llu\n", window, ops, (unsigned long long)seed);
  Engine e(window, rule, 1 << 16);
  RefBook ref;
  std::mt19937_64 rng(seed);
  std::vector<OrderId> ids;
  OrderId next_id = 1;
  Price mid = (lo + hi) / 2 / tick * tick;
  auto rand_price = [&]() -> Price {
    // mostly near a drifting mid, sometimes anywhere in [lo, hi]
    if (rng() % 10 == 0) return Price(lo + rng() % ((hi - lo) / tick) * tick);
    std::int64_t off = std::int64_t(rng() % 41) - 20;
    std::int64_t p = std::int64_t(mid) + off * std::int64_t(tick);
    if (p < std::int64_t(lo)) p = lo;
    if (p > std::int64_t(hi)) p = hi / tick * tick;
    return Price(p);
  };
  for (int i = 0; i < ops; ++i) {
    if (rng() % 500 == 0) {
      std::int64_t m = std::int64_t(mid) + (std::int64_t(rng() % 21) - 10) * std::int64_t(tick);
      if (m > std::int64_t(lo) && m < std::int64_t(hi)) mid = Price(m);
    }
    TradeLog fast;
    std::vector<Trade> slow;
    SubmitResult a, b;
    int op = int(rng() % 100);
    if (op < 45 || ids.empty()) {
      Side s = rng() % 2 ? Side::Buy : Side::Sell;
      Price p = rand_price();
      Qty q = Qty(1 + rng() % 500);
      OrderId id = next_id++;
      if (op < 3) {
        a = e.book.market(id, s, q, fast);
        b = ref.submit(id, s, OrderType::Market, TimeInForce::IOC, 0, q, slow);
      } else if (op < 10) {
        a = e.book.ioc(id, s, p, q, fast);
        b = ref.submit(id, s, OrderType::Limit, TimeInForce::IOC, p, q, slow);
      } else {
        a = e.book.limit(id, s, p, q, fast);
        b = ref.submit(id, s, OrderType::Limit, TimeInForce::GTC, p, q, slow);
      }
      ids.push_back(id);
    } else if (op < 80) {
      std::size_t k = rng() % ids.size();
      OrderId id = ids[k];
      CHECK_EQ(e.book.cancel(id), ref.cancel(id));
      ids[k] = ids.back();
      ids.pop_back();
    } else {
      OrderId id = ids[rng() % ids.size()];
      Price p = rng() % 2 ? rand_price() : (ref.has(id) ? e.book.order(e.book.lookup(id)).price : rand_price());
      Qty q = Qty(rng() % 400);
      a = e.book.modify(id, p, q, fast);
      b = ref.modify(id, p, q, slow);
    }
    CHECK(a.status == b.status && a.filled == b.filled && a.resting == b.resting && a.cancelled == b.cancelled);
    CHECK(fast.trades == slow);
    CHECK(e.book.depth(Side::Buy) == ref.depth(Side::Buy));
    CHECK(e.book.depth(Side::Sell) == ref.depth(Side::Sell));
    if (i % 97 == 0) {
      std::string err;
      if (!e.book.check_invariants(&err)) {
        std::printf("  %s\n", err.c_str());
        ++g_failures;
      }
    }
    if (g_failures) {
      std::printf("  first failure at op %d\n", i);
      return;
    }
  }
  CHECK_EQ(e.store.orders.live(), ref.size());
}

// ------------------------------------------------------------------------------------------------
// ITCH: encode messages by hand and check the decoder + book builder.
// ------------------------------------------------------------------------------------------------
struct ItchWriter {
  std::vector<std::uint8_t> buf;
  std::uint64_t ts = 34200000000000ull;  // 09:30:00
  void u8(std::uint8_t v) { buf.push_back(v); }
  void u16(std::uint16_t v) { u8(v >> 8), u8(v & 0xff); }
  void u32(std::uint32_t v) { u16(v >> 16), u16(v & 0xffff); }
  void u48(std::uint64_t v) { u16(std::uint16_t(v >> 32)), u32(std::uint32_t(v)); }
  void u64(std::uint64_t v) { u32(std::uint32_t(v >> 32)), u32(std::uint32_t(v)); }
  void str(const char* s, int n) {
    int l = int(std::strlen(s));
    for (int i = 0; i < n; ++i) u8(i < l ? std::uint8_t(s[i]) : ' ');
  }
  std::size_t begin(char type, std::uint16_t loc) {
    u16(0);  // patched later
    std::size_t at = buf.size();
    u8(std::uint8_t(type)), u16(loc), u16(0), u48(++ts);
    return at;
  }
  void end(std::size_t at) {
    std::size_t len = buf.size() - at;
    buf[at - 2] = std::uint8_t(len >> 8);
    buf[at - 1] = std::uint8_t(len);
  }
  void system(char code) {
    auto a = begin('S', 0);
    u8(std::uint8_t(code));
    end(a);
  }
  void directory(std::uint16_t loc, const char* sym) {
    auto a = begin('R', loc);
    str(sym, 8);
    for (int i = 0; i < 39 - 19; ++i) u8(0);
    end(a);
  }
  void add(std::uint16_t loc, std::uint64_t ref, char side, std::uint32_t qty, std::uint32_t price, bool mpid = false) {
    auto a = begin(mpid ? 'F' : 'A', loc);
    u64(ref), u8(std::uint8_t(side)), u32(qty), str("XXX", 8), u32(price);
    if (mpid) str("MPID", 4);
    end(a);
  }
  void exec(std::uint16_t loc, std::uint64_t ref, std::uint32_t qty) {
    auto a = begin('E', loc);
    u64(ref), u32(qty), u64(42);
    end(a);
  }
  void exec_px(std::uint16_t loc, std::uint64_t ref, std::uint32_t qty, std::uint32_t px) {
    auto a = begin('C', loc);
    u64(ref), u32(qty), u64(43), u8('Y'), u32(px);
    end(a);
  }
  void cancel(std::uint16_t loc, std::uint64_t ref, std::uint32_t qty) {
    auto a = begin('X', loc);
    u64(ref), u32(qty);
    end(a);
  }
  void del(std::uint16_t loc, std::uint64_t ref) {
    auto a = begin('D', loc);
    u64(ref);
    end(a);
  }
  void replace(std::uint16_t loc, std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t qty, std::uint32_t px) {
    auto a = begin('U', loc);
    u64(old_ref), u64(new_ref), u32(qty), u32(px);
    end(a);
  }
};

static void test_itch() {
  std::printf("itch decoder + book builder\n");
  ItchWriter w;
  w.system('O');
  w.directory(7, "AAPL");
  w.directory(9, "MSFT");
  w.system('Q');
  w.add(7, 100, 'B', 300, 1500000);         // AAPL bid 150.00 x 300
  w.add(7, 101, 'B', 200, 1500000, true);   // F message, same level
  w.add(7, 102, 'S', 100, 1500100);         // ask 150.01
  w.add(9, 200, 'S', 50, 2500000);          // MSFT
  w.exec(7, 100, 100);                      // at the touch
  w.cancel(7, 101, 50);
  w.exec_px(7, 102, 40, 1500050);
  w.replace(7, 102, 103, 500, 1500200);     // new ref, new price
  w.del(9, 200);
  w.del(7, 999);                            // unknown ref
  w.add(7, 104, 'B', 10, 1499900);
  w.exec(7, 104, 10);                       // NOT at the touch (150.00 is better)

  itch::BuilderConfig cfg;
  cfg.max_orders = 1024;
  cfg.max_levels = 1024;
  cfg.max_books = 16;
  cfg.window = 4096;
  auto b = std::make_unique<itch::BookBuilder>(cfg);
  std::uint64_t n = 0;
  std::size_t used = itch::for_each_message(w.buf.data(), w.buf.data() + w.buf.size(), *b, n);
  CHECK_EQ(used, w.buf.size());
  CHECK_EQ(n, 16u);
  // partial trailing message is left unconsumed
  std::uint64_t n2 = 0;
  auto b2 = std::make_unique<itch::BookBuilder>(cfg);
  CHECK(itch::for_each_message(w.buf.data(), w.buf.data() + w.buf.size() - 3, *b2, n2) < w.buf.size() - 3);
  CHECK_EQ(n2, 15u);

  CHECK(std::string(b->symbol(7)) == "AAPL");
  TopOfBook t = b->book(7)->top();
  CHECK_EQ(t.bid_price, 1500000u);
  CHECK_EQ(t.bid_qty, 200u + 150u);
  CHECK_EQ(t.ask_price, 1500200u);
  CHECK_EQ(t.ask_qty, 500u);
  CHECK_EQ(b->book(9)->top().ask_qty, 0u);
  const auto& s = b->stats();
  CHECK_EQ(s.adds, 5u);
  CHECK_EQ(s.unknown_refs, 1u);
  CHECK_EQ(s.exec_continuous, 2u);
  CHECK_EQ(s.exec_not_at_touch, 1u);
  CHECK_EQ(s.bad_messages, 0u);
  CHECK_EQ(b->store().orders.live(), 3u);

  // wrong length is flagged, not misparsed
  std::uint8_t bad[] = {0, 5, 'A', 0, 1, 0, 0};
  std::uint64_t n3 = 0;
  itch::for_each_message(bad, bad + sizeof bad, *b, n3);
  CHECK_EQ(b->stats().bad_messages, 1u);
}

static void test_histogram() {
  std::printf("latency histogram\n");
  LatencyHistogram h;
  for (std::uint64_t i = 1; i <= 100000; ++i) h.record(i);
  auto near = [](std::uint64_t a, double b) { return std::abs(double(a) - b) <= b * 0.04; };
  CHECK(near(h.percentile(50), 50000));
  CHECK(near(h.percentile(99), 99000));
  CHECK_EQ(h.max(), 100000u);
  CHECK_EQ(h.percentile(100), 100000u);
}

int main() {
  test_bitset();
  test_id_map(IdHash::Mix);
  test_id_map(IdHash::Identity);
  test_tick_rule();
  test_histogram();
  test_engine_basics();
  test_overflow_and_recenter();
  test_differential(1024, TickRule::uniform(1), 1, 900, 1100, 300000, 11);   // all in window
  test_differential(64, TickRule::uniform(1), 1, 500, 1500, 300000, 12);     // heavy overflow
  test_differential(128, TickRule::uniform(5), 5, 0, 5000, 200000, 13);      // coarse ticks
  test_differential(256, TickRule::us_equities(), 100, 9000, 11000, 200000, 14);  // across $1 boundary
  test_itch();
  std::printf(g_failures ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", g_failures);
  return g_failures ? 1 : 0;
}
