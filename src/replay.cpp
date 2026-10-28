// itch_replay: rebuild every NASDAQ order book from a TotalView-ITCH 5.0 file and report
// throughput, per-message latency and correctness checks.
//
//   itch_replay <file|-> [options]
//     --latency            time every message with rdtsc and print percentiles
//     --verify             run an independent std::map reference book alongside and compare
//                          top-of-book after EVERY message and full depth at every checkpoint
//     --symbols A,B,...    symbols whose top of book is printed at checkpoints
//     --checkpoint-min N   exchange-time checkpoint interval in minutes (default 30)
//     --tob-csv PATH       also write checkpoint top-of-book rows to a CSV file
//     --window N           price-window slots per side (default 16384)
//     --max-orders N       order pool capacity (default 16M)
//     --max-levels N       price-level pool capacity (default 4M)
//     --prefault           touch all preallocated memory before starting
//     --max-messages N     stop after N messages
//     --mix-hash           use the multiplicative id hash instead of identity (for comparison)
//     --no-prefetch        disable the software prefetch pipeline (for comparison)
//
// Input is the raw (decompressed) ITCH file; use '-' to read stdin, e.g.
//   gunzip -c 12302019.NASDAQ_ITCH50.gz | itch_replay - --latency

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "itch/book_builder.hpp"
#include "itch/itch.hpp"
#include "lob/latency.hpp"

namespace {

using Clock = std::chrono::steady_clock;

// --------------------------------------------------------------------------------------------
// Independent reference: the most obvious possible implementation (hash map of orders plus a
// std::map of aggregate size per price). Shares only the message decoder with the fast path.
// --------------------------------------------------------------------------------------------
class ReferenceMarket : public itch::NullHandler {
 public:
  ReferenceMarket() : bids_(65536), asks_(65536) { orders_.reserve(1 << 24); }

  void on_add(const itch::Header& h, std::uint64_t ref, char side, std::uint32_t shares, const char*,
              std::uint32_t price) {
    if (!orders_.emplace(ref, Ord{h.locate, side, price, shares}).second) return;
    level_map(h.locate, side)[price] += shares;
  }
  void on_execute(const itch::Header&, std::uint64_t ref, std::uint32_t shares) { take(ref, shares); }
  void on_execute_with_price(const itch::Header&, std::uint64_t ref, std::uint32_t shares, char, std::uint32_t) {
    take(ref, shares);
  }
  void on_cancel(const itch::Header&, std::uint64_t ref, std::uint32_t shares) { take(ref, shares); }
  void on_delete(const itch::Header&, std::uint64_t ref) { take(ref, UINT32_MAX); }
  void on_replace(const itch::Header&, std::uint64_t old_ref, std::uint64_t new_ref, std::uint32_t shares,
                  std::uint32_t price) {
    auto it = orders_.find(old_ref);
    if (it == orders_.end()) return;
    Ord o = it->second;
    take(old_ref, UINT32_MAX);
    on_add(itch::Header{'A', o.locate, 0, 0}, new_ref, o.side, shares, nullptr, price);
  }

  lob::TopOfBook top(std::uint16_t locate) const {
    lob::TopOfBook t;
    const auto& b = bids_[locate];
    const auto& a = asks_[locate];
    if (!b.empty()) {
      t.bid_price = b.rbegin()->first;
      t.bid_qty = b.rbegin()->second;
    }
    if (!a.empty()) {
      t.ask_price = a.begin()->first;
      t.ask_qty = a.begin()->second;
    }
    return t;
  }

  std::vector<std::pair<lob::Price, std::uint64_t>> depth(std::uint16_t locate, lob::Side s) const {
    std::vector<std::pair<lob::Price, std::uint64_t>> out;
    if (s == lob::Side::Buy)
      for (auto it = bids_[locate].rbegin(); it != bids_[locate].rend(); ++it) out.emplace_back(*it);
    else
      for (auto& kv : asks_[locate]) out.emplace_back(kv);
    return out;
  }
  std::size_t live_orders() const { return orders_.size(); }

