#include "QidiPrinterAgent.hpp"
#include "Http.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "IPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"
#include "bambu_networking.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "nlohmann/json.hpp"
#include <atomic>
#include <boost/algorithm/string.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/format.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/algorithm/string/case_conv.hpp>
#include <boost/log/trivial.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include "libslic3r/Preset.hpp"
#include <cstddef>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

namespace {

// Check whether any visible, compatible base preset in the collection has the given filament_id.
bool has_visible_base_preset(const PresetCollection& filaments, const std::string& filament_id)
{
    for (const auto& p : filaments.get_presets()) {
        if (p.is_visible && p.is_compatible
            && filaments.get_preset_base(p) == &p
            && p.filament_id == filament_id)
            return true;
    }
    return false;
}

// RAII release of the in-flight fetch slot; movable so a failed thread start still releases it.
struct InFlightGuard
{
    MoonrakerPrinterAgent* owner;
    explicit InFlightGuard(MoonrakerPrinterAgent& o) noexcept : owner(&o) {}
    InFlightGuard(InFlightGuard&& other) noexcept : owner(other.owner) { other.owner = nullptr; }
    InFlightGuard(const InFlightGuard&) = delete;
    InFlightGuard& operator=(const InFlightGuard&) = delete;
    InFlightGuard& operator=(InFlightGuard&&) = delete;
    ~InFlightGuard() { if (owner) owner->release_fetch_slot(); }
};

// nlohmann::json::value() returns the default only when the key is absent; a present but
// null/wrong-typed value throws. Firmware JSON is untrusted, so read defensively.
int read_int_or(const nlohmann::json& obj, const std::string& key, int fallback)
{
    auto it = obj.find(key);
    if (it == obj.end()) {
        return fallback;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    // Firmware may report an integral field as a JSON float (e.g. 2.0). Accept it only when it
    // has no fractional part, so a genuinely fractional value falls back instead of truncating.
    if (it->is_number_float()) {
        const double value = it->get<double>();
        if (value == std::floor(value)) {
            return static_cast<int>(value);
        }
    }
    return fallback;
}

// The SAVE_VARIABLE line for each filament with a Box slot, which makes its T<n> macro load that slot
std::vector<std::string> tool_slot_lines(const nlohmann::json& mapping)
{
    // The firmware has value_t0 to value_t15 and slots 0 to 15
    constexpr int slot_count = 16;

    // One entry per filament: its Box slot index, negative when no slot was chosen
    std::vector<std::string> lines;
    if (!mapping.is_array()) {
        return lines;
    }
    for (int filament = 0; filament < int(mapping.size()) && filament < slot_count; ++filament) {
        if (!mapping[filament].is_number_integer()) {
            continue;
        }
        const int slot = mapping[filament].get<int>();
        if (slot < 0 || slot >= slot_count) {
            continue;
        }
        // SAVE_VARIABLE reads VALUE as a Python literal, so the slot name keeps its own quotes
        lines.push_back("SAVE_VARIABLE VARIABLE=value_t" + std::to_string(filament) + " VALUE=\"'slot" + std::to_string(slot) + "'\"");
    }
    return lines;
}

} // anonymous namespace

const std::string QidiPrinterAgent_VERSION = "0.0.1";

QidiPrinterAgent::QidiPrinterAgent(std::string log_dir) : MoonrakerPrinterAgent(std::move(log_dir))
{
}

AgentInfo QidiPrinterAgent::get_agent_info_static()
{
    return AgentInfo{QIDI_PRINTER_AGENT_ID, "Qidi", QidiPrinterAgent_VERSION, "Qidi printer agent"};
}

FilamentSyncMode QidiPrinterAgent::get_filament_sync_mode() const
{
    if (GUI::wxGetApp().app_config->get_bool("use_printer_agents"))
        return FilamentSyncMode::subscription;
    return FilamentSyncMode::pull;
}

bool QidiPrinterAgent::fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode)
{
    if (sync_mode != get_filament_sync_mode())
        return false;

    // Snapshot what the fetch needs; a reconnect can rewrite device_info meanwhile.
    ConnectionSettings connection = get_connection_settings();
    std::string        model_id;
    std::string        model_name;
    {
        std::lock_guard<std::recursive_mutex> lock(connect_mutex);
        model_id   = device_info.model_id;
        model_name = device_info.model_name;
    }

    // One implementation for both modes; only where it runs differs. pull is the documented
    // blocking contract (the GUI thread reads DevFilaSystem right after the call), so it runs
    // on the caller with the payload applied inline. subscription runs on a background thread
    // so the status loop is not stalled, and the payload is marshalled back to the main thread.
    auto work = [this, connection = std::move(connection), model_id, model_name](bool apply_inline) -> bool {
        try {
            std::string error;

            // 1. Fetch device info and infer series_id
            std::string series_id;
            {
                MoonrakerDeviceInfo info;
                if (fetch_device_info(connection, info, error, {})) {
                    series_id   = infer_series_id(info.model_id, info.dev_name);
                    m_timelapse = info.components.count("timelapse") != 0;
                }
            }
            {
                std::set<std::string> objects;
                if (fetch_object_list(connection, objects, error, {})) {
                    // Klipper keeps config section names as written, so match them case-insensitively.
                    const auto has_object = [&objects](const std::string& name) {
                        return std::any_of(objects.begin(), objects.end(),
                                           [&name](const std::string& object) { return boost::iequals(object, name); });
                    };
                    m_polar_cooler    = has_object("output_pin polar_cooler");
                    m_leveling_macros = has_object("gcode_macro G31") && has_object("gcode_macro G32");
                } else {
                    BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch object list: " << error;
                }
            }
            if (series_id.empty()) {
                // Fall back to the configured Orca model if Moonraker doesn't expose a usable identifier.
                series_id = infer_series_id(model_id, model_name);
            }

            // 2. Fetch filament dictionary
            QidiFilamentDict dict;
            if (!fetch_filament_dict(connection, dict, error)) {
                BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch filament dict: " << error;
            }

            // 3. Fetch slot info and build AmsTrayData directly
            std::vector<AmsTrayData> trays;
            int box_count = 0;
            if (!fetch_slot_info(connection, dict, series_id, trays, box_count, error)) {
                BOOST_LOG_TRIVIAL(warning) << "QidiPrinterAgent::fetch_filament_info: Failed to fetch slot info: " << error;
                return false;
            }

            // 4. Build the AMS payload, resolving preset-dependent ids on the main thread.
            auto resolve_ids = [](std::vector<AmsTrayData>& resolved_trays) {
                auto* bundle = GUI::wxGetApp().preset_bundle;
                if (!bundle) {
                    return; // keep the Qidi-specific ids computed from device data
                }
                for (auto& tray : resolved_trays) {
                    if (!tray.has_filament) {
                        continue;
                    }
                    if (!tray.tray_info_idx.empty() && has_visible_base_preset(bundle->filaments, tray.tray_info_idx)) {
                        continue;
                    }
                    tray.tray_info_idx = bundle->filaments.filament_id_by_type(tray.tray_type);
                    if (tray.tray_info_idx.empty())
                        tray.tray_info_idx = map_filament_type_to_generic_id(tray.tray_type);
                }
            };
            build_ams_payload(box_count, box_count * 4 - 1, trays, apply_inline, resolve_ids);
            return true;
        } catch (const std::exception& e) {
            // why: an exception escaping a detached thread is std::terminate, and firmware
            // JSON is untrusted; mirror run_command_worker and swallow it here.
            BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: unhandled exception: " << e.what();
            return false;
        } catch (...) {
            BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::fetch_filament_info: unhandled exception";
            return false;
        }
    };

    if (sync_mode == FilamentSyncMode::pull) {
        return work(/*apply_inline=*/true);
    }

    // Subscription: fire-and-forget. A `true` return means the refresh was scheduled (or is
    // already running), not that DevFilaSystem has been updated. Reserve under the same mutex
    // shutdown() uses, so the flag and the count can't race.
    {
        std::lock_guard<std::mutex> lock(fetch_lifecycle_mutex);
        if (shutting_down.load())
            return false;
        if (filament_fetch_in_flight.load() > 0)
            return true; // a fetch is already running; don't pile on
        filament_fetch_in_flight.fetch_add(1, std::memory_order_relaxed);
    }

    InFlightGuard guard{*this};
    std::thread([work = std::move(work), guard = std::move(guard)]() mutable {
        work(/*apply_inline=*/false);
    }).detach();
    return true;
}

