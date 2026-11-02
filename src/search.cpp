// Modified from jpegultrascan 1.4.0 (choosebest, trybitsplits, trysplits, partitionscans and
// the final scan ordering), Copyright 2015-2024 Aaron Kaluszka, Apache-2.0.
// Changed: ported from Perl to C++; sizes come from a SizeSource instead of jpegtran runs.

#include "search.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <memory>

namespace jpegultrascan::detail {

namespace {

// perl's ~0, the starting minimum of trysplits and of the final choice
constexpr std::uint64_t kInfinity = std::numeric_limits<std::uint64_t>::max();

std::uint64_t add(std::uint64_t a, std::uint64_t b) { return a > kInfinity - b ? kInfinity : a + b; }

struct Scan {
    std::string plane;  // component indices separated by spaces, as the perl writes them
    int ss = 0;
    int se = 0;
    int ah = 0;
    int al = 0;
    bool sequential = false;

    std::string text() const {
        if (sequential) return plane + ";";
        return plane + ": " + std::to_string(ss) + ' ' + std::to_string(se) + ' ' + std::to_string(ah) + ' ' +
               std::to_string(al) + ';';
    }
};

// a candidate's scans are the perl's concatenated string: here a tree of concatenations,
// with a node made only when the candidate wins
struct Node {
    int left = -1;
    int right = -1;
    int scan = -1;
};

struct Candidate {
    std::uint64_t size = 0;  // 0 plays perl's undef
    std::size_t count = 0;   // the newlines choosebest counts
    int node = -1;           // -1 is the empty string
};

class Arena {
public:
    int leaf(Scan s) {
        scans_.push_back(std::move(s));
        nodes_.push_back({-1, -1, static_cast<int>(scans_.size() - 1)});
        return static_cast<int>(nodes_.size() - 1);
    }
    int concat(int a, int b) {
        if (a < 0) return b;
        if (b < 0) return a;
        nodes_.push_back({a, b, -1});
        return static_cast<int>(nodes_.size() - 1);
    }
    void collect(int node, std::vector<const Scan*>& out) const {
        // iterative in-order walk: the trees can be deep
        std::vector<int> stack;
        if (node >= 0) stack.push_back(node);
        while (!stack.empty()) {
            const Node& n = nodes_[static_cast<std::size_t>(stack.back())];
            stack.pop_back();
            if (n.scan >= 0) {
                out.push_back(&scans_[static_cast<std::size_t>(n.scan)]);
            } else {
                stack.push_back(n.right);
                stack.push_back(n.left);
            }
        }
    }
    std::string text(int node) const {
        std::vector<const Scan*> scans;
        collect(node, scans);
        std::string s;
        for (const Scan* scan : scans) s += scan->text() + "\n";
        return s;
    }

private:
    std::vector<Scan> scans_;
    std::vector<Node> nodes_;
};

enum class Pick { Left, Right, Neither };

// perl: a tie goes to the right side only when the left one has more scans, so between
// equal sizes and equal counts the candidate looked at last wins
Pick choosebest(std::uint64_t leftsize, std::size_t leftcount, std::uint64_t rightsize, std::size_t rightcount) {
    if (!leftsize && !rightsize) return Pick::Neither;
    if (!rightsize || (leftsize && leftsize < rightsize)) return Pick::Left;
    if (!leftsize || (rightsize && leftsize > rightsize) || leftcount > rightcount) return Pick::Right;
    return Pick::Left;
}

template <class MakeNode>
void consider(Candidate& best, std::uint64_t size, std::size_t count, MakeNode make_node) {
    switch (choosebest(size, count, best.size, best.count)) {
    case Pick::Left:
        best = {size, count, make_node()};
        break;
    case Pick::Right:
        break;
    case Pick::Neither:
        best = {};
        break;
    }
}

// trysplits and trybitsplits for one plane, memoized as in the perl
class BandSearch {
public:
    using SizeFn = std::function<std::uint64_t(int ss, int se, int ah, int al)>;