 private:
  struct Ord {
    std::uint16_t locate;
    char side;
    std::uint32_t price;
    std::uint32_t qty;
  };
  std::map<std::uint32_t, std::uint64_t>& level_map(std::uint16_t locate, char side) {
    return side == 'B' ? bids_[locate] : asks_[locate];
  }
  void take(std::uint64_t ref, std::uint32_t shares) {
    auto it = orders_.find(ref);
    if (it == orders_.end()) return;
    Ord& o = it->second;
    std::uint32_t n = std::min(shares, o.qty);
    auto& m = level_map(o.locate, o.side);
    auto lv = m.find(o.price);
    lv->second -= n;
    if (lv->second == 0) m.erase(lv);
    o.qty -= n;
    if (o.qty == 0) orders_.erase(it);
  }

  std::unordered_map<std::uint64_t, Ord> orders_;
  std::vector<std::map<std::uint32_t, std::uint64_t>> bids_, asks_;
};

// --------------------------------------------------------------------------------------------

struct Options {
  std::string path;
  bool latency = false;
  bool verify = false;
  bool prefault = false;
  bool prefetch = true;
  std::vector<std::string> symbols{"AAPL", "MSFT", "AMZN", "TSLA", "SPY", "QQQ"};
  std::uint64_t checkpoint_ns = 30ull * 60 * 1000000000ull;
  std::string tob_csv;
  std::uint64_t max_messages = UINT64_MAX;
  itch::BuilderConfig cfg;
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: itch_replay <file|-> [--latency] [--verify] [--symbols A,B] [--checkpoint-min N]\n"
               "                   [--tob-csv PATH] [--window N] [--max-orders N] [--max-levels N]\n"
               "                   [--prefault] [--max-messages N] [--mix-hash]\n"
               "                   [--no-prefetch]\n");
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  if (argc < 2) usage();
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--latency") o.latency = true;
    else if (a == "--verify") o.verify = true;
    else if (a == "--prefault") o.prefault = true;
    else if (a == "--symbols") {
      o.symbols.clear();
      std::string s = next();
      for (std::size_t p = 0; p <= s.size();) {
        std::size_t q = s.find(',', p);
        if (q == std::string::npos) q = s.size();
        if (q > p) o.symbols.push_back(s.substr(p, q - p));
        p = q + 1;
      }
    } else if (a == "--checkpoint-min") o.checkpoint_ns = std::stoull(next()) * 60ull * 1000000000ull;
    else if (a == "--tob-csv") o.tob_csv = next();
    else if (a == "--window") o.cfg.window = std::uint32_t(std::stoul(next()));
    else if (a == "--max-orders") o.cfg.max_orders = std::stoull(next());
    else if (a == "--max-levels") o.cfg.max_levels = std::stoull(next());
    else if (a == "--max-messages") o.max_messages = std::stoull(next());
    else if (a == "--mix-hash") o.cfg.id_hash = lob::IdHash::Mix;
    else if (a == "--no-prefetch") o.prefetch = false;
    else if (a.size() && a[0] == '-' && a != "-") usage();
    else o.path = a;
  }
  if (o.path.empty() || o.cfg.window % 64 != 0) usage();
  return o;
}

std::string fmt_price(lob::Price p) {
  char b[32];
  std::snprintf(b, sizeof b, "%u.%04u", p / 10000, p % 10000);
  return b;
}
std::string fmt_time(std::uint64_t ns) {
  std::uint64_t s = ns / 1000000000ull;
  char b[32];
  std::snprintf(b, sizeof b, "%02" PRIu64 ":%02" PRIu64 ":%02" PRIu64, s / 3600, s / 60 % 60, s % 60);
  return b;
}

class Replay {
 public:
  explicit Replay(const Options& o) : opt_(o), builder_(std::make_unique<itch::BookBuilder>(o.cfg)) {
    if (o.verify) ref_ = std::make_unique<ReferenceMarket>();
    if (!o.tob_csv.empty()) {
      csv_ = std::fopen(o.tob_csv.c_str(), "w");
      if (!csv_) {
        std::perror("tob-csv");
        std::exit(1);
      }
      std::fprintf(csv_, "time,symbol,bid,bid_qty,ask,ask_qty\n");
    }
  }
  ~Replay() {
    if (csv_) std::fclose(csv_);
  }

