#include "pkql/colstore.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "pkql/codec.h"

namespace pkql {

namespace {

// Column encodings in a block.
constexpr uint8_t kIntFor = 1;       // zigzag min, width, n bit-packed (value - min)
constexpr uint8_t kIntDelta = 2;     // zigzag first, zigzag min delta, width, n-1 bit-packed (delta - min delta)
constexpr uint8_t kFloatScaled = 3;  // exponent e, then kIntFor of round(x * 10^e)
constexpr uint8_t kFloatRaw = 4;     // n x 8 bytes
constexpr uint8_t kTextDict = 5;     // kIntFor of dictionary codes
constexpr uint8_t kTextPlain = 6;    // n x (varint length, bytes)

const double kPow10[] = {1, 10, 100, 1000, 10000, 100000, 1000000};

void putVarint(uint64_t x, std::string& out) {
  while (x >= 0x80) {
    out += static_cast<char>((x & 0x7F) | 0x80);
    x >>= 7;
  }
  out += static_cast<char>(x);
}

uint64_t getVarint(std::string_view& in) {
  uint64_t x = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    if (in.empty()) throw Error("corrupt column block");
    uint8_t b = static_cast<uint8_t>(in[0]);
    in.remove_prefix(1);
    x |= static_cast<uint64_t>(b & 0x7F) << shift;
    if (!(b & 0x80)) return x;
  }
  throw Error("corrupt column block");
}

uint64_t zigzag(int64_t v) { return (static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63); }
int64_t unzigzag(uint64_t u) { return static_cast<int64_t>(u >> 1) ^ -static_cast<int64_t>(u & 1); }

int bitWidth(uint64_t x) { return 64 - std::countl_zero(x); }

// Little-endian bit stream: values of `width` bits, back to back.
void packBits(const std::vector<uint64_t>& v, int width, std::string& out) {
  if (width == 0) return;
  uint64_t acc = 0;
  int bits = 0;
  for (uint64_t x : v) {
    int w = width;
    while (w > 0) {
      int k = std::min(w, 32);
      acc |= (x & ((1ull << k) - 1)) << bits;  // k <= 32 and bits < 8: fits
      bits += k;
      x >>= k;
      w -= k;
      while (bits >= 8) {
        out += static_cast<char>(acc & 0xFF);
        acc >>= 8;
        bits -= 8;
      }
    }
  }
  if (bits > 0) out += static_cast<char>(acc & 0xFF);
}

// Reads n values of `width` bits; calls emit(i, value).
template <typename Emit>
void unpackBits(std::string_view& in, size_t n, int width, Emit emit) {
  if (width == 0) {
    for (size_t i = 0; i < n; i++) emit(i, 0);
    return;
  }
  size_t bytes = (n * static_cast<size_t>(width) + 7) / 8;
  if (in.size() < bytes) throw Error("corrupt column block");
  const uint8_t* p = reinterpret_cast<const uint8_t*>(in.data());
  if (width <= 56) {
    // Each value lies within 8 bytes from its first byte: load them (padded
    // at the end of the stream) and shift.
    uint64_t mask = (1ull << width) - 1;
    size_t bit = 0;
    for (size_t i = 0; i < n; i++, bit += static_cast<size_t>(width)) {
      size_t byte = bit >> 3;
      uint64_t word = 0;
      size_t take = std::min<size_t>(8, bytes - byte);
      std::memcpy(&word, p + byte, take);  // little-endian hosts (x86, ARM)
      emit(i, (word >> (bit & 7)) & mask);
    }
  } else {
    size_t bit = 0;
    for (size_t i = 0; i < n; i++) {
      uint64_t x = 0;
      for (int b = 0; b < width; b++, bit++) x |= static_cast<uint64_t>((p[bit >> 3] >> (bit & 7)) & 1) << b;
      emit(i, x);
    }
  }
  in.remove_prefix(bytes);
}

// Patched frame of reference (PFOR; Zukowski et al., "Super-Scalar RAM-CPU
// Cache Compression", ICDE 2006) of n ints (null slots hold any value):
// values minus the minimum, bit-packed at the width that makes the whole
// column smallest; the few values too wide for it (outliers such as a
// latency spike) are stored apart as (position, value) exceptions, so one
// outlier does not widen every value.
//   zigzag base, width, exception count, n packed values (0 for exceptions),
//   then per exception: position delta, value - base mod 2^64 (varints)
void putIntFor(const std::vector<int64_t>& v, std::string& out) {
  size_t n = v.size();
  // The base is also chosen robustly: a low percentile, so that an outlier
  // below the bulk (INT64_MIN, say) becomes an exception instead of moving
  // every value far from the base. Values below the base wrap around to
  // large unsigned differences and are exceptions like those far above.
  std::vector<int64_t> sorted = v;
  std::sort(sorted.begin(), sorted.end());
  int64_t candidates[3] = {n ? sorted[0] : 0, n ? sorted[n / 1000] : 0, n ? sorted[n / 100] : 0};
  int64_t base = 0;
  int width = 64;
  double best_cost = 1e300;
  for (int64_t b : candidates) {
    size_t by_width[65] = {0};  // how many values need exactly w bits above b
    for (int64_t x : v) by_width[bitWidth(static_cast<uint64_t>(x) - static_cast<uint64_t>(b))]++;
    // Bytes for width w: n * w / 8 packed, plus about 6 per exception.
    size_t wider = 0;  // values needing more than w bits
    for (int w = 64; w >= 0; w--) {
      double cost = static_cast<double>(n) * w / 8.0 + 6.0 * static_cast<double>(wider);
      if (cost < best_cost) {
        best_cost = cost;
        base = b;
        width = w;
      }
      wider += by_width[w];
    }
  }
  uint64_t limit = width == 64 ? ~0ull : (1ull << width) - 1;
  std::vector<uint64_t> packed(n);
  std::vector<size_t> exceptions;
  for (size_t i = 0; i < n; i++) {
    uint64_t u = static_cast<uint64_t>(v[i]) - static_cast<uint64_t>(base);
    if (u > limit) exceptions.push_back(i);
    else packed[i] = u;
  }
  putVarint(zigzag(base), out);
  out += static_cast<char>(width);
  putVarint(exceptions.size(), out);
  packBits(packed, width, out);
  size_t prev = 0;
  for (size_t i : exceptions) {
    putVarint(i - prev, out);
    putVarint(static_cast<uint64_t>(v[i]) - static_cast<uint64_t>(base), out);
    prev = i;
  }
}

template <typename Emit>
void getIntFor(std::string_view& in, size_t n, Emit emit) {
  int64_t mn = unzigzag(getVarint(in));
  if (in.empty()) throw Error("corrupt column block");
  int w = static_cast<uint8_t>(in[0]);
  in.remove_prefix(1);
  if (w > 64) throw Error("corrupt column block");
  size_t nexc = static_cast<size_t>(getVarint(in));
  if (nexc > n) throw Error("corrupt column block");
  if (nexc == 0) {
    unpackBits(in, n, w, [&](size_t i, uint64_t u) { emit(i, static_cast<int64_t>(static_cast<uint64_t>(mn) + u)); });
    return;
  }
  std::vector<uint64_t> u(n);
  unpackBits(in, n, w, [&](size_t i, uint64_t x) { u[i] = x; });
  size_t pos = 0;
  for (size_t e = 0; e < nexc; e++) {
    pos += static_cast<size_t>(getVarint(in));
    if (pos >= n) throw Error("corrupt column block");
    u[pos] = getVarint(in);
  }
  for (size_t i = 0; i < n; i++) emit(i, static_cast<int64_t>(static_cast<uint64_t>(mn) + u[i]));
}

void encodeInts(const std::vector<int64_t>& v, std::string& out) {
  std::string plain, delta;
  plain += static_cast<char>(kIntFor);
  putIntFor(v, plain);
  if (v.size() >= 2) {
    delta += static_cast<char>(kIntDelta);
    putVarint(zigzag(v[0]), delta);
    std::vector<int64_t> d(v.size() - 1);
    for (size_t i = 1; i < v.size(); i++) d[i - 1] = static_cast<int64_t>(static_cast<uint64_t>(v[i]) - static_cast<uint64_t>(v[i - 1]));
    putIntFor(d, delta);
  }
  out += !delta.empty() && delta.size() < plain.size() ? delta : plain;
}

void decodeInts(std::string_view& in, uint8_t tag, size_t n, int64_t* out) {
  if (tag == kIntFor) {
    getIntFor(in, n, [&](size_t i, int64_t x) { out[i] = x; });
  } else if (tag == kIntDelta) {
    int64_t cur = unzigzag(getVarint(in));
    if (n > 0) out[0] = cur;
    getIntFor(in, n > 0 ? n - 1 : 0, [&](size_t i, int64_t d) {
      cur = static_cast<int64_t>(static_cast<uint64_t>(cur) + static_cast<uint64_t>(d));
      out[i + 1] = cur;
    });
  } else {
    throw Error("corrupt column block: bad int encoding");
  }
}

}  // namespace

