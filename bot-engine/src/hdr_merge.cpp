// hdr_merge.cpp — Unified HDR snapshot merger and ranking tool
//
// Consumes per-bot CSV snapshots (produced by bot --snapshot-dir) and
// produces:
//   1. unified.csv      — time-series merged across all bots
//   2. ranking.csv      — final percentile snapshot per bot, ranked
//   3. Console summary  — headline numbers
//
// Why this exists:
//   Track 3's leaderboard (and any post-mortem analysis) needs a single
//   merged view across the entire bot fleet, not N independent CSVs. This
//   tool produces that view using HDR's mathematical additivity property:
//   percentiles of merged histograms are EXACT, not approximate.
//
// Important: this tool does NOT recompute percentiles by averaging.
// Averaging percentiles is mathematically meaningless. Instead, this tool
// uses sum-of-counts and recomputes percentile boundaries — the only
// statistically correct way to merge HDR data. Since we're reading derived
// percentile snapshots (not raw HDR blobs), we use a different approach:
// merge at the SAME percentile rank, taking the worst-case (max) across
// all bots. This is the conservative, correct presentation per the
// telemetry contract: "never average percentiles."
//
// Build (from bot-engine/):
//   g++ -std=c++20 -O2 -Wall -Wextra src/hdr_merge.cpp -o build/hdr_merge

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <filesystem>
#include <algorithm>
#include <iomanip>
#include <cstdint>

namespace fs = std::filesystem;

// One row from a bot CSV — matches the 15-column schema written by bot.cpp
struct SnapshotRow {
    int64_t elapsed_sec;
    uint64_t sent;
    uint64_t acked;
    int64_t naive_p50, naive_p90, naive_p99, naive_p99_9, naive_p99_99, naive_max;
    int64_t co_p50, co_p90, co_p99, co_p99_9, co_p99_99, co_max;
};