std::string QidiPrinterAgent::build_tool_slot_script(const std::string& ams_mapping)
{
    return boost::algorithm::join(tool_slot_lines(nlohmann::json::parse(ams_mapping, nullptr, false)), "\n");
}

int QidiPrinterAgent::connect_printer(const PrinterConnectionParams& params)
{
    // The agent instance is shared by every Qidi preset; forget the previous printer's capabilities.
    m_box_present     = false;
    m_polar_cooler    = false;
    m_leveling_macros = false;
    m_timelapse       = false;
    return MoonrakerPrinterAgent::connect_printer(params);
}

std::vector<PrinterSendOption> QidiPrinterAgent::get_send_options(const std::string& /*dev_id*/) const
{
    std::vector<PrinterSendOption> options;
    if (m_box_present)
        options.push_back({"enable_box", _u8L("Enable Box"),
                           _u8L("Feed filament from the QIDI Box. When off, the printer prints from the external spool."), true});
    if (m_polar_cooler)
        options.push_back({"polar_cooler", _u8L("Enable Polar Cooler"), _u8L("Run the polar cooler fan during the print."), false});
    return options;
}

void QidiPrinterAgent::append_print_payload(nlohmann::json& print) const
{
    print["support_timelapse"] = m_timelapse.load();
}