void BlockDrops::touch(const std::string& row_key, pkv::WriteBatch& batch) {
  if (!t_.blocks) return;
  const BlockMeta* m = t_.blocks->containing(row_key);
  if (!m || std::find(dropped_.begin(), dropped_.end(), m->first_key) != dropped_.end()) return;
  batch.remove(blockKey(t_, m->first_key));
  batch.remove(blockMetaKey(t_, m->first_key));
  dropped_.push_back(m->first_key);
}

void BlockDrops::commit() {
  if (!t_.blocks) return;
  for (const std::string& k : dropped_) {
    auto it = t_.blocks->by_first.find(k);
    if (it == t_.blocks->by_first.end()) continue;
    t_.blocks->covered_rows -= it->second.rows;
    t_.blocks->by_first.erase(it);
    t_.blocks->gaps = true;
  }
  dropped_.clear();
}

const BlockMeta* ColumnBlocks::containing(const std::string& row_key) const {
  auto it = by_first.upper_bound(row_key);
  if (it == by_first.begin()) return nullptr;
  --it;
  return row_key <= it->second.last_key ? &it->second : nullptr;
}

bool blocksEligible(const TableSchema& t) { return t.pk.size() == 1 && t.cols[t.pk[0]].type == Type::Int; }

std::string blockKey(const TableSchema& t, const std::string& first_row_key) {
  std::string k = tablePrefix(t.id);
  k[0] = 'c';
  k.append(first_row_key, keyPrefixLength(first_row_key), std::string::npos);
  return k;
}

