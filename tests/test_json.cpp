// Host-side unit tests for json.cpp.
//
// The fixtures are trimmed but structurally real: the payloads come from live
// responses of GET https://nextendo.network/api/online-counts. The most
// important assertion in this file is test_total_uses_jeux_not_counts(): the
// website's number is the sum of jeux[].joueurs, and the "counts" object
// double-counts games that have several regional title ids.

#include "../source/json.hpp"

#include <cstdio>
#include <cstring>
#include <string>

using namespace nextendo;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                      \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                     \
    do {                                                                       \
        ++g_checks;                                                            \
        long long va = static_cast<long long>(a);                              \
        long long vb = static_cast<long long>(b);                              \
        if (va != vb) {                                                        \
            ++g_failures;                                                      \
            std::printf("FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__,    \
                        __LINE__, #a, #b, va, vb);                             \
        }                                                                      \
    } while (0)

// A trimmed copy of a real live response.
static const char* kRealPayload = R"JSON({
  "counts": {
    "0100152000022000": 11,
    "01006a800016e000": 6,
    "01008f6008c5e000": 2,
    "0100a3d008c5c000": 2,
    "0100c2500fc20000": 13,
    "0100f9f00c696000": 3,
    "01009b90006dc000": 6,
    "0100a7c01b792000": 3,
    "0100f43008c44000": 2,
    "0100770008dd8000": 1,
    "01009b500007c000": 0
  },
  "jeux": [
    { "nom": "Splatoon 3", "joueurs": 13, "titres": ["0100c2500fc20000"] },
    { "nom": "Mario Kart 8 Deluxe", "joueurs": 11, "titres": ["0100152000022000"] },
    { "nom": "Super Mario Maker 2", "joueurs": 6, "titres": ["01009b90006dc000"] },
    { "nom": "Super Smash Bros. Ultimate", "joueurs": 6, "titres": ["01006a800016e000"] },
    { "nom": "Crash Team Racing Nitro-Fueled", "joueurs": 3, "titres": ["0100f9f00c696000"] },
    { "nom": "Minecraft Dungeons II", "joueurs": 3, "titres": ["0100a7c01b792000"] },
    { "nom": "Legendes Pokemon : Z-A", "joueurs": 2, "titres": ["0100f43008c44000"] },
    { "nom": "Pokemon Scarlet", "joueurs": 2, "titres": ["0100a3d008c5c000", "01008f6008c5e000"] },
    { "nom": "MONSTER HUNTER GENERATIONS ULTIMATE", "joueurs": 1, "titres": ["0100770008dd8000"] },
    { "nom": "ARMS", "joueurs": 0, "titres": ["01009b500007c000"] }
  ],
  "noms": {
    "0100152000022000": "Mario Kart 8 Deluxe",
    "0100c2500fc20000": "Splatoon 3"
  }
})JSON";

// The single most important behaviour: the displayed total must match the
// website, which sums jeux[].joueurs. Summing "counts" would report 49 here
// because Pokemon Scarlet/Violet share one number across two regional ids.
static void test_total_uses_jeux_not_counts() {
    OnlineCounts c;
    CHECK(parse_online_counts(kRealPayload, std::strlen(kRealPayload), c) == ParseStatus::Ok);

    CHECK_EQ_INT(c.total, 47);

    long long counts_sum = 0;
    // Deliberately recomputed from the fixture to prove the two differ, so this
    // test fails loudly if someone "simplifies" the parser to use counts.
    counts_sum = 11 + 6 + 2 + 2 + 13 + 3 + 6 + 3 + 2 + 1 + 0;
    CHECK_EQ_INT(counts_sum, 49);
    CHECK(c.total != counts_sum);
}

static void test_games_parsed_in_order() {
    OnlineCounts c;
    parse_online_counts(kRealPayload, std::strlen(kRealPayload), c);

    CHECK_EQ_INT(c.games.size(), 10);
    if (c.games.size() == 10) {
        CHECK(c.games[0].name == "Splatoon 3");
        CHECK_EQ_INT(c.games[0].players, 13);
        CHECK(c.games[1].name == "Mario Kart 8 Deluxe");
        CHECK_EQ_INT(c.games[1].players, 11);
        CHECK(c.games[7].name == "Pokemon Scarlet");
        CHECK(c.games[9].name == "ARMS");
        CHECK_EQ_INT(c.games[9].players, 0);
    }
    CHECK_EQ_INT(c.title_count, 11);
    CHECK_EQ_INT(c.name_count, 2);
    CHECK(c.plausible);
}

// The API returns UTF-8 with accents; the parser must pass those bytes through
// untouched and must decode \uXXXX escapes (including surrogate pairs).
static void test_utf8_and_escapes() {
    const char* payload =
        "{\"jeux\":[{\"nom\":\"L\\u00e9gendes Pok\\u00e9mon : Z-A\",\"joueurs\":2},"
        "{\"nom\":\"Pok\\u00e9mon \\u00c9carlate\",\"joueurs\":1},"
        "{\"nom\":\"Emoji \\ud83c\\udfae test\",\"joueurs\":3},"
        "{\"nom\":\"Tab\\there\\\"quoted\\\\slash\",\"joueurs\":4}]}";

    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.total, 10);
    CHECK_EQ_INT(c.games.size(), 4);
    if (c.games.size() == 4) {
        // Note the split string literals: "\xA9mon" would be read as a single
        // hex escape (m is a hex digit) and overflow a char. Keeping the
        // escape alone in its own literal avoids that trap.
        CHECK(c.games[0].name == "L\xC3\xA9" "gendes Pok\xC3\xA9" "mon : Z-A");
        CHECK(c.games[1].name == "Pok\xC3\xA9" "mon \xC3\x89" "carlate");
        // U+1F3AE decodes to the 4-byte UTF-8 sequence F0 9F 8E AE.
        CHECK(c.games[2].name == "Emoji \xF0\x9F\x8E\xAE" " test");
        CHECK(c.games[3].name == "Tab\there\"quoted\\slash");
    }

    // Truncation must not split a multi-byte sequence.
    const std::string accented = "L\xC3\xA9" "gendes";
    CHECK(utf8_truncate(accented, 100) == accented);
    CHECK(utf8_truncate(accented, 2) == "L");  // cutting before the 2-byte 'e'
    CHECK(utf8_truncate(accented, 3) == "L\xC3\xA9");
    CHECK(utf8_truncate("plain", 3) == "pla");
}

