// main.cpp – event-driven market simulation on top of the limit order book
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -I include -I include/lob -o lob_sim src/main.cpp
//
// A hidden "fundamental" value follows a diffusion with occasional news jumps.
// Six agents trade against each other through the real OrderBook:
//
//   MM-fast/mid/slow  market makers quoting a ladder around a lagged estimate of
//                     the fundamental, skewed by inventory (they get picked off
//                     by informed flow and earn the spread from noise flow)
//   noise             uninformed flow: passive limits near the mid, random
//                     marketable orders, random cancels
//   informed          sees the true fundamental and sweeps anything mispriced
//   momentum          chases the mid-price move over the last few seconds
//
// Time is simulated (ns timestamps are passed to add_order). A shadow model of
// every resting order is rebuilt from the trade stream and checked against the
// book throughout, so the run doubles as an end-to-end correctness test.

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <map>
#include <queue>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "OrderBook.hpp"

using namespace lob;

namespace
{

	// ── market / simulation parameters ───────────────────────────────────────
	constexpr double MIN_PX = 50.0;
	constexpr double TICK = 0.01;
	constexpr int NUM_TICKS = 10'000; // prices 50.00 .. 149.99

	constexpr double SIM_SECONDS = 1800.0; // 30 simulated minutes
	constexpr double START_VALUE = 100.0;
	constexpr double FUND_DT = 0.01;	   // fundamental updates every 10 ms
	constexpr double FUND_SIGMA = 0.02;	   // price units per sqrt(second)
	constexpr double JUMP_RATE = 1.0 / 90; // news events per second
	constexpr double JUMP_SD = 0.30;	   // price units

	constexpr double SAMPLE_DT = 0.1;	  // book sampling period
	constexpr double REPORT_DT = 300.0;	  // progress line period
	constexpr double NOISE_RATE = 150.0;  // noise arrivals / s
	constexpr double CANCEL_RATE = 100.0; // random cancels of noise orders / s
	constexpr double INFORMED_RATE = 4.0; // informed wake-ups / s
	constexpr double MOMENTUM_RATE = 2.0; // momentum wake-ups / s

	enum Agent : int
	{
		MM_FAST,
		MM_MID,
		MM_SLOW,
		NOISE,
		INFORMED,
		MOMENTUM,
		N_AGENTS
	};
	constexpr int N_MM = 3;
	constexpr std::array<const char *, N_AGENTS> AGENT_NAME = {
		"MM-fast", "MM-mid", "MM-slow", "noise", "informed", "momentum"};

	struct MMParams
	{
		double refresh;	 // seconds between quote refreshes
		double ema_tau;	 // lag of its fundamental estimate, seconds
		int half;		 // half spread of the innermost quote, ticks
		int levels;
		int spacing;	 // ticks between ladder rungs
		uint64_t size;
		double skew;	 // price shift per share of inventory
		int64_t max_inv; // stop quoting the side that would grow inventory past this
	};
	constexpr std::array<MMParams, N_MM> MM = {{
		{0.05, 1.0, 3, 3, 2, 40, 0.00004, 1500},
		{0.10, 3.0, 5, 3, 3, 80, 0.00002, 2500},
		{0.25, 8.0, 8, 3, 4, 150, 0.00001, 4000},
	}};

	enum EventKind : int
	{
		EV_FUNDAMENTAL,
		EV_NOISE,
		EV_CANCEL,
		EV_INFORMED,
		EV_MOMENTUM,
		EV_MM,
		EV_SAMPLE,
		EV_REPORT
	};
	struct Event
	{
		double t;
		EventKind kind;
		int who;
		bool operator>(const Event &o) const { return t > o.t; }
	};

	struct Rec // a live order, as the simulator remembers it
	{
		int agent;
		Side side;
		int tick;
		uint64_t qty; // remaining
	};

	struct AgentStats
	{
		double cash = 0.0;
		int64_t inv = 0;
		uint64_t orders = 0, cancels = 0, fills = 0, volume = 0;
	};

	// ─────────────────────────────────────────────────────────────────────────
	class Simulation
	{
	public:
		Simulation()
			: book(BookConfig{.min_price = MIN_PX, .tick_size = TICK, .num_ticks = NUM_TICKS}),
			  rng(20240607), v(START_VALUE), last_mid_tick(tick_of(START_VALUE))
		{
			ema.fill(START_VALUE);
		}