std::string blockMetaPrefix(uint32_t table_id) {
  std::string k = "mb";
  for (int s = 24; s >= 0; s -= 8) k += static_cast<char>((table_id >> s) & 0xFF);
  return k;
}

std::string blockMetaKey(const TableSchema& t, const std::string& first_row_key) {
  std::string k = blockMetaPrefix(t.id);
  k.append(first_row_key, keyPrefixLength(first_row_key), std::string::npos);
  return k;
}

std::string firstRowKeyFromMetaKey(const TableSchema& t, std::string_view meta_key) {
  return tablePrefix(t.id) + std::string(meta_key.substr(blockMetaPrefix(t.id).size()));
}

std::string serializeBlockMeta(const BlockMeta& m) {
  Row r{Value::integer(m.rows), Value::text(m.last_key), Value::integer(static_cast<int64_t>(m.cols.size()))};
  for (const BlockColumnStats& c : m.cols) {
    r.push_back(Value::integer((c.has_range ? 1 : 0) | (c.has_nulls ? 2 : 0)));
    r.push_back(Value::integer(c.min));
    r.push_back(Value::integer(c.max));
  }
  std::string out;
  encodeRow(r, out);
  return out;
}

BlockMeta deserializeBlockMeta(std::string_view bytes, std::string first_row_key) {
  Row r = decodeRow(bytes);
  if (r.size() < 3) throw Error("corrupt block metadata");
  BlockMeta m;
  m.first_key = std::move(first_row_key);
  m.rows = static_cast<uint32_t>(r[0].asInt());
  m.last_key = r[1].asText();
  size_t ncols = static_cast<size_t>(r[2].asInt());
  if (r.size() != 3 + 3 * ncols) throw Error("corrupt block metadata");
  for (size_t c = 0; c < ncols; c++) {
    BlockColumnStats s;
    int64_t flags = r[3 + 3 * c].asInt();
    s.has_range = flags & 1;
    s.has_nulls = flags & 2;
    s.min = r[4 + 3 * c].asInt();
    s.max = r[5 + 3 * c].asInt();
    m.cols.push_back(s);
  }
  return m;
}

