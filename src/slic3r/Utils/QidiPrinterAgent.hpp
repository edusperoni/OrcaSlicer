#ifndef __QIDI_PRINTER_AGENT_HPP__
#define __QIDI_PRINTER_AGENT_HPP__

#include "IPrinterAgent.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "bambu_networking.hpp"
#include "nlohmann/json_fwd.hpp"

#include <atomic>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {

class QidiPrinterAgent final : public MoonrakerPrinterAgent
{
public:
    explicit QidiPrinterAgent(std::string log_dir);
    ~QidiPrinterAgent() override { shutdown(); }

    static AgentInfo get_agent_info_static();
    AgentInfo        get_agent_info() override { return get_agent_info_static(); }

    // Override filament sync (Qidi-specific implementation)
    bool fetch_filament_info(std::string dev_id, FilamentSyncMode sync_mode = FilamentSyncMode::pull) override;

    static bool parse_slot_response(const std::string& response_body,
                                    nlohmann::json&    status,
                                    nlohmann::json&    variables,
                                    std::string&       error);

    // Print operations — apply the job's QIDI print options, then delegate to base.
    // Only the LAN print path is supported by the base; the cloud/record/sdcard variants
    // are inherited and return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED.
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    FilamentSyncMode get_filament_sync_mode() const override;

    int connect_printer(const PrinterConnectionParams& params) override;

    std::vector<PrinterSendOption> get_send_options(const std::string& dev_id) const override;

    // Whether a print may be started while Klipper's print_stats is in this state.
    static bool is_ready_to_print(const std::string& print_state);

    // G-code scripts that apply the job's box, slot mapping, bed leveling and polar cooler
    // options, in the order they must run. Returns false when the filament mapping is malformed.
    static bool build_print_commands(const PrintParams&        params,
                                     bool                      has_leveling_macros,
                                     std::vector<std::string>& gcode,
                                     std::string&              error);

protected:
    void append_print_payload(nlohmann::json& print) const override;
    // Starts the print through Moonraker, whose print start copies the upload to where Qidi's Klipper reads it
    bool print_uploaded_file(const PrintParams& params, const ConnectionSettings& connection, const std::string& filename) override;
    std::string ams_unit_name(int ams_id) const override { return "Qidi Box " + std::to_string(ams_id + 1); }

private:
    // Checks the printer is idle, then sends the job's print options before a print starts.
    // Returns false, after reporting the reason through update_fn, if the print must not start.
    bool apply_print_options(const PrintParams& params, const OnUpdateStatusFn& update_fn) const;
    bool set_timelapse_enabled(const ConnectionSettings& connection, bool enabled, std::string& error) const;

    // Printer capabilities, refreshed by the filament fetch thread and read from the UI thread.
    std::atomic<bool> m_box_present{false};
    std::atomic<bool> m_polar_cooler{false};
    std::atomic<bool> m_leveling_macros{false};
    std::atomic<bool> m_timelapse{false};

    struct QidiFilamentDict
    {
        std::map<int, std::string> colors;
        std::map<int, std::string> filaments;
    };

    // Qidi-specific methods
    bool fetch_slot_info(const ConnectionSettings& connection,
                         const QidiFilamentDict&   dict,
                         const std::string&        series_id,
                         std::vector<AmsTrayData>& trays,
                         int&                      box_count,
                         std::string&              error);
    bool fetch_filament_dict(const ConnectionSettings& connection, QidiFilamentDict& dict, std::string& error) const;
    std::string normalize_filament_type(const std::string& filament_type);
    std::string infer_series_id(const std::string& model_id, const std::string& dev_name);
    std::string normalize_model_key(std::string value);

    // Static helpers
    static void parse_ini_section(const std::string& content, const std::string& section_name, std::map<int, std::string>& result);
    static void parse_filament_sections(const std::string& content, std::map<int, std::string>& result);
    static std::string map_filament_type_to_setting_id(const std::string& filament_type);
};

} // namespace Slic3r

#endif
