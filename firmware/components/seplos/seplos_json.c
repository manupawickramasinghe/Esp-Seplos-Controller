#include "seplos_json.h"

seplos_temp_kind_t seplos_temp_kind(uint8_t idx, uint8_t temp_count)
{
    if (temp_count >= 3) {
        if (idx == temp_count - 2) {
            return SEPLOS_TEMP_AMBIENT;
        }
        if (idx == temp_count - 1) {
            return SEPLOS_TEMP_POWER;
        }
    }
    return SEPLOS_TEMP_CELL;
}

int32_t seplos_power_dw(const seplos_telemetry_t *t)
{
    /* cV * cA = 1e-4 W ; / 1000 -> 0.1 W, rounded */
    int32_t p = (int32_t)t->voltage_cv * (int32_t)t->current_ca;
    return p >= 0 ? (p + 500) / 1000 : -((-p + 500) / 1000);
}

void seplos_json_legacy(sbuf_t *sb, const seplos_pack_state_t *pack, const char *bat_name)
{
    const seplos_telemetry_t *t = &pack->tele;
    const char *err = pack->online ? "" : seplos_err_str(pack->last_err);

    sb_puts(sb, "{ \"EspError\" : ");
    sb_json_str(sb, err);
    sb_puts(sb, " , \"BatName\" : ");
    sb_json_str(sb, bat_name);
    sb_printf(sb, " , \"BMSAddress\" : %u", pack->address);
    sb_puts(sb, " , \"SOC\" : ");
    sb_fixed_as(sb, t->soc_pm, 1, 2);
    sb_puts(sb, " , \"RemainCapacity\" : ");
    sb_fixed(sb, t->remain_cah, 2);
    sb_puts(sb, " , \"BatteryVoltage\" : ");
    sb_fixed(sb, t->voltage_cv, 2);
    sb_puts(sb, " , \"ChargeDischargeCurrent\" : ");
    sb_fixed(sb, t->current_ca, 2);
    sb_puts(sb, " , \"CellMinV\" : ");
    sb_fixed(sb, t->cell_min_mv, 3);
    sb_printf(sb, " , \"CellMinIdx\" : %u", t->cell_min_idx);
    sb_puts(sb, " , \"CellMaxV\" : ");
    sb_fixed(sb, t->cell_max_mv, 3);
    sb_printf(sb, " , \"CellMaxIdx\" : %u", t->cell_max_idx);
    sb_printf(sb, " , \"CellDiffMv\" : %u", t->cell_diff_mv);
    sb_printf(sb, " , \"CellCount\" : %u", t->cell_count);
    sb_puts(sb, " , \"CellVoltages\" : {");
    for (unsigned i = 0; i < t->cell_count; i++) {
        sb_printf(sb, "%s\"Cell%u\" : ", i ? " , " : " ", i + 1);
        sb_fixed(sb, t->cell_mv[i], 3);
    }
    sb_puts(sb, " }");
    sb_printf(sb, " , \"TempCount\" : %u", t->temp_count);
    sb_puts(sb, " , \"Temperatures\" : {");
    for (unsigned i = 0; i < t->temp_count; i++) {
        sb_puts(sb, i ? " , " : " ");
        /* original naming: index 4 = AmbientTemp, 5 = PowerTemp */
        if (i == 4) {
            sb_puts(sb, "\"AmbientTemp\" : ");
        } else if (i == 5) {
            sb_puts(sb, "\"PowerTemp\" : ");
        } else {
            sb_printf(sb, "\"CellTemp%u\" : ", i + 1);
        }
        sb_fixed_as(sb, t->temp_dc[i], 1, 2);
    }
    sb_puts(sb, " }");
    sb_printf(sb, " , \"BatteryCycles\" : %u", t->cycles);
    sb_puts(sb, " , \"SOH\" : ");
    sb_fixed_as(sb, t->soh_pm, 1, 2);
    sb_puts(sb, " , \"BatteryCapacity\" : ");
    sb_fixed(sb, t->capacity_cah, 2);
    sb_puts(sb, " , \"RatedCapacity\" : ");
    sb_fixed(sb, t->rated_cah, 2);
    sb_puts(sb, " , \"BusVoltage\" : ");
    sb_fixed(sb, t->port_voltage_cv, 2);
    sb_puts(sb, " }");
}

