#include "quark/utils/JsonParser.h"

#include <gtest/gtest.h>

#include <charconv>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using quark::JsonParser;

// Overloads that would leave the document dangling or are ambiguous must not compile
template <typename T>
constexpr bool kCanParseInPlace = requires(JsonParser& p, T&& json) { p.parse_in_place(std::forward<T>(json)); };
template <typename T>
constexpr bool kCanParseCopy = requires(JsonParser& p, T&& json) { p.parse_copy(std::forward<T>(json)); };

static_assert(std::is_default_constructible_v<JsonParser>);
static_assert(std::is_nothrow_move_constructible_v<JsonParser>);
static_assert(kCanParseInPlace<std::string&>);
static_assert(kCanParseInPlace<simdjson::padded_string&>);
static_assert(!kCanParseInPlace<std::string>);
static_assert(!kCanParseInPlace<simdjson::padded_string>);
static_assert(!kCanParseInPlace<const char (&)[3]>);
static_assert(kCanParseCopy<const char (&)[3]>);
static_assert(kCanParseCopy<std::string>);

// Line 0 is the REST depth snapshot, the following lines are the depthUpdate stream messages
constexpr const char* kDepthFile = QUARK_TEST_DATA_DIR "/utils/Binance_LinearSwap_BTCUSDT_depth.json";
constexpr size_t kDepthLines = 622;
constexpr size_t kSnapshotLevels = 1000;

std::vector<std::string> read_lines(const char* path) {
    std::vector<std::string> lines;
    std::ifstream in(path);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty()) {
            // padded once at load like a receive buffer, so the lines can be parsed in place
            line.reserve(line.size() + JsonParser::kPadding);
            lines.push_back(std::move(line));
        }
    }
    return lines;
}

double to_double(std::string_view str) {
    double value = 0;
    auto [ptr, ec] = std::from_chars(str.data(), str.data() + str.size(), value);
    EXPECT_EQ(ec, std::errc{}) << str;
    EXPECT_EQ(ptr, str.data() + str.size()) << str;
    return value;
}

struct Level {
    double price;
    double qty;
};

// Price levels are ["price","qty"] string pairs
simdjson::error_code get_levels(simdjson::ondemand::array levels, std::vector<Level>& out) {
    out.clear();
    for (auto level : levels) {
        simdjson::ondemand::array pair;
        if (auto err = level.get_array().get(pair)) {
            return err;
        }
        std::string_view price, qty;
        auto it = pair.begin();
        if (auto err = (*it).get_string().get(price)) {
            return err;
        }
        ++it;
        if (auto err = (*it).get_string().get(qty)) {
            return err;
        }
        out.push_back({to_double(price), to_double(qty)});
    }
    return simdjson::SUCCESS;
}

class JsonParserTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        lines_ = read_lines(kDepthFile);
    }

    static void TearDownTestSuite() {
        lines_.clear();
    }

    void SetUp() override {
        ASSERT_EQ(lines_.size(), kDepthLines) << "failed to read " << kDepthFile;
    }

    static std::vector<std::string> lines_;
};

std::vector<std::string> JsonParserTest::lines_;