    BandSearch(Arena& arena, std::string plane, int max_bits, SizeFn size)
        : arena_(arena), plane_(std::move(plane)), max_bits_(max_bits), size_(std::move(size)),
          splits_(DCTSIZE2 * DCTSIZE2),
          bits_(static_cast<std::size_t>(DCTSIZE2 * DCTSIZE2 * pass_count(max_bits))) {}

    Candidate trybitsplits(int a, int b, int c, int d) {
        auto& slot = bits_[(static_cast<std::size_t>(a) * DCTSIZE2 + static_cast<std::size_t>(b)) *
                               static_cast<std::size_t>(pass_count(max_bits_)) +
                           static_cast<std::size_t>(pass_index(max_bits_, c, d))];
        if (slot) return *slot;
        Candidate min{size_(a, b, c, d), 1, arena_.leaf({plane_, a, b, c, d, false})};
        for (int s = a; s < b; ++s) {
            const Candidate x = trybitsplits(a, s, c, d);
            const Candidate y = trybitsplits(s + 1, b, c, d);
            consider(min, add(x.size, y.size), x.count + y.count, [&] { return arena_.concat(x.node, y.node); });
        }
        slot = min;
        return min;
    }

    Candidate trysplits(int a, int b) {
        auto& slot = splits_[static_cast<std::size_t>(a) * DCTSIZE2 + static_cast<std::size_t>(b)];
        if (slot) return *slot;
        Candidate min{kInfinity, 0, -1};
        std::vector<Candidate> parts;
        for (int n = 0; n <= max_bits_; ++n) {
            parts.clear();
            parts.push_back(trybitsplits(a, b, 0, n));
            std::uint64_t size = parts.back().size;
            std::size_t count = parts.back().count;
            for (int i = n; i > 0; --i) {
                parts.push_back(trybitsplits(a, b, i, i - 1));
                size = add(size, parts.back().size);
                count += parts.back().count;
            }
            consider(min, size, count, [&] {
                int node = -1;
                for (const Candidate& p : parts) node = arena_.concat(node, p.node);
                return node;
            });
        }
        for (int s = a; s < b; ++s) {
            const Candidate x = trysplits(a, s);
            const Candidate y = trysplits(s + 1, b);
            consider(min, add(x.size, y.size), x.count + y.count, [&] { return arena_.concat(x.node, y.node); });
        }
        slot = min;
        return min;
    }

private:
    Arena& arena_;
    std::string plane_;
    int max_bits_;
    SizeFn size_;
    std::vector<std::optional<Candidate>> splits_;
    std::vector<std::optional<Candidate>> bits_;
};

std::vector<int> components_of(const std::string& plane) {
    std::vector<int> out;
    int v = -1;
    for (const char c : plane) {
        if (c >= '0' && c <= '9') {
            v = (v < 0 ? 0 : v * 10) + (c - '0');
        } else if (v >= 0) {
            out.push_back(v);
            v = -1;
        }
    }
    if (v >= 0) out.push_back(v);
    return out;
}

// the fields the perl's final ordering reads from a line: /^([^:]*)(?:: (\d+) (\d+) \d+ (\d+))?/,
// with the missing ones of a sequential line taken as 0
struct Line {
    std::string prefix;
    bool has_numbers = false;
    int ss = 0;
    int se = 0;
    int al = 0;
    const Scan* scan = nullptr;
};

Line parse_line(const Scan& s) {
    Line l;
    l.scan = &s;
    if (s.sequential) {
        l.prefix = s.plane + ";";
    } else {
        l.prefix = s.plane;
        l.has_numbers = true;
        l.ss = s.ss;
        l.se = s.se;
        l.al = s.al;
    }
    return l;
}

}  // namespace

AcSizes::AcSizes(int max_bits)
    : max_bits_(max_bits), sizes_(static_cast<std::size_t>(DCTSIZE2 * DCTSIZE2 * pass_count(max_bits)), 0) {}

void VerboseLog::scan(const std::string& text, std::uint64_t size) {
    if (!seen.insert(text).second) return;
    std::string line = text;
    if (static_cast<int>(line.size()) < width) line.append(static_cast<std::size_t>(width) - line.size(), ' ');
    std::string number = std::to_string(size);
    if (static_cast<int>(number.size()) < width2) number.insert(0, static_cast<std::size_t>(width2) - number.size(), ' ');
    sizes.push_back(line + ' ' + number);
}

std::optional<SearchResult> search(SizeSource& source, int num_components, const SearchOptions& options,
                                   std::string& error) {
    const int planes = num_components - 1;
    const int max_bits = options.max_bits;
    VerboseLog* log = options.log;
    Arena arena;

    // ac scans, one component at a time
    Candidate acall{0, 0, -1};
    for (int p = 0; p <= planes; ++p) {
        if (log) log->summary.push_back("Calculating sizes of AC scans, plane " + std::to_string(p));
        auto sizes = std::make_shared<AcSizes>(max_bits);
        if (!source.measure_ac(p, *sizes)) {
            error = std::string("measuring AC scans failed: ") + source.error();
            return std::nullopt;
        }
        if (log) log->summary.push_back("Searching for best AC scan, plane " + std::to_string(p));
        BandSearch bands(arena, std::to_string(p), max_bits,
                         [sizes](int ss, int se, int ah, int al) { return sizes->get(ss, se, ah, al); });
        const Candidate best = bands.trysplits(1, kMaxCoef);
        acall = {add(acall.size, best.size), acall.count + best.count, arena.concat(acall.node, best.node)};
        if (log) {
            log->summary.push_back("Best AC scan, plane " + std::to_string(p) + ":");
            std::vector<const Scan*> scans;
            arena.collect(best.node, scans);
            for (const Scan* s : scans) log->summary.push_back(s->text());
            log->summary.push_back("Size: " + std::to_string(best.size));
        }
    }

    // dc scans and the sequential scripts, partition by partition
    if (log) log->summary.push_back("Calculating sizes and searching for best DC scan");
    Candidate best{kInfinity, 0, -1};
    std::map<std::string, Candidate> dc_best;
    std::map<std::string, std::uint64_t> sequential;
    const auto partition = [&](const std::vector<std::string>& parts) -> bool {
        Candidate dcall{0, 0, -1};
        for (const std::string& part : parts) {
            auto it = dc_best.find(part);
            if (it == dc_best.end()) {
                auto dc = std::make_shared<std::vector<std::uint64_t>>(static_cast<std::size_t>(pass_count(max_bits)), 0);
                if (!source.measure_dc(components_of(part), *dc)) {
                    error = std::string("measuring DC scans failed: ") + source.error();
                    return false;
                }
                BandSearch dcsearch(arena, part, max_bits, [dc, max_bits](int, int, int ah, int al) {
                    return (*dc)[static_cast<std::size_t>(pass_index(max_bits, ah, al))];
                });
                it = dc_best.emplace(part, dcsearch.trysplits(0, 0)).first;
            }
            const Candidate& c = it->second;
            dcall = {add(dcall.size, c.size), dcall.count + c.count, arena.concat(dcall.node, c.node)};
        }
        consider(best, add(dcall.size, acall.size), dcall.count + acall.count,
                 [&] { return arena.concat(dcall.node, acall.node); });

        bool measured = true;
        for (const std::string& part : parts) measured = measured && sequential.count(part + ";");
        if (!measured) {
            std::vector<std::vector<int>> comps;
            for (const std::string& part : parts) comps.push_back(components_of(part));
            std::vector<std::uint64_t> sizes;
            if (!source.measure_sequential(comps, sizes) || sizes.size() != parts.size()) {
                error = std::string("measuring sequential scans failed: ") + source.error();
                return false;
            }
            for (std::size_t i = 0; i < parts.size(); ++i) sequential.emplace(parts[i] + ";", sizes[i]);
        }
        std::uint64_t allsize = 0;
        for (const std::string& part : parts) allsize = add(allsize, sequential[part + ";"]);
        consider(best, allsize, parts.size(), [&] {
            int node = -1;
            for (const std::string& part : parts) node = arena.concat(node, arena.leaf({part, 0, 0, 0, 0, true}));
            return node;
        });
        return true;
    };
    std::string all;
    std::vector<std::string> separate;
    for (int p = 0; p <= planes; ++p) {
        all += (p ? " " : "") + std::to_string(p);
        separate.push_back(std::to_string(p));
    }
    if (planes > 0 && options.dc == DcMode::Upstream && !partition(separate)) return std::nullopt;
    if (!partition({all})) return std::nullopt;
    if (best.node < 0 || !best.size || best.size == kInfinity) {
        error = "no scan script";
        return std::nullopt;
    }

    std::vector<const Scan*> chosen;
    arena.collect(best.node, chosen);
    if (log) {
        // perl: the best script with the AC part cut out, when it has one
        const std::string text = arena.text(best.node);
        const std::string ac = arena.text(acall.node);
        const std::size_t at = text.find(ac);
        if (at != std::string::npos && !ac.empty()) {
            const std::string dc = text.substr(0, at) + text.substr(at + ac.size());
            log->summary.push_back("Best DC scan:");
            std::size_t start = 0;
            while (start < dc.size()) {
                const std::size_t end = dc.find('\n', start);
                log->summary.push_back(dc.substr(start, end - start));
                start = end + 1;
            }
            log->summary.push_back("Size: " + std::to_string(best.size - acall.size));
        }
    }

    // stable sort by Se, then by plane; sequential lines by their text
    std::vector<Line> lines;
    for (const Scan* s : chosen) lines.push_back(parse_line(*s));
    std::stable_sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) {
        if (!a.has_numbers || !b.has_numbers) return a.prefix < b.prefix;
        if (a.se != b.se) return a.se < b.se;
        return a.prefix < b.prefix;
    });

    // i goes before j when it codes higher bits of an overlapping band of the same plane, or
    // when j's plane sorts after i's and j reaches at least as far as i
    const std::size_t n = lines.size();
    std::vector<std::vector<std::size_t>> prev(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (std::size_t j = 0; j < n; ++j) {
            if (i == j) continue;
            const Line& a = lines[i];
            const Line& b = lines[j];
            const bool overlap = (a.ss <= b.ss && b.ss <= a.se) || (b.ss <= a.ss && a.ss <= b.se);
            if ((b.prefix == a.prefix && overlap && a.al > b.al) || (b.prefix > a.prefix && b.se >= a.se)) {
                prev[j].push_back(i);
            }
        }
    }
    // repeatedly take the first line, in sorted order, whose predecessors are all out
    std::vector<std::size_t> order;
    std::vector<bool> done(n, false);
    while (order.size() < n) {
        bool progress = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (done[i]) continue;
            if (!std::all_of(prev[i].begin(), prev[i].end(), [&](std::size_t k) { return done[k]; })) continue;
            order.push_back(i);
            done[i] = true;
            progress = true;
            break;
        }
        if (!progress) {
            error = "scan order has a cycle";
            return std::nullopt;
        }
    }

    SearchResult result;
    result.bytes = best.size;
    for (const std::size_t i : order) {
        const Scan& s = *lines[i].scan;
        jpeg_scan_info info{};
        const std::vector<int> comps = components_of(s.plane);
        if (comps.empty() || comps.size() > MAX_COMPS_IN_SCAN) {
            error = "bad scan " + s.text();
            return std::nullopt;
        }
        info.comps_in_scan = static_cast<int>(comps.size());
        for (std::size_t k = 0; k < comps.size(); ++k) info.component_index[k] = comps[k];
        if (s.sequential) {
            info.Ss = 0;
            info.Se = kMaxCoef;
        } else {
            info.Ss = s.ss;
            info.Se = s.se;
            info.Ah = s.ah;
            info.Al = s.al;
        }
        result.scans.push_back(info);
    }
    return result;
}

}  // namespace jpegultrascan::detail