static void bool_field(sbuf_t *sb, const char *name, bool v)
{
    sb_printf(sb, "\"%s\":%s,", name, v ? "true" : "false");
}

void seplos_json_pack(sbuf_t *sb, const seplos_pack_state_t *pack)
{
    const seplos_telemetry_t *t = &pack->tele;
    const seplos_alarms_t *a = &pack->alarms;

    sb_printf(sb, "{\"address\":%u,", pack->address);
    bool_field(sb, "online", pack->online);
    bool_field(sb, "has_data", pack->has_data);
    sb_printf(sb, "\"age_ms\":%lu,\"polls_ok\":%lu,\"polls_failed\":%lu,", (unsigned long)pack->age_ms,
              (unsigned long)pack->ok_count, (unsigned long)pack->err_count);
    sb_puts(sb, "\"error\":");
    sb_json_str(sb, pack->online ? "" : seplos_err_str(pack->last_err));
    sb_putc(sb, ',');
    if (!pack->has_data) {
        sb_trim_comma(sb);
        sb_putc(sb, '}');
        return;
    }

    sb_puts(sb, "\"soc\":");          sb_fixed(sb, t->soc_pm, 1);
    sb_puts(sb, ",\"soh\":");         sb_fixed(sb, t->soh_pm, 1);
    sb_puts(sb, ",\"voltage\":");     sb_fixed(sb, t->voltage_cv, 2);
    sb_puts(sb, ",\"current\":");     sb_fixed(sb, t->current_ca, 2);
    sb_puts(sb, ",\"power\":");       sb_fixed(sb, seplos_power_dw(t), 1);
    sb_puts(sb, ",\"remaining_ah\":"); sb_fixed(sb, t->remain_cah, 2);
    sb_puts(sb, ",\"capacity_ah\":"); sb_fixed(sb, t->capacity_cah, 2);
    sb_puts(sb, ",\"rated_ah\":");    sb_fixed(sb, t->rated_cah, 2);
    sb_printf(sb, ",\"cycles\":%u", t->cycles);
    sb_puts(sb, ",\"port_voltage\":"); sb_fixed(sb, t->port_voltage_cv, 2);

    sb_puts(sb, ",\"cells\":[");
    for (unsigned i = 0; i < t->cell_count; i++) {
        sb_fixed(sb, t->cell_mv[i], 3);
        sb_putc(sb, ',');
    }
    sb_trim_comma(sb);
    sb_puts(sb, "],\"cell_min_v\":");
    sb_fixed(sb, t->cell_min_mv, 3);
    sb_printf(sb, ",\"cell_min_index\":%u,\"cell_max_v\":", t->cell_min_idx + 1u);
    sb_fixed(sb, t->cell_max_mv, 3);
    sb_printf(sb, ",\"cell_max_index\":%u,\"cell_diff_mv\":%u", t->cell_max_idx + 1u, t->cell_diff_mv);

    sb_puts(sb, ",\"temps\":{\"cells\":[");
    bool have_amb = false, have_pwr = false;
    int32_t amb = 0, pwr = 0;
    for (uint8_t i = 0; i < t->temp_count; i++) {
        switch (seplos_temp_kind(i, t->temp_count)) {
        case SEPLOS_TEMP_AMBIENT: have_amb = true; amb = t->temp_dc[i]; break;
        case SEPLOS_TEMP_POWER: have_pwr = true; pwr = t->temp_dc[i]; break;
        default:
            sb_fixed(sb, t->temp_dc[i], 1);
            sb_putc(sb, ',');
        }
    }
    sb_trim_comma(sb);
    sb_putc(sb, ']');
    if (have_amb) { sb_puts(sb, ",\"ambient\":"); sb_fixed(sb, amb, 1); }
    if (have_pwr) { sb_puts(sb, ",\"power\":"); sb_fixed(sb, pwr, 1); }
    sb_putc(sb, '}');

    if (pack->has_alarms) {
        sb_puts(sb, ",\"status\":{");
        bool_field(sb, "discharging", a->system_state & 0x01);
        bool_field(sb, "charging", a->system_state & 0x02);
        bool_field(sb, "float_charge", a->system_state & 0x04);
        bool_field(sb, "standby", a->system_state & 0x10);
        bool_field(sb, "shutdown", a->system_state & 0x20);
        bool_field(sb, "discharge_switch", a->switch_state & 0x01);
        bool_field(sb, "charge_switch", a->switch_state & 0x02);
        bool_field(sb, "current_limit", a->switch_state & 0x04);
        bool_field(sb, "heating", a->switch_state & 0x08);
        sb_trim_comma(sb);
        sb_puts(sb, "},\"balancing\":[");
        for (unsigned i = 0; i < 16 && i < t->cell_count; i++) {
            if (a->balancing & (1u << i)) sb_printf(sb, "%u,", i + 1);
        }
        sb_trim_comma(sb);
        sb_puts(sb, "],\"disconnected\":[");
        for (unsigned i = 0; i < 16 && i < t->cell_count; i++) {
            if (a->disconnected & (1u << i)) sb_printf(sb, "%u,", i + 1);
        }
        sb_trim_comma(sb);
        sb_puts(sb, "],\"alarms\":[");
        for (unsigned e = 0; e < 8; e++) {
            for (unsigned b = 0; b < 8; b++) {
                const char *txt = seplos_alarm_event_text(e, b);
                if ((a->event[e] & (1u << b)) && txt) {
                    sb_puts(sb, "{\"text\":");
                    sb_json_str(sb, txt);
                    sb_printf(sb, ",\"protection\":%s},", seplos_alarm_event_is_protection(e, b) ? "true" : "false");
                }
            }
        }
        static const char *const LEVEL[] = { "", "low", "high" };
        for (unsigned i = 0; i < a->cell_count && i < SEPLOS_MAX_CELLS; i++) {
            if (a->cell_alarm[i]) {
                sb_printf(sb, "{\"text\":\"Cell %u voltage %s\",\"protection\":false},", i + 1,
                          a->cell_alarm[i] <= 2 ? LEVEL[a->cell_alarm[i]] : "fault");
            }
        }
        for (unsigned i = 0; i < a->temp_count && i < SEPLOS_MAX_TEMPS; i++) {
            if (a->temp_alarm[i]) {
                sb_printf(sb, "{\"text\":\"Temperature %u %s\",\"protection\":false},", i + 1,
                          a->temp_alarm[i] <= 2 ? LEVEL[a->temp_alarm[i]] : "fault");
            }
        }
        if (a->charge_current_alarm) sb_puts(sb, "{\"text\":\"Charge current alarm\",\"protection\":false},");
        if (a->total_voltage_alarm) sb_puts(sb, "{\"text\":\"Pack voltage alarm\",\"protection\":false},");
        if (a->discharge_current_alarm) sb_puts(sb, "{\"text\":\"Discharge current alarm\",\"protection\":false},");
        sb_trim_comma(sb);
        sb_printf(sb, "],\"alarm_count\":%u,\"alarm_raw\":\"%02X%02X%02X%02X%02X%02X%02X%02X\"", seplos_alarm_count(a),
                  a->event[0], a->event[1], a->event[2], a->event[3], a->event[4], a->event[5], a->event[6],
                  a->event[7]);
    }
    sb_putc(sb, '}');
}