// Parse one CSV file. Skips header, returns rows in order.
static std::vector<SnapshotRow> parse_csv(const std::string& path) {
    std::vector<SnapshotRow> rows;
    std::ifstream in(path);
    if (!in) {
        std::cerr << "warning: cannot open " << path << "\n";
        return rows;
    }

    std::string line;
    std::getline(in, line); // skip header

    while (std::getline(in, line)) {
        if (line.empty()) continue;

        SnapshotRow r{};
        std::stringstream ss(line);
        std::string field;

        // Strict 15-column parse — if any field is missing, skip the row
        auto next = [&]() -> bool {
            return static_cast<bool>(std::getline(ss, field, ','));
        };

        // Wrap parsing in try/catch — partial/corrupt rows (e.g. a bot
        // crashed mid-write) throw std::invalid_argument/out_of_range
        // from stoll/stoull. We log and skip the row rather than
        // crashing the entire merge tool and destroying fleet telemetry.
        bool ok = true;
        try {
            if (!next()) { ok = false; } else r.elapsed_sec    = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.sent           = std::stoull(field);
            if (ok && !next()) { ok = false; } else if (ok) r.acked          = std::stoull(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_p50      = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_p90      = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_p99      = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_p99_9    = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_p99_99   = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.naive_max      = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_p50         = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_p90         = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_p99         = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_p99_9       = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_p99_99      = std::stoll(field);
            if (ok && !next()) { ok = false; } else if (ok) r.co_max         = std::stoll(field);
        } catch (const std::exception& e) {
            std::cerr << "warning: skipping corrupt row in " << path
                      << " (" << e.what() << ")\n";
            ok = false;
        }

        if (ok) rows.push_back(r);
    }
    return rows;
}

// Merge strategy at a single time-step:
//   - sent/acked: SUM across bots (aggregate throughput)
//   - all percentiles: MAX across bots (conservative worst-case at this rank)
//
// Why max-of-percentiles and not arithmetic mean?
//   The telemetry contract is explicit: averaging percentiles is meaningless
//   because it discards distribution shape. The conservative honest answer
//   is the worst observed value at that rank — what a contestant would see
//   from the slowest bot. This matches how Aeron and wrk2 report fleet
//   latencies. For TRUE statistically-correct merging, the bots' raw HDR
//   blobs should be merged additively (which the bot already does at end-
//   of-run via hdr_add). This CSV-merge is for time-series visualization,
//   where max-of-percentiles is the standard convention.
static SnapshotRow merge_rows(const std::vector<SnapshotRow>& rows) {
    SnapshotRow out{};
    if (rows.empty()) return out;

    out.elapsed_sec = rows.front().elapsed_sec;
    for (const auto& r : rows) {
        out.sent          += r.sent;
        out.acked         += r.acked;
        out.naive_p50      = std::max(out.naive_p50,    r.naive_p50);
        out.naive_p90      = std::max(out.naive_p90,    r.naive_p90);
        out.naive_p99      = std::max(out.naive_p99,    r.naive_p99);
        out.naive_p99_9    = std::max(out.naive_p99_9,  r.naive_p99_9);
        out.naive_p99_99   = std::max(out.naive_p99_99, r.naive_p99_99);
        out.naive_max      = std::max(out.naive_max,    r.naive_max);
        out.co_p50         = std::max(out.co_p50,       r.co_p50);
        out.co_p90         = std::max(out.co_p90,       r.co_p90);
        out.co_p99         = std::max(out.co_p99,       r.co_p99);
        out.co_p99_9       = std::max(out.co_p99_9,     r.co_p99_9);
        out.co_p99_99      = std::max(out.co_p99_99,    r.co_p99_99);
        out.co_max         = std::max(out.co_max,       r.co_max);
    }
    return out;
}

// Composite score (per the leaderboard scoring contract):
//   score = lower CO p99 + higher throughput → better
// We use a simple normalized formula. Final ranking will be done by the
// leaderboard service against the live field; this gives a useful
// post-hoc ordering for debugging.
static double composite_score(const SnapshotRow& r) {
    if (r.co_p99 == 0 || r.acked == 0) return 0.0;
    // Lower latency = higher score; higher throughput = higher score.
    // Units: p99 in nanoseconds → seconds; acked in count.
    double lat_sec = static_cast<double>(r.co_p99) / 1e9;
    double tput    = static_cast<double>(r.acked);
    return (tput / 1000.0) / (lat_sec * 1000.0 + 1.0);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "usage: hdr_merge <snapshot_dir> [output_prefix]\n"
                  << "  Reads bot_*.csv from snapshot_dir, writes\n"
                  << "  <prefix>_unified.csv and <prefix>_ranking.csv\n"
                  << "  default prefix: merged\n";
        return 1;
    }

    std::string in_dir = argv[1];
    std::string prefix = (argc >= 3) ? argv[2] : "merged";

    // ── Discover bot CSV files ────────────────────────────────────────────
    std::vector<fs::path> bot_files;
    for (const auto& entry : fs::directory_iterator(in_dir)) {
        std::string name = entry.path().filename().string();
        if (name.rfind("bot_", 0) == 0 && name.size() > 4 &&
            name.substr(name.size() - 4) == ".csv") {
            bot_files.push_back(entry.path());
        }
    }
    std::sort(bot_files.begin(), bot_files.end());

    if (bot_files.empty()) {
        std::cerr << "error: no bot_*.csv files found in " << in_dir << "\n";
        return 2;
    }

    std::cout << "Found " << bot_files.size() << " bot CSV files:\n";
    for (const auto& p : bot_files) std::cout << "  " << p.filename() << "\n";
    std::cout << "\n";

    // ── Load all bots ─────────────────────────────────────────────────────
    std::vector<std::vector<SnapshotRow>> all_bots;
    std::vector<std::string> bot_names;
    size_t min_rows = SIZE_MAX;

    for (const auto& p : bot_files) {
        auto rows = parse_csv(p.string());
        if (rows.empty()) continue;
        all_bots.push_back(rows);
        bot_names.push_back(p.stem().string());  // "bot_0", "bot_1", ...
        min_rows = std::min(min_rows, rows.size());
    }

    if (all_bots.empty()) {
        std::cerr << "error: all CSV files were empty or unreadable\n";
        return 3;
    }

    // ── Build unified time-series ─────────────────────────────────────────
    std::string unified_path = prefix + "_unified.csv";
    std::ofstream unified(unified_path);
    unified << "elapsed_sec,total_sent,total_acked,"
            << "naive_p50,naive_p90,naive_p99,naive_p99_9,naive_p99_99,naive_max,"
            << "co_p50,co_p90,co_p99,co_p99_9,co_p99_99,co_max\n";

    for (size_t t = 0; t < min_rows; t++) {
        std::vector<SnapshotRow> at_t;
        for (const auto& bot : all_bots) at_t.push_back(bot[t]);
        SnapshotRow merged = merge_rows(at_t);

        unified << merged.elapsed_sec << ","
                << merged.sent << "," << merged.acked << ","
                << merged.naive_p50 << "," << merged.naive_p90 << ","
                << merged.naive_p99 << "," << merged.naive_p99_9 << ","
                << merged.naive_p99_99 << "," << merged.naive_max << ","
                << merged.co_p50 << "," << merged.co_p90 << ","
                << merged.co_p99 << "," << merged.co_p99_9 << ","
                << merged.co_p99_99 << "," << merged.co_max << "\n";
    }
    unified.close();

    // ── Build final-snapshot ranking ──────────────────────────────────────
    // Use each bot's LAST row as its final snapshot. Rank by composite score.
    struct Ranked {
        std::string name;
        SnapshotRow final_row;
        double score;
    };
    std::vector<Ranked> ranked;
    for (size_t i = 0; i < all_bots.size(); i++) {
        SnapshotRow last = all_bots[i].back();
        ranked.push_back({bot_names[i], last, composite_score(last)});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.score > b.score; });

    std::string ranking_path = prefix + "_ranking.csv";
    std::ofstream ranking(ranking_path);
    ranking << "rank,bot,score,sent,acked,co_p50,co_p99,co_p99_9,co_max\n";
    for (size_t i = 0; i < ranked.size(); i++) {
        const auto& r = ranked[i];
        ranking << (i + 1) << "," << r.name << ","
                << std::fixed << std::setprecision(4) << r.score << ","
                << r.final_row.sent << "," << r.final_row.acked << ","
                << r.final_row.co_p50 << "," << r.final_row.co_p99 << ","
                << r.final_row.co_p99_9 << "," << r.final_row.co_max << "\n";
    }
    ranking.close();

    // ── Console summary ───────────────────────────────────────────────────
    std::cout << "=================================================================\n";
    std::cout << " MERGED FLEET SUMMARY — " << all_bots.size() << " bots, "
              << min_rows << " time samples\n";
    std::cout << "=================================================================\n";

    // Final merged snapshot (across all bots, at last time sample)
    std::vector<SnapshotRow> final_at_t;
    for (const auto& bot : all_bots) final_at_t.push_back(bot.back());
    SnapshotRow final_merged = merge_rows(final_at_t);

    std::cout << "Total sent across fleet:  " << final_merged.sent << "\n";
    std::cout << "Total acked across fleet: " << final_merged.acked << "\n";
    std::cout << "Worst-case CO percentiles (ns):\n";
    std::cout << "  p50:    " << final_merged.co_p50    << "\n";
    std::cout << "  p90:    " << final_merged.co_p90    << "\n";
    std::cout << "  p99:    " << final_merged.co_p99    << "\n";
    std::cout << "  p99.9:  " << final_merged.co_p99_9  << "\n";
    std::cout << "  p99.99: " << final_merged.co_p99_99 << "\n";
    std::cout << "  max:    " << final_merged.co_max    << "\n";
    std::cout << "\n";

    std::cout << "─── PER-BOT FINAL RANKING (composite score) ───\n";
    std::cout << std::left << std::setw(6)  << "Rank"
              << std::setw(10) << "Bot"
              << std::setw(10) << "Score"
              << std::setw(12) << "Acked"
              << std::setw(12) << "CO_p99(ns)" << "\n";
    for (size_t i = 0; i < ranked.size(); i++) {
        const auto& r = ranked[i];
        std::cout << std::left << std::setw(6) << (i + 1)
                  << std::setw(10) << r.name
                  << std::setw(10) << std::fixed << std::setprecision(4) << r.score
                  << std::setw(12) << r.final_row.acked
                  << std::setw(12) << r.final_row.co_p99 << "\n";
    }

    std::cout << "\nOutputs written:\n";
    std::cout << "  " << unified_path << " (time-series for Track 3)\n";
    std::cout << "  " << ranking_path << " (final ranking)\n";

    return 0;
}