TEST_F(JsonParserTest, ParsesDepthSnapshot) {
    JsonParser parser;
    simdjson::ondemand::document doc;
    ASSERT_EQ(parser.parse_copy(lines_[0]).get(doc), simdjson::SUCCESS);

    uint64_t last_update_id = 0, event_time = 0, transaction_time = 0;
    ASSERT_EQ(doc["lastUpdateId"].get_uint64().get(last_update_id), simdjson::SUCCESS);
    ASSERT_EQ(doc["E"].get_uint64().get(event_time), simdjson::SUCCESS);
    ASSERT_EQ(doc["T"].get_uint64().get(transaction_time), simdjson::SUCCESS);
    EXPECT_EQ(last_update_id, 1256817213812ULL);
    EXPECT_EQ(event_time, 1646302647681ULL);
    EXPECT_EQ(transaction_time, 1646302647671ULL);

    std::vector<Level> bids, asks;
    simdjson::ondemand::array array;
    ASSERT_EQ(doc["bids"].get_array().get(array), simdjson::SUCCESS);
    ASSERT_EQ(get_levels(array, bids), simdjson::SUCCESS);
    ASSERT_EQ(doc["asks"].get_array().get(array), simdjson::SUCCESS);
    ASSERT_EQ(get_levels(array, asks), simdjson::SUCCESS);

    ASSERT_EQ(bids.size(), kSnapshotLevels);
    ASSERT_EQ(asks.size(), kSnapshotLevels);
    EXPECT_DOUBLE_EQ(bids.front().price, 43250.90);
    EXPECT_DOUBLE_EQ(bids.front().qty, 2.852);
    EXPECT_DOUBLE_EQ(bids.back().price, 43060.50);
    EXPECT_DOUBLE_EQ(bids.back().qty, 0.001);
    EXPECT_DOUBLE_EQ(asks.front().price, 43251.00);
    EXPECT_DOUBLE_EQ(asks.front().qty, 0.170);
    EXPECT_DOUBLE_EQ(asks.back().price, 43429.60);
    EXPECT_DOUBLE_EQ(asks.back().qty, 0.001);

    // the book is sorted from the top and never crossed
    EXPECT_LT(bids.front().price, asks.front().price);
    for (size_t i = 1; i < kSnapshotLevels; ++i) {
        EXPECT_GT(bids[i - 1].price, bids[i].price) << "bid level " << i;
        EXPECT_LT(asks[i - 1].price, asks[i].price) << "ask level " << i;
    }
}

TEST_F(JsonParserTest, ParsesDepthUpdateStream) {
    // a single parser reused for the whole stream, as on the hot path
    JsonParser parser;

    uint64_t snapshot_id = 0;
    {
        simdjson::ondemand::document doc;
        ASSERT_EQ(parser.parse_in_place(lines_[0]).get(doc), simdjson::SUCCESS);
        ASSERT_EQ(doc["lastUpdateId"].get_uint64().get(snapshot_id), simdjson::SUCCESS);
    }

    size_t total_bids = 0, total_asks = 0;
    uint64_t prev_final_id = 0, prev_event_time = 0;
    std::vector<Level> levels;
    for (size_t i = 1; i < lines_.size(); ++i) {
        SCOPED_TRACE("line " + std::to_string(i));
        simdjson::ondemand::document doc;
        ASSERT_EQ(parser.parse_in_place(lines_[i]).get(doc), simdjson::SUCCESS);

        std::string_view stream;
        ASSERT_EQ(doc["stream"].get_string().get(stream), simdjson::SUCCESS);
        EXPECT_EQ(stream, "btcusdt@depth@0ms");

        simdjson::ondemand::object data;
        ASSERT_EQ(doc["data"].get_object().get(data), simdjson::SUCCESS);

        // fields are read in document order, which is the fast path of on-demand
        std::string_view event, symbol;
        uint64_t event_time = 0, transaction_time = 0, first_id = 0, final_id = 0, prev_id = 0;
        ASSERT_EQ(data["e"].get_string().get(event), simdjson::SUCCESS);
        ASSERT_EQ(data["E"].get_uint64().get(event_time), simdjson::SUCCESS);
        ASSERT_EQ(data["T"].get_uint64().get(transaction_time), simdjson::SUCCESS);
        ASSERT_EQ(data["s"].get_string().get(symbol), simdjson::SUCCESS);
        ASSERT_EQ(data["U"].get_uint64().get(first_id), simdjson::SUCCESS);
        ASSERT_EQ(data["u"].get_uint64().get(final_id), simdjson::SUCCESS);
        ASSERT_EQ(data["pu"].get_uint64().get(prev_id), simdjson::SUCCESS);
        EXPECT_EQ(event, "depthUpdate");
        EXPECT_EQ(symbol, "BTCUSDT");
        EXPECT_LE(first_id, final_id);
        EXPECT_LE(transaction_time, event_time);
        EXPECT_LE(prev_event_time, event_time);

        if (i == 1) {
            // the first update bridges the snapshot: U <= lastUpdateId + 1 <= u
            EXPECT_LE(first_id, snapshot_id + 1);
            EXPECT_GE(final_id, snapshot_id + 1);
        } else {
            // no message lost: each pu is the u of the previous message
            EXPECT_EQ(prev_id, prev_final_id);
        }
        prev_final_id = final_id;
        prev_event_time = event_time;

        simdjson::ondemand::array array;
        ASSERT_EQ(data["b"].get_array().get(array), simdjson::SUCCESS);
        ASSERT_EQ(get_levels(array, levels), simdjson::SUCCESS);
        total_bids += levels.size();
        ASSERT_EQ(data["a"].get_array().get(array), simdjson::SUCCESS);
        ASSERT_EQ(get_levels(array, levels), simdjson::SUCCESS);
        total_asks += levels.size();
    }

    EXPECT_EQ(prev_final_id, 1256817844052ULL);
    EXPECT_EQ(total_bids, 13551U);
    EXPECT_EQ(total_asks, 11055U);
}

