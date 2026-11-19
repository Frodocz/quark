// A second translation unit including JsonParser.h: the test binary only links if simdjson's
// header-defined functions are inline.
#include "quark/utils/JsonParser.h"

int quark_json_parser_second_tu() {
    quark::JsonParser parser(64);
    auto doc = parser.parse_copy(R"({"a":1})");
    int64_t a = 0;
    return doc["a"].get_int64().get(a) == simdjson::SUCCESS ? static_cast<int>(a) : -1;
}
