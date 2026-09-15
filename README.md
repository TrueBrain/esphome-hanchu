# esphome-hanchu

An [ESPHome](https://esphome.io) external component that reads a Hanchu ESS inverter locally over Bluetooth, without the Hanchu cloud.

Built for a Hanchu **HV ESS** (HESS-HY-T-10K inverter with a HOME-ESS-HV battery rack) on an ESP32-C6.
Other Hanchu inverters with an `HC:L1xx` Bluetooth logger will likely work, but are untested.

The component only reads; it never changes inverter settings.

## Usage

```yaml
external_components:
  - source: github://TrueBrain/esphome-hanchu
    components: [hanchu_ble]

esp32_ble_tracker:

ble_client:
  - mac_address: AA:BB:CC:DD:EE:FF  # the inverter's HC:L1xx logger
    id: inverter_ble

hanchu_ble:
  - id: inverter
    ble_client_id: inverter_ble
    update_interval: 30s

sensor:
  - platform: hanchu_ble
    hanchu_ble_id: inverter
    battery_soc:
      name: "Battery SOC"
    battery_power:
      name: "Battery power"
      group: flows
    grid_power:
      name: "Grid power"
      group: flows
    grid_import_total:
      name: "Grid import"
    custom:
      - key: P499
        name: "P499"

text_sensor:
  - platform: hanchu_ble
    hanchu_ble_id: inverter
    serial_number:
      name: "Inverter serial"
```

Every poll connects, reads and disconnects; the logger drops idle connections after about 10 seconds anyway.

## Configuration

`hanchu_ble`:

- `update_interval` (default `5s`): how often to poll.
- `request_timeout` (default `5s`): how long to wait for a reply.
- `max_keys_per_request` (default `12`): keys per request.
- `on_poll`: automation that runs after every poll with at least one reply.

Every sensor and text sensor also accepts:

- `update_interval`: read this value less often than the component polls. Text sensors default to once after boot.
- `group`: values in the same group are always read in one request, so they are from the same moment.

## Sensors

- Battery: `battery_soc`, `battery_power` (positive = discharging), `battery_voltage`, `battery_current` (positive = discharging), `battery_temperature`, `battery_charge_today`, `battery_discharge_today`, `battery_capacity`, `battery_state`
- Grid: `grid_power` (positive = import), `grid_import_total`, `grid_export_total`, `grid_import_midnight`, `grid_export_midnight`, `grid_voltage_l1`, `grid_current_l1`, `grid_frequency`, `inverter_active_power`, `inverter_reactive_power`, `power_factor`
- PV, DC coupled: `pv_power`, `pv_energy_today`, `pv_energy_total`, `pv1_voltage`, `pv1_current`, `pv2_voltage`, `pv2_current`, `pv3_voltage`, `pv3_current`
- PV, AC coupled: `pv_ac_power`, `pv_ac_power_l1`, `pv_ac_power_l2`, `pv_ac_power_l3`, `pv_ac_forward_total`, `pv_ac_reverse_total`, `pv_ac_meter_enabled`, `pv_ac_meter_direction`
- Backup output: `eps_voltage`, `eps_current`, `eps_frequency`, `eps_power`, `eps_reactive_power`, `eps_energy_today`, `eps_energy_total`
- Settings: `work_mode`, `charge_power_limit`, `discharge_power_limit`, `max_charge_soc`, `min_soc_on_grid`, `max_grid_charge_soc`
- Device: `rated_power`, `phase_mode`, `meter_type`, `utc_offset`

Text sensors: `serial_number`, `model`, `brand`, `firmware_main`, `firmware_safety`, `firmware_arm`, `logger_firmware`, `clock`, `timezone`

Units, device classes and state classes are set; any of them can be overridden.

Grid import and export come from the energy meter, which only keeps lifetime counters; energy today is `grid_import_total` minus `grid_import_midnight` (and the same for export).

Any other key can be read with `custom:`. Set the logger for `hanchu_ble` to `VERBOSE` to see every value the inverter returns.

## Notes

- The battery rack has its own Bluetooth logger, but it does not expose battery data; all battery values come from the inverter.
- The protocol and key mapping are based on the reverse engineering in [upton68/hanchu-ess-ble](https://github.com/upton68/hanchu-ess-ble).

## License

[MIT](LICENSE)