// A server-side addition must never break the overlay: unknown keys at any
// level are skipped, and a missing "jeux" still yields a usable document.
static void test_forward_compatible() {
    const char* payload =
        "{\"nouveau\":{\"a\":[1,2,{\"b\":null}]},\"jeux\":[{\"joueurs\":5,"
        "\"nom\":\"Game\",\"extra\":[true,false]},{\"joueurs\":7,\"nom\":\"G2\"}],"
        "\"counts\":{\"x\":5},\"autre\":\"ignored\",\"flag\":true}";

    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.total, 12);
    CHECK_EQ_INT(c.games.size(), 2);
}

static void test_missing_jeux_is_not_fatal() {
    const char* payload = "{\"counts\":{\"0100c2500fc20000\":4}}";
    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.total, 0);
    CHECK_EQ_INT(c.title_count, 1);
    CHECK(c.plausible);
}

static void test_nobody_online_is_ok() {
    const char* payload = "{\"jeux\":[],\"counts\":{},\"noms\":{}}";
    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.total, 0);
    CHECK_EQ_INT(c.games.size(), 0);
    CHECK(c.plausible);
}

// Whitespace, floats and integers that arrive as JSON numbers must all survive.
static void test_number_shapes() {
    const char* payload =
        "{\"jeux\":[{\"nom\":\"A\",\"joueurs\":12.0},{\"nom\":\"B\",\"joueurs\":3.9}]}";
    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.total, 15);

    const char* whitespace = "  \n\t {\n  \"jeux\" : [ { \"nom\" : \"X\" , \"joueurs\" : 8 } ] \n } \n ";
    OnlineCounts c2;
    CHECK(parse_online_counts(whitespace, std::strlen(whitespace), c2) == ParseStatus::Ok);
    CHECK_EQ_INT(c2.total, 8);
}

// A cut-off body is a network problem, not a syntax problem, and the overlay
// needs to tell those apart to show the right message.
static void test_truncated_vs_syntax() {
    {
        OnlineCounts c;
        CHECK(parse_online_counts("", 0, c) == ParseStatus::Truncated);
    }
    {
        OnlineCounts c;
        const char* ws = "   \n  ";
        CHECK(parse_online_counts(ws, std::strlen(ws), c) == ParseStatus::Truncated);
    }
    {
        OnlineCounts c;
        const char* cut = "{\"jeux\":[{\"nom\":\"Splatoon 3\",\"joueurs\":13},{\"nom\":\"Mar";
        CHECK(parse_online_counts(cut, std::strlen(cut), c) == ParseStatus::Truncated);
    }
    {
        OnlineCounts c;
        const char* cut = "{\"jeux\":[{\"nom\":\"Splatoon 3\",\"joueurs\":13}";
        CHECK(parse_online_counts(cut, std::strlen(cut), c) == ParseStatus::Truncated);
    }
    {
        OnlineCounts c;
        const char* bad = "not json at all";
        CHECK(parse_online_counts(bad, std::strlen(bad), c) == ParseStatus::Syntax);
    }
    {
        OnlineCounts c;
        const char* bad = "{\"jeux\":[{\"nom\":}]}";
        CHECK(parse_online_counts(bad, std::strlen(bad), c) == ParseStatus::Syntax);
    }
}

// The overlay reads a length-delimited buffer, so the parser must never look
// past `len` even when the bytes after it look like JSON.
static void test_no_read_past_length() {
    const char* payload = "{\"jeux\":[{\"nom\":\"Splatoon\",\"joueurs\":9}]}";
    const std::size_t full = std::strlen(payload);
    const std::size_t cut = full - 2;  // drop the closing "]}" only

    OnlineCounts c;
    CHECK(parse_online_counts(payload, cut, c) == ParseStatus::Truncated);
    CHECK_EQ_INT(c.total, 0);  // partial array is discarded, not half-counted
}

static void test_negative_and_absurd_values_are_clamped() {
    const char* payload =
        "{\"jeux\":[{\"nom\":\"Neg\",\"joueurs\":-5},{\"nom\":\"Huge\",\"joueurs\":99999999}]}";
    OnlineCounts c;
    CHECK(parse_online_counts(payload, std::strlen(payload), c) == ParseStatus::Ok);
    CHECK_EQ_INT(c.games.size(), 2);
    if (c.games.size() == 2) {
        CHECK_EQ_INT(c.games[0].players, 0);
        CHECK(c.games[1].players <= 1000000);
    }
}

int main() {
    test_total_uses_jeux_not_counts();
    test_games_parsed_in_order();
    test_utf8_and_escapes();
    test_forward_compatible();
    test_missing_jeux_is_not_fatal();
    test_nobody_online_is_ok();
    test_number_shapes();
    test_truncated_vs_syntax();
    test_no_read_past_length();
    test_negative_and_absurd_values_are_clamped();

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