// raw_json() points into the buffer that was actually parsed
std::string_view raw_json(simdjson::ondemand::document& doc) {
    std::string_view raw;
    EXPECT_EQ(doc.raw_json().get(raw), simdjson::SUCCESS);
    return raw;
}

TEST_F(JsonParserTest, ParseInPlaceDoesNotCopy) {
    JsonParser parser;
    const std::string& line = lines_[1];
    ASSERT_GE(line.capacity(), line.size() + JsonParser::kPadding);

    simdjson::ondemand::document doc;
    ASSERT_EQ(parser.parse_in_place(line).get(doc), simdjson::SUCCESS);
    EXPECT_EQ(raw_json(doc).data(), line.data());
    EXPECT_EQ(raw_json(doc), line);

    // a sub-range of a larger buffer, the bytes after it serve as padding
    std::string buffer = line + line;
    ASSERT_EQ(parser.parse_in_place(buffer.data(), line.size(), buffer.size()).get(doc), simdjson::SUCCESS);
    EXPECT_EQ(raw_json(doc).data(), buffer.data());
    EXPECT_EQ(raw_json(doc), line);

    simdjson::padded_string padded{line};
    ASSERT_EQ(parser.parse_in_place(padded).get(doc), simdjson::SUCCESS);
    EXPECT_EQ(raw_json(doc).data(), padded.data());
    EXPECT_EQ(raw_json(doc), line);
}

TEST_F(JsonParserTest, ParseInPlaceRejectsUnpaddedBuffer) {
    JsonParser parser;
    const std::string& line = lines_[1];

    // never falls back to a copy silently
    EXPECT_EQ(parser.parse_in_place(line.data(), line.size(), line.size()).error(), simdjson::INSUFFICIENT_PADDING);
    EXPECT_EQ(parser.parse_in_place(line.data(), line.size(), line.size() + JsonParser::kPadding - 1).error(),
              simdjson::INSUFFICIENT_PADDING);

    std::string unpadded = line;
    unpadded.shrink_to_fit();
    if (unpadded.capacity() < unpadded.size() + JsonParser::kPadding) {
        EXPECT_EQ(parser.parse_in_place(unpadded).error(), simdjson::INSUFFICIENT_PADDING);
    }
}

TEST_F(JsonParserTest, ParseCopyOutlivesTheSource) {
    JsonParser parser;
    std::string source = lines_[1];
    source.shrink_to_fit(); // no padding needed

    simdjson::ondemand::document doc;
    ASSERT_EQ(parser.parse_copy(source).get(doc), simdjson::SUCCESS);
    EXPECT_NE(raw_json(doc).data(), source.data());
    EXPECT_EQ(raw_json(doc), lines_[1]);

    // the document only depends on the parser's own copy
    std::fill(source.begin(), source.end(), ' ');
    source.clear();
    source.shrink_to_fit();
    uint64_t final_id = 0;
    ASSERT_EQ(doc["data"]["u"].get_uint64().get(final_id), simdjson::SUCCESS);
    EXPECT_EQ(final_id, 1256817214169ULL);
}