bool QidiPrinterAgent::is_ready_to_print(const std::string& print_state)
{
    // Klipper keeps the last job's end state until the next print starts.
    return print_state == "standby" || print_state == "complete" || print_state == "cancelled" || print_state == "error";
}

bool QidiPrinterAgent::build_print_commands(const PrintParams&        params,
                                            bool                      has_leveling_macros,
                                            std::vector<std::string>& gcode,
                                            std::string&              error)
{
    gcode.clear();

    nlohmann::json extra = nlohmann::json::parse(params.extra_options, nullptr, false);
    if (!extra.is_object())
        extra = nlohmann::json::object();
    const auto option = [&extra](const char* key) -> std::optional<bool> {
        const auto it = extra.find(key);
        if (it == extra.end() || !it->is_boolean())
            return std::nullopt;
        return it->get<bool>();
    };

    // Enable Box can only turn the box off: a job that maps no filament to the box writes no
    // value_t<n>, so an enabled box would load the slots a previous job saved.
    const bool use_box = params.task_use_ams && option("enable_box").value_or(true);
    gcode.push_back(std::string("SAVE_VARIABLE VARIABLE=enable_box VALUE=") + (use_box ? "1" : "0"));

    // The firmware's T<n> macro loads from the slot saved in value_t<n>, so each project
    // filament index maps to its box slot. A box that is off ignores them.
    if (use_box && !params.ams_mapping.empty()) {
        const nlohmann::json mapping = nlohmann::json::parse(params.ams_mapping, nullptr, false);
        if (!mapping.is_array()) {
            error = "invalid filament mapping: " + params.ams_mapping;
            return false;
        }
        const std::vector<std::string> slots = tool_slot_lines(mapping);
        gcode.insert(gcode.end(), slots.begin(), slots.end());
    }

    // G31/G32 switch the mesh the firmware's G29 builds; PRINT_END resets it, so it is sent every job.
    if (has_leveling_macros)
        gcode.push_back(params.task_bed_leveling ? "G31" : "G32");

    if (const auto polar_cooler = option("polar_cooler"))
        gcode.push_back(std::string("SAVE_VARIABLE VARIABLE=enable_polar_cooler VALUE=") + (*polar_cooler ? "1" : "0"));

    return true;
}

bool QidiPrinterAgent::set_timelapse_enabled(const ConnectionSettings& connection, bool enabled, std::string& error) const
{
    bool success = false;
    auto http    = Http::post(join_url(connection.base_url, std::string("/machine/timelapse/settings?enabled=") + (enabled ? "true" : "false")));
    configure_http(http, connection);
    if (!connection.api_key.empty())
        http.header("X-Api-Key", connection.api_key);
    http.header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string, unsigned status) {
            success = status == 200;
            if (!success)
                error = "HTTP error: " + std::to_string(status);
        })
        .on_error([&](std::string, std::string err, unsigned status) {
            error = status > 0 ? err + " (HTTP " + std::to_string(status) + ")" : err;
        })
        .perform_sync();
    return success;
}