		void run()
		{
			for (int m = 0; m < N_MM; ++m)
				push(0.0, EV_MM, m);
			push(FUND_DT, EV_FUNDAMENTAL);
			push(expo(NOISE_RATE), EV_NOISE);
			push(expo(CANCEL_RATE), EV_CANCEL);
			push(expo(INFORMED_RATE), EV_INFORMED);
			push(expo(MOMENTUM_RATE), EV_MOMENTUM);
			push(SAMPLE_DT, EV_SAMPLE);
			push(REPORT_DT, EV_REPORT);

			print_header();
			while (!events.empty() && events.top().t <= SIM_SECONDS)
			{
				const Event e = events.top();
				events.pop();
				now = e.t;
				dispatch(e);
				check_cheap();
				if (++n_events % 5000 == 0)
					check_full();
			}
			finish();
		}

	private:
		OrderBook book;
		std::mt19937_64 rng;
		std::priority_queue<Event, std::vector<Event>, std::greater<>> events;

		double now = 0.0;
		double v;						// true fundamental
		std::array<double, N_MM> ema{}; // each MM's lagged estimate of it
		uint64_t next_id = 1;
		uint64_t n_events = 0;
		int last_mid_tick; // last two-sided mid, used when one side is empty

		std::unordered_map<uint64_t, Rec> live;
		std::map<int, uint64_t> ref_bid, ref_ask; // shadow depth: tick → resting qty
		std::array<std::vector<uint64_t>, N_MM> quotes;
		std::vector<uint64_t> noise_resting; // may hold stale ids (lazily cleaned)
		std::vector<Trade> trade_buf;		 // reused by every add_order call
		std::array<AgentStats, N_AGENTS> ag{};

		// market statistics
		uint64_t n_trades = 0, traded_qty = 0, max_trade = 0, buy_aggr = 0;
		double notional = 0.0;
		std::vector<double> mid_series, fund_series; // every SAMPLE_DT
		double spread_sum = 0.0, depth_sum = 0.0;
		std::size_t spread_n = 0, peak_orders = 0;
		uint64_t period_trades = 0, period_volume = 0;