TEST_F(JsonParserTest, GrowsBeyondInitialCapacity) {
    // the 42KB snapshot is much larger than what was preallocated
    JsonParser parser{64};
    ASSERT_GT(lines_[0].size(), 64U);

    for (bool in_place : {false, true}) {
        SCOPED_TRACE(in_place ? "parse_in_place" : "parse_copy");
        simdjson::ondemand::document doc;
        auto result = in_place ? parser.parse_in_place(lines_[0]) : parser.parse_copy(lines_[0]);
        ASSERT_EQ(std::move(result).get(doc), simdjson::SUCCESS);
        simdjson::ondemand::array asks;
        ASSERT_EQ(doc["asks"].get_array().get(asks), simdjson::SUCCESS);
        size_t count = 0;
        ASSERT_EQ(asks.count_elements().get(count), simdjson::SUCCESS);
        EXPECT_EQ(count, kSnapshotLevels);
    }
}

TEST_F(JsonParserTest, TinyInitialCapacitySkipsWarmUp) {
    for (size_t capacity : {0U, 1U, 2U, 3U, 4U}) {
        SCOPED_TRACE("capacity " + std::to_string(capacity));
        JsonParser parser{capacity};
        simdjson::ondemand::document doc;
        ASSERT_EQ(parser.parse_copy(lines_[1]).get(doc), simdjson::SUCCESS);
        std::string_view stream;
        ASSERT_EQ(doc["stream"].get_string().get(stream), simdjson::SUCCESS);
        EXPECT_EQ(stream, "btcusdt@depth@0ms");
    }
}

TEST_F(JsonParserTest, IsUsableAfterMove) {
    JsonParser source;
    JsonParser parser{std::move(source)};
    JsonParser assigned{16};
    assigned = std::move(parser);

    for (JsonParser* p : {&parser, &assigned}) {
        // the moved-from parser must be reusable too
        simdjson::ondemand::document doc;
        ASSERT_EQ(p->parse_copy(lines_[1]).get(doc), simdjson::SUCCESS);
        uint64_t final_id = 0;
        ASSERT_EQ(doc["data"]["u"].get_uint64().get(final_id), simdjson::SUCCESS);
        EXPECT_EQ(final_id, 1256817214169ULL);
    }
}