bool QidiPrinterAgent::apply_print_options(const PrintParams& params, const OnUpdateStatusFn& update_fn) const
{
    const auto fail = [&update_fn](const std::string& detail) {
        BOOST_LOG_TRIVIAL(error) << "QidiPrinterAgent::apply_print_options: " << detail;
        if (update_fn)
            update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, detail);
        return false;
    };

    const ConnectionSettings connection = get_connection_settings();

    nlohmann::json status;
    std::string    error;
    if (!query_printer_status(connection, status, error, {}))
        return fail((boost::format(_u8L("Failed to read the printer state: %1%")) % error).str());
    const std::string state = status.contains("print_stats") && status["print_stats"].is_object()
                                  ? status["print_stats"].value("state", "")
                                  : "";
    if (state.empty())
        return fail(_u8L("The printer is not ready. Check that Klipper is running."));
    if (!is_ready_to_print(state))
        return fail((boost::format(_u8L("The printer is busy (%1%).")) % state).str());

    std::vector<std::string> gcode;
    if (!build_print_commands(params, m_leveling_macros, gcode, error))
        return fail(error);
    // One script request, as each gcode/script call blocks until Klipper has run it.
    if (!send_gcode(connection.dev_id, boost::algorithm::join(gcode, "\n"), connection))
        return fail(_u8L("Failed to apply the print options."));

    if (m_timelapse && !set_timelapse_enabled(connection, params.task_record_timelapse, error))
        return fail((boost::format(_u8L("Failed to set timelapse: %1%")) % error).str());

    return true;
}

int QidiPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    // Only the LAN print path is implemented by the Moonraker base; the print options are applied
    // here, before the print starts. The cloud/record/sdcard variants inherited from the base
    // deliberately return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED without side effects.
    if (!apply_print_options(params, update_fn))
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    return MoonrakerPrinterAgent::start_local_print(std::move(params), update_fn, cancel_fn);
}

bool QidiPrinterAgent::print_uploaded_file(const PrintParams& params, const ConnectionSettings& connection, const std::string& filename)
{
    // On the X-Max 4 and Q2, Klipper only opens the copy Moonraker's print start makes, so SDCARD_PRINT_FILE can't find the upload
    std::string response;
    return send_jsonrpc_command(connection, print_start_request(params, filename), response);
}

nlohmann::json QidiPrinterAgent::print_start_request(const PrintParams& params, const std::string& filename)
{
    nlohmann::json request{{"filename", filename}};
    // Qidi's Moonraker prints Metadata/plate_<plateindex>.gcode from a 3MF, defaulting to plate 1
    if (boost::iends_with(filename, ".3mf"))
        request["plateindex"] = std::to_string(params.plate_index);
    return request;
}