std::string encodeBlock(const TableSchema& t, const std::vector<std::string>& keys, const std::vector<Row>& rows,
                        BlockMeta* meta) {
  size_t n = rows.size();
  meta->first_key = keys.front();
  meta->last_key = keys.back();
  meta->rows = static_cast<uint32_t>(n);
  meta->cols.assign(t.cols.size(), BlockColumnStats());
  std::string out;
  putVarint(n, out);
  putVarint(t.cols.size(), out);
  std::string col;
  for (size_t c = 0; c < t.cols.size(); c++) {
    col.clear();
    BlockColumnStats& st = meta->cols[c];
    std::vector<uint8_t> nulls(n, 0);
    for (size_t i = 0; i < n; i++) nulls[i] = rows[i][c].isNull();
    st.has_nulls = std::find(nulls.begin(), nulls.end(), 1) != nulls.end();
    col += static_cast<char>(st.has_nulls ? 1 : 0);
    if (st.has_nulls) {
      std::string bitmap((n + 7) / 8, '\0');
      for (size_t i = 0; i < n; i++) {
        if (nulls[i]) bitmap[i >> 3] = static_cast<char>(bitmap[i >> 3] | (1 << (i & 7)));
      }
      col += bitmap;
    }
    Type type = t.cols[c].type;
    if (type == Type::Int || type == Type::Bool || type == Type::Null) {
      std::vector<int64_t> v(n, 0), present;
      for (size_t i = 0; i < n; i++) {
        if (nulls[i]) continue;
        int64_t x = type == Type::Bool ? (rows[i][c].asBool() ? 1 : 0) : rows[i][c].asInt();
        if (!st.has_range) st.min = st.max = x;
        st.has_range = true;
        st.min = std::min(st.min, x);
        st.max = std::max(st.max, x);
        v[i] = x;
        present.push_back(x);
      }
      // Null slots get the median value, keeping the frame of reference
      // tight (an outlier there would widen or shift it).
      int64_t fill = 0;
      if (!present.empty()) {
        std::nth_element(present.begin(), present.begin() + static_cast<long>(present.size() / 2), present.end());
        fill = present[present.size() / 2];
      }
      for (size_t i = 0; i < n; i++) {
        if (nulls[i]) v[i] = fill;
      }
      encodeInts(v, col);
    } else if (type == Type::Float) {
      // The smallest power of ten that makes every value an exact integer.
      int exp = -1;
      std::vector<int64_t> scaled(n, 0);
      for (int e = 0; e <= 6 && exp < 0; e++) {
        bool ok = true;
        for (size_t i = 0; i < n && ok; i++) {
          if (nulls[i]) continue;
          double x = rows[i][c].asFloat();
          double y = x * kPow10[e];
          if (!(std::fabs(y) < 9.0e15) || (x == 0 && std::signbit(x))) {
            ok = false;
            break;
          }
          int64_t r = static_cast<int64_t>(std::llround(y));
          if (static_cast<double>(r) / kPow10[e] != x) ok = false;  // the decoder's exact operation
          scaled[i] = r;
        }
        if (ok) exp = e;
      }
      if (exp >= 0) {
        col += static_cast<char>(kFloatScaled);
        col += static_cast<char>(exp);
        col += static_cast<char>(kIntFor);
        putIntFor(scaled, col);
      } else {
        col += static_cast<char>(kFloatRaw);
        for (size_t i = 0; i < n; i++) {
          double x = nulls[i] ? 0.0 : rows[i][c].asFloat();
          col.append(reinterpret_cast<const char*>(&x), 8);
        }
      }
    } else {  // Text
      const TextDict* dict = t.dict(static_cast<int>(c));
      std::vector<int64_t> codes(n, 0);
      bool coded = dict != nullptr;
      for (size_t i = 0; i < n && coded; i++) {
        if (nulls[i]) continue;
        int32_t code = dict->find(rows[i][c].asText());
        if (code < 0) coded = false;
        codes[i] = code;
      }
      if (coded) {
        col += static_cast<char>(kTextDict);
        putIntFor(codes, col);
      } else {
        col += static_cast<char>(kTextPlain);
        for (size_t i = 0; i < n; i++) {
          const std::string& s = nulls[i] ? std::string() : rows[i][c].asText();
          putVarint(s.size(), col);
          col += s;
        }
      }
    }
    putVarint(col.size(), out);
    out += col;
  }
  return out;
}