TEST(JsonParser, ReportsMalformedJson) {
    JsonParser parser;

    // detected by the structural stage, iterate() already fails
    EXPECT_NE(parser.parse_copy(R"({"stream":"btcusdt@depth)").error(), simdjson::SUCCESS);
    EXPECT_EQ(parser.parse_copy("").error(), simdjson::EMPTY);

    // on-demand only detects a bad value when it is accessed
    simdjson::ondemand::document doc;
    ASSERT_EQ(parser.parse_copy(R"({"u":12x})").get(doc), simdjson::SUCCESS);
    uint64_t value = 0;
    EXPECT_NE(doc["u"].get_uint64().get(value), simdjson::SUCCESS);

    // the parser is still usable after an error
    ASSERT_EQ(parser.parse_copy(R"({"u":12})").get(doc), simdjson::SUCCESS);
    ASSERT_EQ(doc["u"].get_uint64().get(value), simdjson::SUCCESS);
    EXPECT_EQ(value, 12U);
}

TEST(JsonParser, ReportsTypeMismatchAndMissingField) {
    JsonParser parser;
    simdjson::ondemand::document doc;
    ASSERT_EQ(parser.parse_copy(R"({"s":"BTCUSDT","u":1})").get(doc), simdjson::SUCCESS);

    uint64_t value = 0;
    EXPECT_EQ(doc["s"].get_uint64().get(value), simdjson::INCORRECT_TYPE);
    EXPECT_EQ(doc["missing"].get_uint64().get(value), simdjson::NO_SUCH_FIELD);
}

TEST(JsonParser, RejectsCapacityAboveSimdjsonMax) {
    constexpr size_t too_large = simdjson::SIMDJSON_MAXSIZE_BYTES + 1;
#if defined(__cpp_exceptions)
    EXPECT_THROW(JsonParser{too_large}, quark::QuarkError);
#else
    EXPECT_DEATH(JsonParser{too_large}, "JsonParser failed to preallocate");
#endif
}

TEST(JsonParser, TestParseGeneral) {
    // clang-format off
    std::string input = R"(
        {
        "status": "ok",
        "nodes": [ 
            {
                "source": "spot-web",
                "price": "0.338",
                "fee": 0.06
            }
        ],
        "data": 
            {
                "symbol": "polyusdt",
                "fee-currency": "poly",
                "source": "spot-web",
                "order-id": 345487249132375,
                "price": "0.338",
                "created-at": 1629443051839,
                "role": "taker",
                "match-id": "5014",
                "filled-amount": "147.928994082840236",
                "filled-fees": "0",
                "filled-points": "0.1",
                "fee-deduct-currency": "hbpoint",
                "fee-deduct-state": "done",
                "trade-id": 1085,
                "id": 313288753120940,
                "type": "buy-market",
                "fee": 0.06
            }
        
        }
    )";

    // clang-format on
    JsonParser parser;
    simdjson::ondemand::document doc;
    EXPECT_EQ(parser.parse_in_place(input).get(doc), simdjson::INSUFFICIENT_PADDING);

    input.reserve(input.size() + JsonParser::kPadding);
    EXPECT_EQ(parser.parse_in_place(input).get(doc), simdjson::SUCCESS);

    // 1. check parse string_view
    std::string_view data_sv;
    EXPECT_EQ(doc["error_code"].get(data_sv), simdjson::NO_SUCH_FIELD);
    EXPECT_TRUE(data_sv.empty());

    EXPECT_EQ(doc["status"].get(data_sv), simdjson::SUCCESS);
    EXPECT_EQ(data_sv, "ok");

    // 2. test integer
    simdjson::ondemand::object data;
    EXPECT_EQ(doc["data"].get(data), simdjson::SUCCESS);
    int64_t num_i{0};
    EXPECT_EQ(data["anystr"].get(num_i), simdjson::NO_SUCH_FIELD);
    EXPECT_EQ(num_i, 0);
    EXPECT_EQ(data["match-id"].get(num_i), simdjson::INCORRECT_TYPE);
    EXPECT_EQ(data["match-id"]->get_int64_in_string().get(num_i), simdjson::SUCCESS);
    EXPECT_EQ(num_i, 5014);

    EXPECT_EQ(data["id"].get(num_i), simdjson::SUCCESS);
    EXPECT_EQ(num_i, 313288753120940);

    // 3. test double
    double num_d{0.0};
    EXPECT_EQ(data["anystr"].get(num_d), simdjson::NO_SUCH_FIELD);
    EXPECT_EQ(num_d, 0);
    EXPECT_EQ(data["filled-amount"].get(num_d), simdjson::INCORRECT_TYPE);
    EXPECT_EQ(data["filled-amount"]->get_double_in_string().get(num_d), simdjson::SUCCESS);
    EXPECT_NEAR(num_d, 147.928994082840236, 1e-10);

    EXPECT_EQ(data["fee"].get(num_d), simdjson::SUCCESS);
    EXPECT_NEAR(num_d, 0.06, 1e-10);

    // 4. test array
    auto nodes = doc["nodes"];
    EXPECT_TRUE(!nodes->is_scalar());
    for (auto node : nodes->get_array()) {
        std::string_view source;
        EXPECT_EQ(node["source"].get(source), simdjson::SUCCESS);
        EXPECT_EQ(source, "spot-web");

        std::string_view price_str;
        EXPECT_EQ(node["price"]->get(price_str), simdjson::SUCCESS);
        EXPECT_EQ(price_str, "0.338");
        double d;
        EXPECT_EQ(node["price"].get(d), simdjson::INCORRECT_TYPE);
        EXPECT_EQ(node["price"]->get_double_in_string().get(d), simdjson::SUCCESS);
        // EXPECT_NEAR(d, 0.338, 1e-10);
        EXPECT_EQ(node["fee"]->get(d), simdjson::SUCCESS);
        EXPECT_NEAR(d, 0.06, 1e-10);
    }
}

} // namespace