  int run() {
    if (opt_.prefault) {
      auto t0 = Clock::now();
      builder_->prefault();
      std::printf("prefaulted %.1f s\n", std::chrono::duration<double>(Clock::now() - t0).count());
    }
    tsc_ghz_ = lob::calibrate_tsc_ghz();

    int fd = opt_.path == "-" ? 0 : ::open(opt_.path.c_str(), O_RDONLY);
    if (fd < 0) {
      std::perror(opt_.path.c_str());
      return 1;
    }
#ifdef POSIX_FADV_SEQUENTIAL
    if (fd != 0) ::posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif

    // One buffer, allocated once. Partial messages at the end are carried to the front.
    const std::size_t kBuf = std::size_t(64) << 20;
    std::vector<std::uint8_t> buf(kBuf);
    std::size_t have = 0;
    bool eof = false;
    auto wall0 = Clock::now();
    while (!eof && messages_ < opt_.max_messages) {
      auto r0 = Clock::now();
      while (have < kBuf) {
        ssize_t n = ::read(fd, buf.data() + have, kBuf - have);
        if (n < 0) {
          std::perror("read");
          return 1;
        }
        if (n == 0) {
          eof = true;
          break;
        }
        have += std::size_t(n);
        bytes_ += std::uint64_t(n);
      }
      io_ns_ += ns_since(r0);

      auto p0 = Clock::now();
      std::uint64_t cp0 = checkpoint_ns_spent_;
      std::size_t used;
      if (opt_.latency && opt_.verify) used = process<true, true>(buf.data(), buf.data() + have);
      else if (opt_.latency) used = process<true, false>(buf.data(), buf.data() + have);
      else if (opt_.verify) used = process<false, true>(buf.data(), buf.data() + have);
      else used = process<false, false>(buf.data(), buf.data() + have);
      process_ns_ += ns_since(p0) - (checkpoint_ns_spent_ - cp0);

      std::memmove(buf.data(), buf.data() + used, have - used);
      have -= used;
      std::fprintf(stderr, "\r%6.2f GB  %12" PRIu64 " msgs  %s", double(bytes_) / 1e9, messages_,
                   fmt_time(last_ts_).c_str());
    }
    std::fprintf(stderr, "\n");
    wall_ns_ = ns_since(wall0);
    if (fd != 0) ::close(fd);
    if (have != 0 && eof) std::printf("WARNING: %zu trailing bytes (truncated final message)\n", have);

    checkpoint(last_ts_, /*final=*/true);
    report();
    return failures_ == 0 ? 0 : 1;
  }

