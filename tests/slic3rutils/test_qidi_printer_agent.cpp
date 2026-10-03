#include "catch2/catch_test_macros.hpp"
#include "catch2/matchers/catch_matchers.hpp"
#include "catch2/matchers/catch_matchers_string.hpp"
#include <catch2/catch_message.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_all.hpp>

#include <nlohmann/json.hpp>

#include <slic3r/Utils/QidiPrinterAgent.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "slic3r/Utils/bambu_networking.hpp"

using namespace Slic3r;

namespace {

std::vector<std::string> print_commands(const PrintParams& params, bool has_leveling_macros = true)
{
    std::vector<std::string> gcode;
    std::string              error;
    REQUIRE(QidiPrinterAgent::build_print_commands(params, has_leveling_macros, gcode, error));
    return gcode;
}

bool contains(const std::vector<std::string>& gcode, const std::string& command)
{
    return std::find(gcode.begin(), gcode.end(), command) != gcode.end();
}

bool sets_variable(const std::vector<std::string>& gcode, const std::string& variable)
{
    const std::string prefix = "SAVE_VARIABLE VARIABLE=" + variable + " ";
    return std::any_of(gcode.begin(), gcode.end(), [&](const std::string& command) { return command.rfind(prefix, 0) == 0; });
}

} // namespace

TEST_CASE("Qidi slot response rejects null variables without throwing", "[QidiPrinterAgent]")
{
    const std::string response = R"({
        "result": {
            "status": {
                "save_variables": {
                    "variables": null
                }
            }
        }
    })";
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    CHECK_FALSE(parsed);
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("variables"));
    CHECK_THAT(error, Catch::Matchers::ContainsSubstring("object"));
}

TEST_CASE("Qidi slot response rejects missing and non-object fields without throwing", "[QidiPrinterAgent]")
{
    const std::string response = GENERATE(as<std::string>{},
                                          R"({})",                                                         // missing result
                                          R"({"result":null})",                                            // non-object result
                                          R"({"result":{}})",                                              // missing status
                                          R"({"result":{"status":null}})",                                 // non-object status
                                          R"({"result":{"status":{}}})",                                   // missing save_variables
                                          R"({"result":{"status":{"save_variables":null}}})",              // non-object save_variables
                                          R"({"result":{"status":{"save_variables":{}}}})",                // missing variables
                                          R"({"result":{"status":{"save_variables":{"variables":42}}}})",  // scalar
                                          R"({"result":{"status":{"save_variables":{"variables":[]}}}})"); // array
    CAPTURE(response);

    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    CHECK_FALSE(parsed);
}

TEST_CASE("Qidi slot response exposes valid status and variables", "[QidiPrinterAgent]")
{
    const std::string response = R"({
        "result": {
            "status": {
                "save_variables": {
                    "variables": {
                        "box_count": 2,
                        "color_slot0": 3
                    }
                },
                "box_stepper slot0": {
                    "runout_button": 0
                }
            }
        }
    })";
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = false;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response(response, status, variables, error));
    REQUIRE(parsed);
    CHECK(status.is_object());
    CHECK(variables.is_object());
    CHECK(variables.at("box_count") == 2);
    CHECK(status.contains("box_stepper slot0"));
}

TEST_CASE("Qidi slot response rejects invalid JSON", "[QidiPrinterAgent]")
{
    nlohmann::json status;
    nlohmann::json variables;
    std::string    error;
    bool           parsed = true;

    REQUIRE_NOTHROW(parsed = QidiPrinterAgent::parse_slot_response("{not json", status, variables, error));
    CHECK_FALSE(parsed);
    CHECK(error == "Invalid JSON response");
}

TEST_CASE("A filament mapped to a Box slot saves that slot in its tool variable", "[QidiPrinterAgent]")
{
    // Filament 2 is mapped to slot 5; filaments 0 and 1 have no slot.
    // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name needs its own quotes.
    CHECK(QidiPrinterAgent::build_tool_slot_script("[-1, -1, 5]") == "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot5'\"");
}

TEST_CASE("Each mapped filament gets its own tool variable line", "[QidiPrinterAgent]")
{
    CHECK(QidiPrinterAgent::build_tool_slot_script("[3, -1, 0]") ==
          "SAVE_VARIABLE VARIABLE=value_t0 VALUE=\"'slot3'\"\n"
          "SAVE_VARIABLE VARIABLE=value_t2 VALUE=\"'slot0'\"");
}

TEST_CASE("A mapping without a usable Box slot saves nothing", "[QidiPrinterAgent]")
{
    const std::string mapping = GENERATE(as<std::string>{},
                                         "",           // empty mapping
                                         "[]",         // no filaments
                                         "[-1, -1]",   // no filament has a slot
                                         "[16]",       // past the firmware's slot15
                                         "[\"slot1\"]", // not a slot index
                                         "{\"0\": 1}", // not an array
                                         "[1,");       // not valid JSON
    CAPTURE(mapping);
    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping).empty());
}

TEST_CASE("Filaments past the firmware's last tool variable are skipped", "[QidiPrinterAgent]")
{
    // The firmware has value_t0 to value_t15: filament 15 maps to slot 1, filament 16 to slot 4.
    std::string mapping = "[";
    for (int filament = 0; filament < 15; ++filament)
        mapping += "-1, ";
    mapping += "1, 4]";

    CHECK(QidiPrinterAgent::build_tool_slot_script(mapping) == "SAVE_VARIABLE VARIABLE=value_t15 VALUE=\"'slot1'\"");
}

