#pragma once

// Streaming sketches for approximate aggregates: fixed or logarithmic memory
// per group whatever the row count, and mergeable, so parallel morsels can
// each build one and combine them.

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace pkql {

// HyperLogLog distinct counter (Flajolet et al. 2007, with the sparse start
// of HyperLogLog++, Heule et al., EDBT 2013): small sets are kept exactly as
// a set of 64-bit hashes; past kSparseMax it switches to 2^14 one-byte
// registers (16 KB, ~0.8% standard error), with linear counting for small
// estimates.
class HyperLogLog {
 public:
  static constexpr size_t kSparseMax = 2048;

  void add(uint64_t hash) {
    hash = mix(hash);
    if (reg_.empty()) {
      sparse_.insert(hash);
      if (sparse_.size() > kSparseMax) densify();
      return;
    }
    addDense(hash);
  }

  void merge(HyperLogLog& o) {
    if (o.reg_.empty()) {
      for (uint64_t h : o.sparse_) {
        if (reg_.empty()) {
          sparse_.insert(h);
          if (sparse_.size() > kSparseMax) densify();
        } else {
          addDense(h);
        }
      }
      return;
    }
    if (reg_.empty()) densify();
    for (size_t i = 0; i < kM; i++) reg_[i] = std::max(reg_[i], o.reg_[i]);
  }

  uint64_t estimate() const {
    if (reg_.empty()) return sparse_.size();  // exact
    double sum = 0;
    size_t zeros = 0;
    for (uint8_t r : reg_) {
      sum += std::ldexp(1.0, -r);
      zeros += r == 0;
    }
    double m = static_cast<double>(kM);
    double e = 0.7213 / (1 + 1.079 / m) * m * m / sum;
    if (e <= 2.5 * m && zeros > 0) e = m * std::log(m / static_cast<double>(zeros));
    return static_cast<uint64_t>(std::llround(e));
  }

  // splitmix64 finalizer: std::hash of an integer is the identity.
  static uint64_t mix(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    return x ^ (x >> 31);
  }

 private:
  static constexpr int kBits = 14;
  static constexpr size_t kM = size_t{1} << kBits;

  void addDense(uint64_t h) {  // h is already mixed
    size_t idx = static_cast<size_t>(h >> (64 - kBits));
    uint64_t rest = (h << kBits) | (1ull << (kBits - 1));  // a guard bit bounds the rank
    uint8_t rank = static_cast<uint8_t>(std::countl_zero(rest) + 1);
    if (rank > reg_[idx]) reg_[idx] = rank;
  }
  void densify() {
    reg_.assign(kM, 0);
    for (uint64_t h : sparse_) addDense(h);
    sparse_.clear();
  }

  std::unordered_set<uint64_t> sparse_;
  std::vector<uint8_t> reg_;  // empty while sparse
};

// DDSketch (Masson, Rim and Lee, "DDSketch: A Fast and Fully-Mergeable
// Quantile Sketch with Relative-Error Guarantees", VLDB 2019): a value x > 0
// is counted in bucket ceil(log_gamma(x)), gamma = (1 + a) / (1 - a). Every
// value in a bucket is within relative error a of the bucket's
// representative, so any quantile is answered within a of the true value
// at that rank. Buckets are kept in dense arrays (one for positive values,
// one for negative magnitudes) that grow to cover the indexes seen; their
// size grows with log(max / min), not with the number of values.
class DDSketch {
 public:
  static constexpr double kAlpha = 0.01;  // 1% relative accuracy

  void add(double x) {
    count_++;
    if (x > kMinValue) pos_.add(index(x));
    else if (x < -kMinValue) neg_.add(index(-x));
    else zeros_++;
  }

  void merge(const DDSketch& o) {
    count_ += o.count_;
    zeros_ += o.zeros_;
    pos_.merge(o.pos_);
    neg_.merge(o.neg_);
  }

  uint64_t count() const { return count_; }

  // The value at rank floor(q * (count - 1)) (0-based, ascending), within
  // kAlpha relative error. Requires count() > 0.
  double quantile(double q) const {
    uint64_t rank = static_cast<uint64_t>(q * static_cast<double>(count_ - 1));
    // Ascending order: negatives from the largest magnitude down, then zeros, then positives.
    uint64_t seen = 0;
    for (size_t i = neg_.counts.size(); i-- > 0;) {
      seen += neg_.counts[i];
      if (seen > rank) return -value(neg_.offset + static_cast<int32_t>(i));
    }
    seen += zeros_;
    if (seen > rank) return 0.0;
    for (size_t i = 0; i < pos_.counts.size(); i++) {
      seen += pos_.counts[i];
      if (seen > rank) return value(pos_.offset + static_cast<int32_t>(i));
    }
    return pos_.counts.empty() ? 0.0 : value(pos_.offset + static_cast<int32_t>(pos_.counts.size() - 1));
  }

 private:
  static constexpr double kMinValue = 1e-12;  // smaller magnitudes count as zero
  static double gamma() { return (1 + kAlpha) / (1 - kAlpha); }
  static double logGamma() {
    static const double lg = std::log(gamma());
    return lg;
  }
  static int32_t index(double x) { return static_cast<int32_t>(std::ceil(std::log(x) / logGamma())); }
  // The bucket's representative: 2 g^i / (g + 1), within kAlpha of any
  // value in (g^(i-1), g^i].
  static double value(int32_t i) { return 2 * std::pow(gamma(), i) / (gamma() + 1); }

  struct Store {
    std::vector<uint64_t> counts;  // counts[k] is bucket offset + k
    int32_t offset = 0;
    void add(int32_t i, uint64_t n = 1) {
      if (counts.empty()) {
        offset = i;
        counts.assign(1, 0);
      } else if (i < offset) {
        counts.insert(counts.begin(), static_cast<size_t>(offset - i), 0);
        offset = i;
      } else if (static_cast<size_t>(i - offset) >= counts.size()) {
        counts.resize(static_cast<size_t>(i - offset) + 1, 0);
      }
      counts[static_cast<size_t>(i - offset)] += n;
    }
    void merge(const Store& o) {
      for (size_t k = 0; k < o.counts.size(); k++) {
        if (o.counts[k]) add(o.offset + static_cast<int32_t>(k), o.counts[k]);
      }
    }
  };

  uint64_t count_ = 0, zeros_ = 0;
  Store pos_, neg_;
};

}  // namespace pkql