uint32_t decodeBlock(const TableSchema& t, std::string_view in, const std::vector<bool>* needed,
                     std::vector<ColumnVector>& cols) {
  size_t n = static_cast<size_t>(getVarint(in));
  size_t ncols = static_cast<size_t>(getVarint(in));
  if (ncols != t.cols.size()) throw Error("corrupt column block: column count");
  cols.resize(ncols);
  for (size_t c = 0; c < ncols; c++) {
    size_t len = static_cast<size_t>(getVarint(in));
    if (in.size() < len) throw Error("corrupt column block");
    std::string_view col = in.substr(0, len);
    in.remove_prefix(len);
    if (needed && !(*needed)[c]) continue;
    ColumnVector& cv = cols[c];
    cv.reserve(t.cols[c].type, n);
    cv.dict = t.dict(static_cast<int>(c));
    if (col.empty()) throw Error("corrupt column block");
    bool has_nulls = col[0] != 0;
    col.remove_prefix(1);
    std::fill(cv.nulls.begin(), cv.nulls.end(), 0);
    if (has_nulls) {
      size_t bytes = (n + 7) / 8;
      if (col.size() < bytes) throw Error("corrupt column block");
      for (size_t i = 0; i < n; i++) cv.nulls[i] = (static_cast<uint8_t>(col[i >> 3]) >> (i & 7)) & 1;
      col.remove_prefix(bytes);
    }
    if (col.empty()) throw Error("corrupt column block");
    uint8_t tag = static_cast<uint8_t>(col[0]);
    col.remove_prefix(1);
    Type type = t.cols[c].type;
    if (type == Type::Int || type == Type::Bool || type == Type::Null) {
      cv.ints.resize(n);
      decodeInts(col, tag, n, cv.ints.data());
    } else if (type == Type::Float) {
      cv.floats.resize(n);
      if (tag == kFloatScaled) {
        if (col.size() < 2) throw Error("corrupt column block");
        int e = static_cast<uint8_t>(col[0]);
        uint8_t itag = static_cast<uint8_t>(col[1]);
        col.remove_prefix(2);
        if (e > 6) throw Error("corrupt column block");
        std::vector<int64_t> v(n);
        decodeInts(col, itag, n, v.data());
        for (size_t i = 0; i < n; i++) cv.floats[i] = static_cast<double>(v[i]) / kPow10[e];
      } else if (tag == kFloatRaw) {
        if (col.size() < 8 * n) throw Error("corrupt column block");
        std::memcpy(cv.floats.data(), col.data(), 8 * n);
      } else {
        throw Error("corrupt column block: bad float encoding");
      }
    } else {
      if (tag == kTextDict) {
        if (!cv.dict) throw Error("corrupt column block: coded text without a dictionary");
        size_t limit = cv.dict->values.size();
        getIntFor(col, n, [&](size_t i, int64_t code) {
          if (!cv.nulls[i] && (code < 0 || static_cast<size_t>(code) >= limit)) throw Error("corrupt column block: code");
          cv.codes[i] = cv.nulls[i] ? -1 : static_cast<int32_t>(code);
        });
      } else if (tag == kTextPlain) {
        for (size_t i = 0; i < n; i++) {
          size_t l = static_cast<size_t>(getVarint(col));
          if (col.size() < l) throw Error("corrupt column block");
          cv.texts[i].assign(col.data(), l);
          col.remove_prefix(l);
          cv.codes[i] = -1;
        }
      } else {
        throw Error("corrupt column block: bad text encoding");
      }
    }
  }
  return static_cast<uint32_t>(n);
}

}  // namespace pkql