bool QidiPrinterAgent::fetch_slot_info(const ConnectionSettings& connection,
                                       const QidiFilamentDict&   dict,
                                       const std::string&        series_id,
                                       std::vector<AmsTrayData>& trays,
                                       int&                      box_count,
                                       std::string&              error)
{
    std::string url = join_url(connection.base_url, "/printer/objects/query?save_variables=variables&multi_color_controller=box");
    for (int i = 0; i < 16; ++i) {
        url += "&box_stepper%20slot" + std::to_string(i) + "=runout_button";
    }

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    configure_http(http, connection);
    if (!connection.api_key.empty()) {
        http.header("X-Api-Key", connection.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    nlohmann::json status;
    nlohmann::json variables;
    if (!parse_slot_response(response_body, status, variables, error))
        return false;

    box_count = read_int_or(variables, "box_count", 1);
    if (box_count < 0) {
        box_count = 0;
    }
    m_box_present = box_count > 0;

    const int max_slots = box_count * 4;
    trays.clear();
    trays.reserve(max_slots);

    // Lambda to build setting_id from slot data
    auto build_setting_id = [&](int filament_type_idx, int vendor_type, const std::string& tray_type) {
        const int vendor = (vendor_type == 1) ? 1 : 0;
        if (is_numeric(series_id) && filament_type_idx > 0) {
            return "QD_" + series_id + "_" + std::to_string(vendor) + "_" + std::to_string(filament_type_idx);
        }
        return map_filament_type_to_setting_id(tray_type);
    };

    for (int i = 0; i < max_slots; ++i) {
        AmsTrayData tray;
        tray.slot_index = i;

        // Read slot variables. Newer firmware stores a hex color list and a material id instead of dictionary
        // indexes, and only in the slots it has rewritten, so each slot is checked for both forms.
        const auto color_value    = variables.value("color_slot" + std::to_string(i), nlohmann::json(1));
        const auto filament_value = variables.value("filament_slot" + std::to_string(i), nlohmann::json(1));
        const int color_index     = color_value.is_number() ? color_value.get<int>() : 0;
        const int filament_type   = filament_value.is_number() ? filament_value.get<int>() : 0;
        const int vendor_type     = variables.value("vendor_slot" + std::to_string(i), 0);

        // Check filament presence via runout sensor
        std::string box_stepper_key = "box_stepper slot" + std::to_string(i);
        tray.has_filament = false;
        if (status.contains(box_stepper_key)) {
            auto& box_stepper = status[box_stepper_key];
            if (box_stepper.contains("runout_button") && !box_stepper["runout_button"].is_null()) {
                const int runout_button = read_int_or(box_stepper, "runout_button", 0);
                tray.has_filament = (runout_button == 0);
            }
        }

        if (tray.has_filament) {
            // Look up filament type name from dictionary
            std::string filament_name = "PLA";
            auto filament_it = dict.filaments.find(filament_type);
            if (filament_it != dict.filaments.end()) {
                filament_name = filament_it->second;
            }
            if (filament_value.is_string()) {
                const nlohmann::json::json_pointer decoded_type("/multi_color_controller/box/box" + std::to_string(i / 4) +
                                                                "_filament_info_" + std::to_string(i % 4) + "_filament_type");
                if (status.contains(decoded_type) && status[decoded_type].is_string()) {
                    filament_name = status[decoded_type].get<std::string>();
                }
            }
            tray.tray_type = normalize_filament_type(filament_name);

            // Qidi-specific setting id derived from device data only; the preset-dependent
            // fallback runs on the main thread in the resolve hook passed to build_ams_payload.
            tray.tray_info_idx = build_setting_id(filament_type, vendor_type, tray.tray_type);

            // Look up color from dictionary
            auto color_it = dict.colors.find(color_index);
            if (color_value.is_array() && !color_value.empty() && color_value[0].is_string()) {
                tray.tray_color = color_value[0].get<std::string>();
            } else if (color_it != dict.colors.end()) {
                tray.tray_color = color_it->second;
            } else {
                tray.tray_color = "FFFFFFFF";
            }
        }

        trays.push_back(tray);
    }

    return true;
}

bool QidiPrinterAgent::parse_slot_response(const std::string& response_body,
                                           nlohmann::json&    status,
                                           nlohmann::json&    variables,
                                           std::string&       error)
{
    auto json = nlohmann::json::parse(response_body, nullptr, false, true);
    if (json.is_discarded()) {
        error = "Invalid JSON response";
        return false;
    }

    if (!json.is_object() || !json.contains("result") || !json["result"].is_object() || !json["result"].contains("status") ||
        !json["result"]["status"].is_object() || !json["result"]["status"].contains("save_variables") ||
        !json["result"]["status"]["save_variables"].is_object() || !json["result"]["status"]["save_variables"].contains("variables") ||
        !json["result"]["status"]["save_variables"]["variables"].is_object()) {
        // why: Qidi firmware may send null here, but json::value() throws for it.
        error = "Unexpected JSON structure: save_variables.variables must be an object";
        return false;
    }

    status    = json["result"]["status"];
    variables = status["save_variables"]["variables"];
    return true;
}

bool QidiPrinterAgent::fetch_filament_dict(const ConnectionSettings& connection,
                                           QidiFilamentDict& dict,
                                           std::string& error) const
{
    std::string url = join_url(connection.base_url, "/server/files/config/officiall_filas_list.cfg");

    std::string response_body;
    bool        success = false;
    std::string http_error;

    auto http = Http::get(url);
    configure_http(http, connection);
    if (!connection.api_key.empty()) {
        http.header("X-Api-Key", connection.api_key);
    }
    http.timeout_connect(5)
        .timeout_max(10)
        .on_complete([&](std::string body, unsigned status) {
            if (status == 200) {
                response_body = body;
                success       = true;
            } else {
                http_error = "HTTP error: " + std::to_string(status);
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            http_error = err;
            if (status > 0) {
                http_error += " (HTTP " + std::to_string(status) + ")";
            }
        })
        .perform_sync();

    if (!success) {
        error = http_error.empty() ? "Connection failed" : http_error;
        return false;
    }

    dict.colors.clear();
    dict.filaments.clear();
    parse_ini_section(response_body, "colordict", dict.colors);
    parse_filament_sections(response_body, dict.filaments);

    return !dict.colors.empty();
}

void QidiPrinterAgent::parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    bool               in_section     = false;
    std::string        section_header = "[" + section_name + "]";

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            in_section = (line == section_header);
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (in_section) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                try {
                    int index     = std::stoi(key);
                    result[index] = value;
                } catch (...) {}
            }
        }
    }
}