 private:
  static std::uint64_t ns_since(Clock::time_point t) {
    return std::uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t).count());
  }

  // Advance a lookahead cursor by one complete message; returns the message or nullptr.
  static LOB_ALWAYS_INLINE const std::uint8_t* scout(const std::uint8_t*& c, const std::uint8_t* end) {
    if (end - c < 2) return nullptr;
    std::size_t len = itch::be16(c);
    if (std::size_t(end - c) < 2 + len) return nullptr;
    const std::uint8_t* m = c + 2;
    c += 2 + len;
    return m;
  }

  template <bool kLatency, bool kVerify>
  std::size_t process(const std::uint8_t* p, const std::uint8_t* end) {
    const std::uint8_t* start = p;
    itch::BookBuilder& b = *builder_;
    // Three lookahead cursors running 16, 8 and 4 messages ahead of p (see BookBuilder prefetch).
    const std::uint8_t *c1 = p, *c2 = p, *c3 = p;
    if (opt_.prefetch) {
      for (int i = 0; i < 16; ++i)
        if (const std::uint8_t* m = scout(c1, end)) b.prefetch_slot(m);
      for (int i = 0; i < 8; ++i) scout(c2, end);
      for (int i = 0; i < 4; ++i) scout(c3, end);
    }
    while (end - p >= 2) {
      std::size_t len = itch::be16(p);
      if (std::size_t(end - p) < 2 + len) break;
      const std::uint8_t* m = p + 2;
      p += 2 + len;
      if (opt_.prefetch) {
        if (const std::uint8_t* f = scout(c1, end)) b.prefetch_slot(f);
        if (const std::uint8_t* f = scout(c2, end)) b.prefetch_order(f);
        if (const std::uint8_t* f = scout(c3, end)) b.prefetch_level(f);
      }
      if (len >= 11) {
        std::uint64_t ts = itch::be48(m + 5);
        if (LOB_UNLIKELY(ts >= next_checkpoint_)) on_time_checkpoint(ts);
        last_ts_ = ts;
      }
      if constexpr (kLatency) {
        std::uint64_t t0 = lob::rdtsc();
        itch::dispatch(m, len, b);
        std::uint64_t t1 = lob::rdtsc();
        hist_all_.record(t1 - t0);
        hist_type_[m[0]].record(t1 - t0);
        hist_session_[b.continuous() ? 1 : 0].record(t1 - t0);
      } else {
        itch::dispatch(m, len, b);
      }
      ++type_counts_[m[0]];
      if constexpr (kVerify) verify_message(m, len);
      if (LOB_UNLIKELY(++messages_ >= opt_.max_messages)) break;
    }
    return std::size_t(p - start);
  }

  // ---- verification ----

  void verify_message(const std::uint8_t* m, std::size_t len) {
    itch::dispatch(m, len, *ref_);
    std::uint16_t locate = itch::be16(m + 1);
    char t = char(m[0]);
    if (locate == 0 || !std::strchr("AFECXDU", t)) return;
    ++tob_checks_;
    lob::OrderBook* bk = builder_->book(locate);
    lob::TopOfBook mine = bk ? bk->top() : lob::TopOfBook{};
    lob::TopOfBook theirs = ref_->top(locate);
    if (mine != theirs) {
      if (++tob_mismatches_ <= 10)
        std::printf("TOB MISMATCH msg %" PRIu64 " type %c %s: fast %s x %" PRIu64 " / %s x %" PRIu64
                    "  ref %s x %" PRIu64 " / %s x %" PRIu64 "\n",
                    messages_, t, builder_->symbol(locate), fmt_price(mine.bid_price).c_str(), mine.bid_qty,
                    fmt_price(mine.ask_price).c_str(), mine.ask_qty, fmt_price(theirs.bid_price).c_str(),
                    theirs.bid_qty, fmt_price(theirs.ask_price).c_str(), theirs.ask_qty);
    }
  }

  // Full-depth comparison of every book plus the book's own invariant checker.
  void verify_all_depth() {
    std::uint64_t books = 0, levels = 0, bad = 0, inv_bad = 0;
    builder_->for_each_book([&](std::uint16_t locate, const lob::OrderBook& bk) {
      ++books;
      for (lob::Side s : {lob::Side::Buy, lob::Side::Sell}) {
        auto a = bk.depth(s);
        auto r = ref_->depth(locate, s);
        levels += a.size();
        if (a != r && ++bad <= 5) std::printf("DEPTH MISMATCH %s side %d\n", builder_->symbol(locate), int(s));
      }
      std::string err;
      if (!bk.check_invariants(&err) && ++inv_bad <= 5) std::printf("INVARIANT FAILED: %s\n", err.c_str());
    });
    depth_checks_ += books;
    depth_mismatches_ += bad;
    invariant_failures_ += inv_bad;
    std::printf("  verify: %" PRIu64 " books, %" PRIu64 " levels compared, %" PRIu64 " depth mismatches, %" PRIu64
                " invariant failures\n",
                books, levels, bad, inv_bad);
  }

  // ---- checkpoints ----

  void on_time_checkpoint(std::uint64_t ts) {
    if (next_checkpoint_ != 0) checkpoint(ts, false);
    next_checkpoint_ = (ts / opt_.checkpoint_ns + 1) * opt_.checkpoint_ns;
  }

  void checkpoint(std::uint64_t ts, bool final) {
    auto t0 = Clock::now();
    std::printf("[%s]%s %" PRIu64 " msgs, %zu live orders\n", fmt_time(ts).c_str(), final ? " FINAL" : "",
                messages_, builder_->store().orders.live());
    for (const std::string& sym : opt_.symbols) {
      std::uint16_t loc = find_locate(sym);
      if (!loc) continue;
      lob::TopOfBook t = builder_->book(loc)->top();
      std::printf("    %-6s bid %10s x %-7" PRIu64 " ask %10s x %-7" PRIu64 "\n", sym.c_str(),
                  fmt_price(t.bid_price).c_str(), t.bid_qty, fmt_price(t.ask_price).c_str(), t.ask_qty);
      if (csv_)
        std::fprintf(csv_, "%s,%s,%s,%" PRIu64 ",%s,%" PRIu64 "\n", fmt_time(ts).c_str(), sym.c_str(),
                     fmt_price(t.bid_price).c_str(), t.bid_qty, fmt_price(t.ask_price).c_str(), t.ask_qty);
    }
    // NASDAQ's book should not be crossed during continuous trading for a normally trading stock.
    if (builder_->continuous()) {
      std::uint64_t crossed = 0, checked = 0;
      builder_->for_each_book([&](std::uint16_t loc, const lob::OrderBook& bk) {
        if (builder_->trading_state(loc) != 'T') return;
        lob::TopOfBook t = bk.top();
        if (!t.bid_qty || !t.ask_qty) return;
        ++checked;
        if (t.bid_price >= t.ask_price) {
          ++crossed;
          if (crossed <= 3)
            std::printf("    crossed/locked: %s %s / %s\n", builder_->symbol(loc), fmt_price(t.bid_price).c_str(),
                        fmt_price(t.ask_price).c_str());
        }
      });
      crossed_books_ += crossed;
      crossed_checks_ += checked;
      std::printf("    two-sided books checked: %" PRIu64 ", crossed or locked: %" PRIu64 "\n", checked, crossed);
    }
    if (opt_.verify) verify_all_depth();
    std::fflush(stdout);
    checkpoint_ns_spent_ += ns_since(t0);
  }

  std::uint16_t find_locate(const std::string& sym) {
    auto it = locate_cache_.find(sym);
    if (it != locate_cache_.end()) return it->second;
    std::uint16_t found = 0;
    builder_->for_each_book([&](std::uint16_t loc, const lob::OrderBook&) {
      if (sym == builder_->symbol(loc)) found = loc;
    });
    if (found) locate_cache_[sym] = found;
    return found;
  }

  // ---- report ----

  void print_hist(const char* name, const lob::LatencyHistogram& h) {
    if (!h.count()) return;
    auto ns = [&](std::uint64_t ticks) { return double(ticks) / tsc_ghz_; };
    std::printf("  %-22s %12" PRIu64 "  mean %7.1f  p50 %7.1f  p90 %7.1f  p99 %7.1f  p99.9 %8.1f  p99.99 %8.1f  max %10.1f\n",
                name, h.count(), ns(std::uint64_t(h.mean())), ns(h.percentile(50)), ns(h.percentile(90)),
                ns(h.percentile(99)), ns(h.percentile(99.9)), ns(h.percentile(99.99)), ns(h.max()));
  }

  void report() {
    const itch::BuilderStats& s = builder_->stats();
    const auto& st = builder_->store();
    std::printf("\n================ ITCH replay report ================\n");
    std::printf("input bytes            %" PRIu64 " (%.2f GB)\n", bytes_, double(bytes_) / 1e9);
    std::printf("messages               %" PRIu64 "\n", messages_);
    std::printf("message mix           ");
    for (int t = 0; t < 256; ++t)
      if (type_counts_[t]) std::printf(" %c:%" PRIu64, char(t), type_counts_[t]);
    std::printf("\n");
    std::printf("books (instruments)    %zu\n", builder_->book_count());
    std::printf("peak live orders       %zu  (pool capacity %zu)\n", st.orders.peak(), st.orders.capacity());
    std::printf("peak price levels      %zu  (pool capacity %zu)\n", st.levels.peak(), st.levels.capacity());
    std::printf("live orders at end     %zu\n", st.orders.live());
    std::uint64_t ovf = 0, grows = 0, recenters = 0;
    builder_->for_each_book([&](std::uint16_t, const lob::OrderBook& bk) {
      ovf += bk.overflow_levels_created();
      grows += bk.overflow_grows();
      recenters += bk.recenters();
    });
    std::printf("overflow levels created %" PRIu64 "  (vector grows after startup: %" PRIu64 ")\n", ovf, grows);
    std::printf("window recenters       %" PRIu64 "\n", recenters);
    std::printf("window                 %u slots/side, arena %.1f MB virtual\n", builder_->arena().window(),
                double(builder_->arena().bytes()) / 1e6);

    std::printf("\n-- throughput (book processing only; excludes file I/O and checkpoint printing) --\n");
    double secs = double(process_ns_) / 1e9;
    std::printf("processing time        %.3f s\n", secs);
    std::printf("messages / second      %.2f M\n", double(messages_) / secs / 1e6);
    std::printf("mean ns / message      %.1f\n", double(process_ns_) / double(messages_));
    std::printf("file read time         %.3f s   wall total %.3f s\n", double(io_ns_) / 1e9, double(wall_ns_) / 1e9);
    if (opt_.latency) std::printf("(note: --latency adds two rdtsc per message, so throughput above is pessimistic)\n");
    if (opt_.verify) std::printf("(note: --verify also runs the reference book, so throughput above is meaningless)\n");

    if (opt_.latency) {
      std::printf("\n-- per-message latency, ns (TSC %.3f GHz; includes ~%.0f ns rdtsc overhead) --\n", tsc_ghz_,
                  rdtsc_overhead_ns());
      print_hist("all messages", hist_all_);
      print_hist("  outside 09:30-16:00", hist_session_[0]);
      print_hist("  regular hours", hist_session_[1]);
      const char* names[][2] = {{"A", "A add"},        {"F", "F add (MPID)"},  {"E", "E execute"},
                                {"C", "C execute@price"}, {"X", "X cancel"},     {"D", "D delete"},
                                {"U", "U replace"},    {"P", "P trade (no-op)"}};
      for (auto& n : names) print_hist(n[1], hist_type_[std::uint8_t(n[0][0])]);
    }

    std::printf("\n-- correctness --\n");
    auto check = [&](const char* what, std::uint64_t v, bool must_be_zero = true) {
      bool bad = must_be_zero && v != 0;
      if (bad) ++failures_;
      std::printf("  %-48s %12" PRIu64 "%s\n", what, v, bad ? "   <-- FAIL" : "");
    };
    check("bad/unknown messages", s.bad_messages);
    check("adds rejected (dup id / pool full)", s.add_rejects);
    check("unknown order refs (E/C/X/D/U)", s.unknown_refs);
    check("over-execution / over-cancel", s.over_reductions);
    check("locate mismatches", s.locate_mismatches);
    check("books dropped (max_books)", s.books_dropped);
    check("executions in continuous trading", s.exec_continuous, false);
    check("  ...not at the top of book", s.exec_not_at_touch, false);
    if (s.exec_continuous)
      std::printf("  %-48s %11.4f%%\n", "  ...fraction at the top of book",
                  100.0 * double(s.exec_continuous - s.exec_not_at_touch) / double(s.exec_continuous));
    check("crossed/locked two-sided books at checkpoints", crossed_books_, false);
    std::printf("  %-48s %12" PRIu64 "\n", "  (two-sided book observations)", crossed_checks_);
    if (opt_.verify) {
      check("top-of-book comparisons vs reference", tob_checks_, false);
      check("  ...mismatches", tob_mismatches_);
      check("full-depth book comparisons", depth_checks_, false);
      check("  ...mismatches", depth_mismatches_);
      check("invariant check failures", invariant_failures_);
      std::printf("  %-48s %12zu (fast: %zu)\n", "reference live orders at end", ref_->live_orders(),
                  st.orders.live());
      if (ref_->live_orders() != st.orders.live()) ++failures_;
    }
    std::printf("\nRESULT: %s\n", failures_ == 0 ? "PASS" : "FAIL");
  }

  double rdtsc_overhead_ns() const {
    lob::LatencyHistogram h;
    for (int i = 0; i < 100000; ++i) {
      std::uint64_t a = lob::rdtsc();
      std::uint64_t b = lob::rdtsc();
      h.record(b - a);
    }
    return double(h.percentile(50)) / tsc_ghz_;
  }

  Options opt_;
  std::unique_ptr<itch::BookBuilder> builder_;
  std::unique_ptr<ReferenceMarket> ref_;
  FILE* csv_ = nullptr;
  double tsc_ghz_ = 1.0;

  std::uint64_t messages_ = 0, bytes_ = 0, last_ts_ = 0, next_checkpoint_ = 0;
  std::uint64_t process_ns_ = 0, io_ns_ = 0, wall_ns_ = 0, checkpoint_ns_spent_ = 0;
  std::uint64_t type_counts_[256] = {};
  lob::LatencyHistogram hist_all_;
  lob::LatencyHistogram hist_session_[2];  // [0] outside regular hours, [1] 09:30-16:00
  std::unique_ptr<lob::LatencyHistogram[]> hist_type_holder_{new lob::LatencyHistogram[256]};
  lob::LatencyHistogram* hist_type_ = hist_type_holder_.get();

  std::uint64_t tob_checks_ = 0, tob_mismatches_ = 0, depth_checks_ = 0, depth_mismatches_ = 0,
                invariant_failures_ = 0;
  std::uint64_t crossed_books_ = 0, crossed_checks_ = 0;
  std::uint64_t failures_ = 0;
  std::map<std::string, std::uint16_t> locate_cache_;
};

}  // namespace

int main(int argc, char** argv) {
  Options o = parse(argc, argv);
  Replay r(o);
  return r.run();
}