TEST_CASE("The print start of a 3MF names the plate to print", "[QidiPrinterAgent]")
{
    PrintParams params;
    params.plate_index = 2;

    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode.3mf") ==
          nlohmann::json{{"filename", "Cube.gcode.3mf"}, {"plateindex", "2"}});
    CHECK(QidiPrinterAgent::print_start_request(params, "Cube.gcode") == nlohmann::json{{"filename", "Cube.gcode"}});
}

TEST_CASE("Qidi print commands map each project filament to its box slot", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options = R"({"enable_box":true})";
    params.task_use_ams  = true;
    params.ams_mapping   = "[0,-1,5]";

    const auto gcode = print_commands(params);

    CHECK(contains(gcode, "SAVE_VARIABLE VARIABLE=enable_box VALUE=1"));
    // The firmware reads the value as a Python string literal, hence the nested quotes.
    CHECK(contains(gcode, R"(SAVE_VARIABLE VARIABLE=value_t0 VALUE="'slot0'")"));
    CHECK(contains(gcode, R"(SAVE_VARIABLE VARIABLE=value_t2 VALUE="'slot5'")"));
    CHECK_FALSE(sets_variable(gcode, "value_t1"));
}

TEST_CASE("Qidi print commands leave the slot mapping alone when the box is off", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options = R"({"enable_box":false})";
    params.task_use_ams  = true;
    params.ams_mapping   = "[0,1]";

    const auto gcode = print_commands(params);

    CHECK(contains(gcode, "SAVE_VARIABLE VARIABLE=enable_box VALUE=0"));
    CHECK_FALSE(sets_variable(gcode, "value_t0"));
    CHECK_FALSE(sets_variable(gcode, "value_t1"));
}

TEST_CASE("Qidi print commands fall back to the job's box use without an Enable Box choice", "[QidiPrinterAgent]")
{
    const bool use_ams = GENERATE(true, false);
    CAPTURE(use_ams);

    PrintParams params{};
    params.extra_options = GENERATE(as<std::string>{}, "", "{}", "not json", R"({"enable_box":"yes"})");
    params.task_use_ams  = use_ams;

    const auto gcode = print_commands(params);

    CHECK(contains(gcode, std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (use_ams ? "1" : "0")));
}

TEST_CASE("Qidi print commands reject a filament mapping that is not an array", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options = R"({"enable_box":true})";
    params.task_use_ams  = true;
    params.ams_mapping   = GENERATE(as<std::string>{}, "{}", "not json", "3");

    std::vector<std::string> gcode;
    std::string              error;
    CHECK_FALSE(QidiPrinterAgent::build_print_commands(params, true, gcode, error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Qidi print commands send no slot mapping when the job has none", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options = R"({"enable_box":true})";
    params.task_use_ams  = true;

    const auto gcode = print_commands(params);

    CHECK(contains(gcode, "SAVE_VARIABLE VARIABLE=enable_box VALUE=1"));
    CHECK_FALSE(sets_variable(gcode, "value_t0"));
}

TEST_CASE("Qidi print commands select bed leveling only when the printer has the macros", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.task_bed_leveling = GENERATE(true, false);
    CAPTURE(params.task_bed_leveling);

    const auto with_macros = print_commands(params, true);
    CHECK(contains(with_macros, params.task_bed_leveling ? "G31" : "G32"));
    CHECK_FALSE(contains(with_macros, params.task_bed_leveling ? "G32" : "G31"));

    const auto without_macros = print_commands(params, false);
    CHECK_FALSE(contains(without_macros, "G31"));
    CHECK_FALSE(contains(without_macros, "G32"));
}

TEST_CASE("Qidi print commands set the polar cooler only when the dialog offered it", "[QidiPrinterAgent]")
{
    PrintParams params{};

    SECTION("not offered")
    {
        CHECK_FALSE(sets_variable(print_commands(params), "enable_polar_cooler"));
    }
    SECTION("on")
    {
        params.extra_options = R"({"polar_cooler":true})";
        CHECK(contains(print_commands(params), "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=1"));
    }
    SECTION("off")
    {
        params.extra_options = R"({"polar_cooler":false})";
        CHECK(contains(print_commands(params), "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=0"));
    }
}

TEST_CASE("Qidi print commands enable the box before mapping slots", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options     = R"({"enable_box":true,"polar_cooler":true})";
    params.task_use_ams      = true;
    params.ams_mapping       = "[3]";
    params.task_bed_leveling = true;

    const auto gcode = print_commands(params);

    REQUIRE(gcode.size() == 4);
    CHECK(gcode[0] == "SAVE_VARIABLE VARIABLE=enable_box VALUE=1");
    CHECK(gcode[1] == R"(SAVE_VARIABLE VARIABLE=value_t0 VALUE="'slot3'")");
    CHECK(gcode[2] == "G31");
    CHECK(gcode[3] == "SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=1");
}

TEST_CASE("Qidi print commands keep the box off when the job maps no filament to it", "[QidiPrinterAgent]")
{
    PrintParams params{};
    params.extra_options = R"({"enable_box":true})";
    params.task_use_ams  = false;
    params.ams_mapping   = "[-1,-1]";

    const auto gcode = print_commands(params);

    CHECK(contains(gcode, "SAVE_VARIABLE VARIABLE=enable_box VALUE=0"));
    CHECK_FALSE(sets_variable(gcode, "value_t0"));
}

TEST_CASE("Qidi printer accepts a print only while idle", "[QidiPrinterAgent]")
{
    for (const char* idle : {"standby", "complete", "cancelled", "error"}) {
        CAPTURE(idle);
        CHECK(QidiPrinterAgent::is_ready_to_print(idle));
    }
    for (const char* busy : {"printing", "paused", "", "unknown"}) {
        CAPTURE(busy);
        CHECK_FALSE(QidiPrinterAgent::is_ready_to_print(busy));
    }
}