void QidiPrinterAgent::parse_filament_sections(const std::string& content, std::map<int, std::string>& result)
{
    std::istringstream stream(content);
    std::string        line;
    int                current_fila_index = -1;

    while (std::getline(stream, line)) {
        boost::trim(line);
        if (!line.empty() && line[0] == '[') {
            current_fila_index = -1;
            if (line.size() > 5 && line.substr(0, 5) == "[fila" && line.back() == ']') {
                std::string num_str = line.substr(5, line.size() - 6);
                try {
                    current_fila_index = std::stoi(num_str);
                } catch (...) {
                    current_fila_index = -1;
                }
            }
            continue;
        }
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        if (current_fila_index > 0) {
            auto pos = line.find('=');
            if (pos != std::string::npos) {
                std::string key   = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                boost::trim(key);
                boost::trim(value);
                if (key == "filament") {
                    result[current_fila_index] = value;
                }
            }
        }
    }
}

std::string QidiPrinterAgent::map_filament_type_to_setting_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper == "PLA") {
        return "QD_1_0_1";
    }
    if (upper == "ABS") {
        return "QD_1_0_11";
    }
    if (upper == "PETG") {
        return "QD_1_0_41";
    }
    if (upper == "TPU") {
        return "QD_1_0_50";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_model_key(std::string value)
{
    boost::algorithm::to_lower(value);
    std::string normalized;
    normalized.reserve(value.size());
    for (unsigned char c : value) {
        if (std::isalnum(c)) {
            normalized.push_back(static_cast<char>(c));
        }
    }
    return normalized;
}

std::string QidiPrinterAgent::infer_series_id(const std::string& model_id, const std::string& dev_name)
{
    std::string source = model_id.empty() ? dev_name : model_id;
    boost::trim(source);
    if (source.empty()) {
        return "";
    }
    if (is_numeric(source)) {
        return source;
    }

    const std::string key = normalize_model_key(source);
    if (key.find("q2") != std::string::npos) {
        return "1";
    }
    if (key.find("xmax") != std::string::npos && key.find("4") != std::string::npos) {
        return "3";
    }
    if ((key.find("xplus") != std::string::npos || key.find("plus") != std::string::npos) && key.find("4") != std::string::npos) {
        return "0";
    }
    return "";
}

std::string QidiPrinterAgent::normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    if (upper.find("PLA") != std::string::npos)
        return "PLA";
    if (upper.find("ABS") != std::string::npos)
        return "ABS";
    if (upper.find("PETG") != std::string::npos)
        return "PETG";
    if (upper.find("TPU") != std::string::npos)
        return "TPU";
    if (upper.find("ASA") != std::string::npos)
        return "ASA";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";
    if (upper.find("PC") != std::string::npos)
        return "PC";
    if (upper.find("PVA") != std::string::npos)
        return "PVA";

    return upper;
}

} // namespace Slic3r