		// ── helpers ──────────────────────────────────────────────────────────
		static int tick_of(double p)
		{
			return std::clamp(static_cast<int>(std::llround((p - MIN_PX) / TICK)), 0, NUM_TICKS - 1);
		}
		static double px_of(int tick) { return MIN_PX + tick * TICK; }
		uint64_t now_ns() const { return 1'000'000'000ULL + static_cast<uint64_t>(now * 1e9); }

		void push(double t, EventKind k, int who = 0) { events.push({t, k, who}); }
		double expo(double rate) { return now + std::exponential_distribution<double>(rate)(rng); }
		double normal() { return std::normal_distribution<double>()(rng); }
		double uniform() { return std::uniform_real_distribution<double>()(rng); }
		Side random_side() { return (rng() & 1) ? Side::Buy : Side::Sell; }
		uint64_t lognormal_qty(double median, double sigma, uint64_t cap)
		{
			const double q = median * std::exp(sigma * normal());
			return std::clamp<uint64_t>(static_cast<uint64_t>(q), 1, cap);
		}

		std::map<int, uint64_t> &ref_of(Side s) { return s == Side::Buy ? ref_bid : ref_ask; }
		void sub_ref(Side s, int tick, uint64_t q)
		{
			auto &m = ref_of(s);
			auto it = m.find(tick);
			assert(it != m.end() && it->second >= q);
			if ((it->second -= q) == 0)
				m.erase(it);
		}

		bool two_sided() const { return book.best_bid() && book.best_ask(); }
		int best_bid_tick() const { return tick_of(book.price_of(*book.best_bid())); }
		int best_ask_tick() const { return tick_of(book.price_of(*book.best_ask())); }
		double mid() const { return book.mid_price(); }

		// ── order flow ───────────────────────────────────────────────────────
		// Apply a batch of trades to the shadow model and the agents' books.
		// The aggressing order is already registered in `live`.
		void apply(const std::vector<Trade> &trades)
		{
			for (const Trade &t : trades)
			{
				auto bi = live.find(t.bid_order_id);
				auto ai = live.find(t.ask_order_id);
				assert(bi != live.end() && ai != live.end());
				Rec &b = bi->second;
				Rec &a = ai->second;
				const Rec &maker = (t.aggressor == Side::Buy) ? a : b;
				assert(px_of(maker.tick) == t.price);
				assert(t.aggressor == Side::Buy ? b.tick >= a.tick : a.tick <= b.tick);
				assert(t.quantity <= b.qty && t.quantity <= a.qty);

				sub_ref(maker.side, maker.tick, t.quantity);
				const double value = t.price * static_cast<double>(t.quantity);
				ag[b.agent].cash -= value;
				ag[b.agent].inv += static_cast<int64_t>(t.quantity);
				ag[a.agent].cash += value;
				ag[a.agent].inv -= static_cast<int64_t>(t.quantity);
				for (int who : {b.agent, a.agent})
				{
					++ag[who].fills;
					ag[who].volume += t.quantity;
				}

				++n_trades;
				++period_trades;
				traded_qty += t.quantity;
				period_volume += t.quantity;
				max_trade = std::max(max_trade, t.quantity);
				notional += value;
				buy_aggr += (t.aggressor == Side::Buy);

				if ((b.qty -= t.quantity) == 0)
					live.erase(t.bid_order_id);
				if ((a.qty -= t.quantity) == 0)
					live.erase(t.ask_order_id);
			}
		}

		// Submit a limit order. `ioc` emulates immediate-or-cancel: whatever does
		// not trade right away is pulled again. Returns the order id.
		uint64_t submit(int agent, Side side, int tick, uint64_t qty, bool ioc)
		{
			tick = std::clamp(tick, 0, NUM_TICKS - 1);
			const uint64_t id = next_id++;
			live[id] = {agent, side, tick, qty};
			++ag[agent].orders;
			trade_buf.clear();
			book.add_order(id, qty, px_of(tick), side, trade_buf, now_ns());
			apply(trade_buf);

			if (auto it = live.find(id); it != live.end())
			{
				ref_of(side)[tick] += it->second.qty; // remainder rests
				if (ioc)
					cancel(id, /*count=*/false);
			}
			peak_orders = std::max(peak_orders, live.size());
			return id;
		}

		void cancel(uint64_t id, bool count = true)
		{
			auto it = live.find(id);
			const bool known = it != live.end();
			[[maybe_unused]] const bool ok = book.cancel_order(id);
			assert(ok == known);
			if (!known)
				return;
			if (count)
				++ag[it->second.agent].cancels;
			sub_ref(it->second.side, it->second.tick, it->second.qty);
			live.erase(it);
		}

		// ── agents ───────────────────────────────────────────────────────────
		void on_fundamental()
		{
			v += FUND_SIGMA * std::sqrt(FUND_DT) * normal();
			if (uniform() < FUND_DT * JUMP_RATE)
				v += JUMP_SD * normal();
			v = std::clamp(v, 60.0, 140.0);
			for (int m = 0; m < N_MM; ++m)
				ema[m] += (v - ema[m]) * (1.0 - std::exp(-FUND_DT / MM[m].ema_tau));
			push(now + FUND_DT, EV_FUNDAMENTAL);
		}

		void on_market_maker(int m)
		{
			const MMParams &P = MM[m];
			for (uint64_t id : quotes[m])
				cancel(id);
			quotes[m].clear();

			const int64_t inv = ag[m].inv;
			const int centre = tick_of(ema[m] - P.skew * static_cast<double>(inv));
			for (int k = 0; k < P.levels; ++k)
			{
				const int off = P.half + k * P.spacing;
				const uint64_t qty = P.size * static_cast<uint64_t>(1 + k);
				if (inv < P.max_inv && centre - off >= 0)
					quotes[m].push_back(submit(m, Side::Buy, centre - off, qty, false));
				if (inv > -P.max_inv && centre + off < NUM_TICKS)
					quotes[m].push_back(submit(m, Side::Sell, centre + off, qty, false));
			}
			push(now + P.refresh * (0.9 + 0.2 * uniform()), EV_MM, m);
		}

		void on_noise()
		{
			const Side side = random_side();
			if (uniform() < 0.25)
			{
				// Marketable order with a price cap 20 ticks through the touch.
				if (side == Side::Buy && book.best_ask())
					submit(NOISE, side, best_ask_tick() + 20, lognormal_qty(25, 0.9, 600), true);
				else if (side == Side::Sell && book.best_bid())
					submit(NOISE, side, best_bid_tick() - 20, lognormal_qty(25, 0.9, 600), true);
			}
			else
			{
				// Passive limit: geometric distance (mean ≈ 7 ticks) behind the mid.
				const int ref = two_sided() ? tick_of(mid()) : last_mid_tick;
				const int off = std::geometric_distribution<int>(0.12)(rng);
				const int tick = side == Side::Buy ? ref - off : ref + off;
				noise_resting.push_back(submit(NOISE, side, tick, lognormal_qty(30, 0.8, 500), false));
			}
			push(expo(NOISE_RATE), EV_NOISE);
		}

		void on_cancel()
		{
			while (!noise_resting.empty())
			{
				const std::size_t i = rng() % noise_resting.size();
				const uint64_t id = noise_resting[i];
				noise_resting[i] = noise_resting.back();
				noise_resting.pop_back();
				if (live.count(id)) // else it was already filled: try another
				{
					cancel(id);
					break;
				}
			}
			push(expo(CANCEL_RATE), EV_CANCEL);
		}

		void on_informed()
		{
			constexpr double EDGE = 0.02;
			const uint64_t qty = 20 + rng() % 180;
			constexpr int64_t MAX_INV = 30000; // risk limit: beyond it, only trade back toward flat
			const int64_t inv = ag[INFORMED].inv;
			if (book.best_ask() && v > book.price_of(*book.best_ask()) + EDGE && inv < MAX_INV)
				submit(INFORMED, Side::Buy, tick_of(v), qty, true);
			else if (book.best_bid() && v < book.price_of(*book.best_bid()) - EDGE && inv > -MAX_INV)
				submit(INFORMED, Side::Sell, tick_of(v), qty, true);
			push(expo(INFORMED_RATE), EV_INFORMED);
		}

		void on_momentum()
		{
			constexpr std::size_t LOOKBACK = 50; // 5 s of samples
			constexpr double THRESHOLD = 0.15;
			if (mid_series.size() > LOOKBACK && two_sided())
			{
				const double move = mid() - mid_series[mid_series.size() - LOOKBACK];
				if (move > THRESHOLD)
					submit(MOMENTUM, Side::Buy, best_ask_tick() + 5, 30, true);
				else if (move < -THRESHOLD)
					submit(MOMENTUM, Side::Sell, best_bid_tick() - 5, 30, true);
			}
			push(expo(MOMENTUM_RATE), EV_MOMENTUM);
		}

		// ── sampling / reporting ─────────────────────────────────────────────
		void on_sample()
		{
			if (two_sided())
			{
				last_mid_tick = tick_of(mid());
				spread_sum += book.spread();
				depth_sum += static_cast<double>(book.best_bid()->get_total_volume() +
												 book.best_ask()->get_total_volume());
				++spread_n;
			}
			mid_series.push_back(two_sided() ? mid() : (mid_series.empty() ? START_VALUE : mid_series.back()));
			fund_series.push_back(v);
			push(now + SAMPLE_DT, EV_SAMPLE);
		}

		static uint64_t depth_top(const std::map<int, uint64_t> &m, bool from_high, int levels)
		{
			uint64_t s = 0;
			int n = 0;
			if (from_high)
				for (auto it = m.rbegin(); it != m.rend() && n < levels; ++it, ++n)
					s += it->second;
			else
				for (auto it = m.begin(); it != m.end() && n < levels; ++it, ++n)
					s += it->second;
			return s;
		}

		void print_header() const
		{
			std::cout << "Limit order book simulation – " << SIM_SECONDS << " s simulated, tick "
					  << TICK << ", " << NUM_TICKS << " price levels\n\n"
					  << "   time    fundamental   mid      spread   bid5   ask5   trades   volume\n"
					  << "  ─────────────────────────────────────────────────────────────────────\n";
		}

		void on_report()
		{
			std::cout << std::fixed << std::setprecision(2) << "  " << std::setw(5) << static_cast<int>(now) << " s"
					  << std::setw(11) << v << std::setw(10) << (two_sided() ? mid() : 0.0)
					  << std::setw(9) << (two_sided() ? book.spread() : 0.0)
					  << std::setw(8) << depth_top(ref_bid, true, 5) << std::setw(7) << depth_top(ref_ask, false, 5)
					  << std::setw(9) << period_trades << std::setw(9) << period_volume << "\n";
			period_trades = period_volume = 0;
			push(now + REPORT_DT, EV_REPORT);
		}

		void dispatch(const Event &e)
		{
			switch (e.kind)
			{
			case EV_FUNDAMENTAL: on_fundamental(); break;
			case EV_NOISE: on_noise(); break;
			case EV_CANCEL: on_cancel(); break;
			case EV_INFORMED: on_informed(); break;
			case EV_MOMENTUM: on_momentum(); break;
			case EV_MM: on_market_maker(e.who); break;
			case EV_SAMPLE: on_sample(); break;
			case EV_REPORT: on_report(); break;
			}
		}

		// ── consistency checks ───────────────────────────────────────────────
		void check_cheap() const
		{
			assert(book.order_count() == live.size());
			if (two_sided())
				assert(book.price_of(*book.best_bid()) < book.price_of(*book.best_ask())); // never crossed
		}

		void check_full() const
		{
			assert(book.pool_used() == live.size());
			assert(book.bid_levels() == ref_bid.size());
			assert(book.ask_levels() == ref_ask.size());
			if (ref_bid.empty())
				assert(!book.best_bid());
			else
			{
				assert(tick_of(book.price_of(*book.best_bid())) == ref_bid.rbegin()->first);
				assert(book.best_bid()->get_total_volume() == ref_bid.rbegin()->second);
			}
			if (ref_ask.empty())
				assert(!book.best_ask());
			else
			{
				assert(tick_of(book.price_of(*book.best_ask())) == ref_ask.begin()->first);
				assert(book.best_ask()->get_total_volume() == ref_ask.begin()->second);
			}
			uint64_t qb = 0, qa = 0, rb = 0, ra = 0;
			for (const auto &kv : live)
				(kv.second.side == Side::Buy ? qb : qa) += kv.second.qty;
			for (const auto &kv : ref_bid)
				rb += kv.second;
			for (const auto &kv : ref_ask)
				ra += kv.second;
			assert(qb == rb && qa == ra);
		}

		// ── end of run ───────────────────────────────────────────────────────
		static std::string sparkline(const std::vector<double> &s, double lo, double hi, int cols)
		{
			static const char *BARS[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
			std::string out;
			for (int c = 0; c < cols; ++c)
			{
				const double x = s[static_cast<std::size_t>(c) * (s.size() - 1) / static_cast<std::size_t>(cols - 1)];
				const int level = hi > lo ? static_cast<int>((x - lo) / (hi - lo) * 7.999) : 0;
				out += BARS[std::clamp(level, 0, 7)];
			}
			return out;
		}

		void print_book(int levels) const
		{
			std::cout << "\nFinal book (top " << levels << " levels per side)\n";
			std::vector<std::pair<int, uint64_t>> asks, bids;
			for (auto it = ref_ask.begin(); it != ref_ask.end() && static_cast<int>(asks.size()) < levels; ++it)
				asks.push_back(*it);
			for (auto it = ref_bid.rbegin(); it != ref_bid.rend() && static_cast<int>(bids.size()) < levels; ++it)
				bids.push_back(*it);
			auto row = [](const char *tag, int tick, uint64_t q)
			{
				std::cout << "  " << tag << std::setw(8) << px_of(tick) << std::setw(8) << q << "  "
						  << std::string(std::min<uint64_t>(q / 40, 40), '#') << "\n";
			};
			for (auto it = asks.rbegin(); it != asks.rend(); ++it)
				row("ask ", it->first, it->second);
			std::cout << "  ─── spread " << (two_sided() ? book.spread() : 0.0) << " ───\n";
			for (const auto &b : bids)
				row("bid ", b.first, b.second);
		}

		void finish()
		{
			check_full();
			const double v_end = v;
			std::cout << std::fixed << std::setprecision(2);
			print_book(8);

			// Drain: cancelling everything that still rests must empty the book.
			std::vector<uint64_t> rest;
			for (const auto &kv : live)
				rest.push_back(kv.first);
			for (uint64_t id : rest)
				cancel(id, false);
			assert(book.order_count() == 0 && book.pool_used() == 0);
			assert(book.bid_levels() == 0 && book.ask_levels() == 0);

			// Every share bought was sold by someone, and every dollar paid was received.
			int64_t inv_sum = 0;
			double cash_sum = 0.0;
			uint64_t total_orders = 0, total_cancels = 0;
			for (const AgentStats &a : ag)
			{
				inv_sum += a.inv;
				cash_sum += a.cash;
				total_orders += a.orders;
				total_cancels += a.cancels;
			}
			assert(inv_sum == 0 && std::fabs(cash_sum) < 1e-3);

			// Price-discovery quality and volatility (1-second changes).
			double track_err = 0.0, mret2 = 0.0, fret2 = 0.0;
			for (std::size_t i = 0; i < mid_series.size(); ++i)
				track_err += std::fabs(mid_series[i] - fund_series[i]);
			track_err /= static_cast<double>(mid_series.size());
			const std::size_t step = static_cast<std::size_t>(1.0 / SAMPLE_DT);
			std::size_t nret = 0;
			for (std::size_t i = step; i < mid_series.size(); i += step, ++nret)
			{
				mret2 += std::pow(mid_series[i] - mid_series[i - step], 2);
				fret2 += std::pow(fund_series[i] - fund_series[i - step], 2);
			}
			const double vol_mid = std::sqrt(mret2 / static_cast<double>(nret));
			const double vol_fund = std::sqrt(fret2 / static_cast<double>(nret));

			const auto [flo, fhi] = std::minmax_element(fund_series.begin(), fund_series.end());
			const auto [mlo, mhi] = std::minmax_element(mid_series.begin(), mid_series.end());
			const double plo = std::min(*flo, *mlo), phi = std::max(*fhi, *mhi);
			std::cout << "\nPrice path (fundamental vs. book mid)\n"
					  << "  fundamental  " << sparkline(fund_series, plo, phi, 64) << "\n"
					  << "  book mid     " << sparkline(mid_series, plo, phi, 64) << "\n"
					  << "  range " << plo << " – " << phi << "\n";

			const double nt = static_cast<double>(n_trades);
			std::cout << "\nMarket statistics\n"
					  << "  events processed     " << n_events << "\n"
					  << "  orders / cancels     " << total_orders << " / " << total_cancels
					  << "   (order-to-trade " << static_cast<double>(total_orders) / nt << ")\n"
					  << "  trades               " << n_trades << "  (" << 100.0 * static_cast<double>(buy_aggr) / nt
					  << "% buyer-initiated)\n"
					  << "  volume / VWAP        " << traded_qty << " / " << notional / static_cast<double>(traded_qty)
					  << "   (end fundamental " << v_end << ")\n"
					  << "  avg trade / largest  " << static_cast<double>(traded_qty) / nt << " / " << max_trade << "\n"
					  << "  avg spread / depth   " << spread_sum / static_cast<double>(spread_n) << " / "
					  << depth_sum / static_cast<double>(spread_n) << " shares at the touch\n"
					  << "  1 s volatility       mid " << vol_mid << "   fundamental " << vol_fund << "\n"
					  << "  mean |mid - value|   " << track_err << "\n"
					  << "  peak resting orders  " << peak_orders << "\n";

			std::cout << "\nAgents (PnL marked at the final fundamental)\n"
					  << "  " << std::left << std::setw(10) << "agent" << std::right << std::setw(10) << "orders"
					  << std::setw(10) << "cancels" << std::setw(9) << "fills" << std::setw(10) << "volume"
					  << std::setw(11) << "inventory" << std::setw(12) << "PnL" << "\n";
			for (int i = 0; i < N_AGENTS; ++i)
			{
				const AgentStats &a = ag[i];
				std::cout << "  " << std::left << std::setw(10) << AGENT_NAME[i] << std::right << std::setw(10) << a.orders
						  << std::setw(10) << a.cancels << std::setw(9) << a.fills << std::setw(10) << a.volume
						  << std::setw(11) << a.inv << std::setw(12) << a.cash + static_cast<double>(a.inv) * v_end << "\n";
			}
			std::cout << "\nBook matched the shadow model throughout, drained clean, "
						 "inventory and cash netted to zero ✓\n";
		}
	};

} // namespace

int main()
{
	Simulation().run();
	return 0;
}
