#include "edge_derived.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *cursor;
    edge_expression_variable variable;
    void *context;
    unsigned nodes;
    bool valid;
    bool validation;
} expression;
static void space(expression *p) { while (*p->cursor && strchr(" \t\r\n", *p->cursor)) ++p->cursor; }
static bool take(expression *p, const char *token) {
    space(p);
    size_t size = strlen(token);
    if (strncmp(p->cursor, token, size)) return false;
    p->cursor += size;
    return true;
}
static double parse(expression *, int, unsigned, bool);
static double atom(expression *p, unsigned depth, bool evaluate) {
    if (depth > 24) { p->valid = false; return 0; }
    if (take(p, "(")) { double value = parse(p, 0, depth + 1, evaluate); if (!take(p, ")")) p->valid = false; return value; }
    if (++p->nodes > 128) { p->valid = false; return 0; }
    if (take(p, "!")) return atom(p, depth + 1, evaluate) == 0;
    if (take(p, "-")) return -atom(p, depth + 1, evaluate);
    if (take(p, "+")) return atom(p, depth + 1, evaluate);
    space(p);
    if (isalpha((unsigned char)*p->cursor) || *p->cursor == '_') {
        char name[33]; size_t size = 0;
        while (isalnum((unsigned char)*p->cursor) || *p->cursor == '_') {
            if (size == sizeof(name) - 1) { p->valid = false; return 0; }
            name[size++] = *p->cursor++;
        }
        name[size] = 0;
        if (!strcmp(name, "true")) return 1;
        if (!strcmp(name, "false")) return 0;
        if (take(p, "(")) {
            double a = parse(p, 0, depth + 1, evaluate), b = 0, c = 0;
            unsigned count = 1;
            bool conditional = !strcmp(name, "if");
            if (take(p, ",")) { b = parse(p, 0, depth + 1, evaluate && (!conditional || a != 0)); count = 2; }
            if (take(p, ",")) { c = parse(p, 0, depth + 1, evaluate && (!conditional || a == 0)); count = 3; }
            if (!take(p, ")")) p->valid = false;
            if (conditional && count == 3) return a != 0 ? b : c;
            if (!strcmp(name, "min") && count == 2) return fmin(a, b);
            if (!strcmp(name, "max") && count == 2) return fmax(a, b);
            if (!strcmp(name, "abs") && count == 1) return fabs(a);
            if (!strcmp(name, "round") && count == 1) return round(a);
            if (!strcmp(name, "sqrt") && count == 1) { if (evaluate && a < 0) p->valid = false; return evaluate && a >= 0 ? sqrt(a) : 0; }
            p->valid = false; return 0;
        }
        double value = 0;
        if ((evaluate || p->validation) && (!p->variable(p->context, name, &value) || !isfinite(value))) p->valid = false;
        return value;
    }
    char *end = NULL;
    if (p->cursor[0] == '0' && (p->cursor[1] == 'x' || p->cursor[1] == 'X')) { p->valid = false; return 0; }
    double value = strtod(p->cursor, &end);
    if (end == p->cursor || !isfinite(value)) { p->valid = false; return 0; }
    p->cursor = end;
    return value;
}
static double parse(expression *p, int minimum, unsigned depth, bool evaluate) {
    double left = atom(p, depth, evaluate);
    static const char *const operators[] = {"||", "&&", "==", "!=", "<=", ">=", "<", ">", "+", "-", "*", "/", "%"};
    for (;;) {
        if (!p->valid) return 0;
        space(p);
        int op = -1, precedence = -1;
        for (int i = 0; i < 13; ++i) if (!strncmp(p->cursor, operators[i], strlen(operators[i]))) {
            op = i; precedence = i < 2 ? i : i < 4 ? 2 : i < 8 ? 3 : i < 10 ? 4 : 5; break;
        }
        if (precedence < minimum) return left;
        p->cursor += strlen(operators[op]);
        bool right_active = evaluate && !(op == 0 && left != 0) && !(op == 1 && left == 0);
        double right = parse(p, precedence + 1, depth + 1, right_active);
        if (++p->nodes > 128) p->valid = false;
        if (!evaluate) { left = 0; continue; }
        switch (op) {
            case 0: left = left != 0 || right != 0; break;
            case 1: left = left != 0 && right != 0; break;
            case 2: left = left == right; break;
            case 3: left = left != right; break;
            case 4: left = left <= right; break;
            case 5: left = left >= right; break;
            case 6: left = left < right; break;
            case 7: left = left > right; break;
            case 8: left += right; break;
            case 9: left -= right; break;
            case 10: left *= right; break;
            case 11: case 12:
                if (right == 0) { p->valid = false; return 0; }
                left = op == 11 ? left / right : fmod(left, right); break;
        }
        if (!isfinite(left)) p->valid = false;
    }
}
bool edge_expression_evaluate(const char *text, edge_expression_variable variable, void *context, double *result) {
    if (!text || !*text || strlen(text) > 512 || !variable || !result) return false;
    expression p = {text, variable, context, 0, true, false};
    *result = parse(&p, 0, 0, true); space(&p);
    return p.valid && !*p.cursor && isfinite(*result);
}
typedef struct { int64_t at; double value; } window_sample;
typedef struct {
    char id[65], unit[33], quality[25];
    double value;
    int64_t at;
    bool valid, updated;
} latest_sample;
typedef struct {
    const iot_edge_v1_DerivedPointConfig *config;
    window_sample *window;
    size_t count;
    char unit[33];
    int64_t overflow_until;
    iot_edge_v1_TelemetryValue output;
} derived_rule;
struct edge_derived {
    derived_rule *rules;
    size_t count, latest_count, capacity, window_capacity;
    latest_sample *latest;
};
static void text(char *to, size_t capacity, const char *from) { snprintf(to, capacity, "%s", from); }
static latest_sample *find(edge_derived *state, const char *id) {
    for (size_t i = 0; i < state->latest_count; ++i) if (!strcmp(state->latest[i].id, id)) return &state->latest[i];
    if (state->latest_count == state->capacity) return NULL;
    latest_sample *value = &state->latest[state->latest_count++];
    text(value->id, sizeof(value->id), id);
    return value;
}
static bool scalar(const iot_edge_v1_TelemetryValue *value, double *number) {
    if (!value->has_value) return false;
    switch (value->value.which_value) {
        case iot_edge_v1_ScalarValue_bool_value_tag: *number = value->value.value.bool_value ? 1 : 0; break;
        case iot_edge_v1_ScalarValue_signed_value_tag: *number = (double)value->value.value.signed_value; break;
        case iot_edge_v1_ScalarValue_unsigned_value_tag: *number = (double)value->value.value.unsigned_value; break;
        case iot_edge_v1_ScalarValue_double_value_tag: *number = value->value.value.double_value; break;
        case iot_edge_v1_ScalarValue_decimal_value_tag: {
            char *end = NULL;
            *number = strtod(value->value.value.decimal_value, &end);
            if (end == value->value.value.decimal_value || *end) return false;
            break;
        }
        default: return false;
    }
    return isfinite(*number);
}
typedef struct { edge_derived *state; derived_rule *rule; int64_t now; const char *error; } evaluation;
static bool variable(void *context, const char *name, double *number) {
    evaluation *evaluation = context;
    const iot_edge_v1_DerivedPointConfig *rule = evaluation->rule->config;
    for (pb_size_t i = 0; i < rule->inputs_count; ++i) if (!strcmp(name, rule->inputs[i].alias)) {
        latest_sample *sample = find(evaluation->state, rule->inputs[i].point_id);
        if (!sample || !sample->valid) { evaluation->error = "missing"; return false; }
        if (sample->at > evaluation->now || evaluation->now - sample->at >= (int64_t)rule->max_age_seconds * 1000) { evaluation->error = "stale"; return false; }
        *number = sample->value;
        return true;
    }
    evaluation->error = "invalid";
    return false;
}
static bool bound_variable(void *context, const char *name, double *number) {
    const iot_edge_v1_DerivedPointConfig *rule = context;
    *number = 1;
    for (pb_size_t i = 0; i < rule->inputs_count; ++i) if (!strcmp(name, rule->inputs[i].alias)) return true;
    return false;
}
static bool valid_expression(const char *source, const iot_edge_v1_DerivedPointConfig *rule) {
    if (!*source || strlen(source) > 512) return false;
    expression parser = {source, bound_variable, (void *)rule, 0, true, true};
    (void)parse(&parser, 0, 0, false);
    space(&parser);
    return parser.valid && !*parser.cursor;
}
bool edge_derived_validate(const iot_edge_v1_DerivedPointConfig *rule) {
    if (!rule || rule->device_id.size != 16 || !*rule->point_id || !*rule->name || !rule->inputs_count || rule->inputs_count > 16 || rule->unit_rules_count > 8 || !rule->max_age_seconds || rule->max_age_seconds > 86400) return false;
    if (strcmp(rule->kind, "expression") && strcmp(rule->kind, "average") && strcmp(rule->kind, "minimum") && strcmp(rule->kind, "maximum")) return false;
    if (strcmp(rule->kind, "expression") && (!rule->window_seconds || rule->window_seconds > 86400 || rule->boolean_result || !*rule->source_alias)) return false;
    for (pb_size_t i = 0; i < rule->inputs_count; ++i) {
        if (!*rule->inputs[i].point_id || !*rule->inputs[i].alias) return false;
        const char *alias = rule->inputs[i].alias;
        if ((!isalpha((unsigned char)*alias) && *alias != '_') || !strcmp(alias, "true") || !strcmp(alias, "false")) return false;
        for (; *alias; ++alias) if (!isalnum((unsigned char)*alias) && *alias != '_') return false;
        for (pb_size_t j = 0; j < i; ++j) if (!strcmp(rule->inputs[i].alias, rule->inputs[j].alias)) return false;
    }
    if (!strcmp(rule->kind, "expression") && !valid_expression(rule->expression, rule)) return false;
    double unused;
    if (strcmp(rule->kind, "expression") && !bound_variable((void *)rule, rule->source_alias, &unused)) return false;
    for (pb_size_t i = 0; i < rule->unit_rules_count; ++i) if (!valid_expression(rule->unit_rules[i].condition, rule)) return false;
    return true;
}
static const char *point_id(const iot_edge_v1_ConfigItem *item, const uint8_t *device) {
#define POINT(member) return memcmp(item->item.member.device_id.bytes, device, 16) ? NULL : item->item.member.element_id
    switch (item->which_item) {
        case iot_edge_v1_ConfigItem_modbus_register_tag: POINT(modbus_register);
        case iot_edge_v1_ConfigItem_s7_area_tag: POINT(s7_area);
        case iot_edge_v1_ConfigItem_sl651_element_tag: POINT(sl651_element);
        case iot_edge_v1_ConfigItem_industrial_point_tag: POINT(industrial_point);
        case iot_edge_v1_ConfigItem_derived_point_tag:
            return memcmp(item->item.derived_point.device_id.bytes, device, 16) ? NULL : item->item.derived_point.point_id;
        default: return NULL;
    }
#undef POINT
}
bool edge_derived_validate_config(const edge_runtime_config *config) {
    for (uint32_t i = 0; i < config->item_count; ++i) {
        const iot_edge_v1_ConfigItem *item = &config->items[i];
        if (item->which_item != iot_edge_v1_ConfigItem_derived_point_tag) continue;
        const iot_edge_v1_DerivedPointConfig *rule = &item->item.derived_point;
        if (!edge_derived_validate(rule)) return false;
        unsigned count = 0;
        for (uint32_t j = 0; j < config->item_count; ++j) {
            const char *id = point_id(&config->items[j], rule->device_id.bytes);
            if (!id) continue;
            if (j != i && !strcmp(id, rule->point_id)) return false;
            if (config->items[j].which_item == iot_edge_v1_ConfigItem_derived_point_tag && ++count > 32) return false;
        }
        for (pb_size_t input = 0; input < rule->inputs_count; ++input) {
            bool found = false;
            for (uint32_t j = 0; j < config->item_count; ++j) {
                if (config->items[j].which_item == iot_edge_v1_ConfigItem_derived_point_tag && j >= i) continue;
                const char *id = point_id(&config->items[j], rule->device_id.bytes);
                if (id && !strcmp(id, rule->inputs[input].point_id)) found = true;
            }
            if (!found) return false;
        }
    }
    return true;
}
edge_derived *edge_derived_create(const edge_runtime_config *config, const uint8_t device_id[16]) {
    size_t count = 0;
    for (uint32_t i = 0; i < config->item_count; ++i) if (config->items[i].which_item == iot_edge_v1_ConfigItem_derived_point_tag && !memcmp(config->items[i].item.derived_point.device_id.bytes, device_id, 16)) ++count;
    if (!count) return NULL;
    if (count > 32) return NULL;
    edge_derived *state = calloc(1, sizeof(*state));
    if (!state) return NULL;
    state->rules = calloc(count, sizeof(*state->rules));
    state->capacity = count * 17;
    state->latest = calloc(state->capacity, sizeof(*state->latest));
    if (!state->rules || !state->latest) { edge_derived_free(state); return NULL; }
    for (uint32_t i = 0; i < config->item_count; ++i) {
        const iot_edge_v1_ConfigItem *item = &config->items[i];
        if (item->which_item != iot_edge_v1_ConfigItem_derived_point_tag || memcmp(item->item.derived_point.device_id.bytes, device_id, 16)) continue;
        if (!edge_derived_validate(&item->item.derived_point)) { edge_derived_free(state); return NULL; }
        derived_rule *rule = &state->rules[state->count++];
        rule->config = &item->item.derived_point;
        rule->output.derived = true; rule->output.hidden = rule->config->hidden;
        text(rule->output.element_id, sizeof(rule->output.element_id), rule->config->point_id);
        text(rule->output.name, sizeof(rule->output.name), rule->config->name);
        text(rule->output.quality, sizeof(rule->output.quality), "missing");
        for (pb_size_t j = 0; j < rule->config->inputs_count; ++j) (void)find(state, rule->config->inputs[j].point_id);
        (void)find(state, rule->config->point_id);
    }
    size_t windows = 0;
    for (size_t i = 0; i < state->count; ++i) if (strcmp(state->rules[i].config->kind, "expression")) ++windows;
    state->window_capacity = 4096 / (windows ? windows : 1);
    return state;
}
void edge_derived_free(edge_derived *state) {
    if (!state) return;
    for (size_t i = 0; i < state->count; ++i) free(state->rules[i].window);
    free(state->rules); free(state->latest); free(state);
}
size_t edge_derived_count(const edge_derived *state) { return state ? state->count : 0; }
void edge_derived_values(const edge_derived *state, iot_edge_v1_TelemetryValue *output) {
    for (size_t i = 0; state && i < state->count; ++i) output[i] = state->rules[i].output;
}
bool edge_derived_update(edge_derived *state, const iot_edge_v1_TelemetryValue *values, size_t count, int64_t at) {
    if (!state) return false;
    for (size_t i = 0; i < state->latest_count; ++i) state->latest[i].updated = false;
    for (size_t i = 0; i < count; ++i) for (size_t j = 0; j < state->latest_count; ++j) {
        latest_sample *sample = &state->latest[j];
        const int64_t observed = values[i].sample_time_ms ? values[i].sample_time_ms : at;
        if (strcmp(sample->id, values[i].element_id) || observed <= sample->at) continue;
        sample->at = observed; sample->valid = scalar(&values[i], &sample->value); sample->updated = true;
        text(sample->unit, sizeof(sample->unit), values[i].unit);
    }
    bool any = false;
    for (size_t i = 0; i < state->count; ++i) {
        derived_rule *rule = &state->rules[i];
        const iot_edge_v1_DerivedPointConfig *config = rule->config;
        latest_sample *output = find(state, config->point_id);
        bool changed = !output->at;
        latest_sample *source = NULL;
        evaluation context = {state, rule, at, "invalid"};
        bool valid = true; double value = 0;
        for (pb_size_t j = 0; j < config->inputs_count; ++j) {
            latest_sample *input = find(state, config->inputs[j].point_id);
            changed = changed || input->updated || (output->valid && input->at + (int64_t)config->max_age_seconds * 1000 <= at);
            if (!strcmp(config->source_alias, config->inputs[j].alias)) source = input;
            double unused;
            if (!variable(&context, config->inputs[j].alias, &unused)) valid = false;
        }
        if (strcmp(config->kind, "expression")) {
            if (source && source->updated && source->valid) {
                if (strcmp(rule->unit, source->unit)) { rule->count = 0; rule->overflow_until = 0; text(rule->unit, sizeof(rule->unit), source->unit); }
                while (rule->count && rule->window[0].at <= at - (int64_t)config->window_seconds * 1000) { --rule->count; memmove(rule->window, rule->window + 1, rule->count * sizeof(*rule->window)); }
                if (rule->count == state->window_capacity) { rule->overflow_until = rule->window[0].at + (int64_t)config->window_seconds * 1000; --rule->count; memmove(rule->window, rule->window + 1, rule->count * sizeof(*rule->window)); }
                window_sample *next = rule->window ? rule->window : malloc(state->window_capacity * sizeof(*rule->window));
                if (!next) { rule->overflow_until = at + (int64_t)config->window_seconds * 1000; }
                else { rule->window = next; rule->window[rule->count++] = (window_sample){source->at, source->value}; }
            }
            while (rule->count && rule->window[0].at <= at - (int64_t)config->window_seconds * 1000) { --rule->count; memmove(rule->window, rule->window + 1, rule->count * sizeof(*rule->window)); changed = true; }
        }
        if (!changed) continue;
        any = true;
        text(rule->output.unit, sizeof(rule->output.unit), config->unit);
        if (valid) {
            for (pb_size_t j = 0; j < config->unit_rules_count; ++j) {
                double condition;
                if (!edge_expression_evaluate(config->unit_rules[j].condition, variable, &context, &condition)) { valid = false; break; }
                if (condition != 0) { text(rule->output.unit, sizeof(rule->output.unit), config->unit_rules[j].unit); break; }
            }
        }
        if (valid && !strcmp(config->kind, "expression")) valid = edge_expression_evaluate(config->expression, variable, &context, &value);
        else if (valid) {
            if (!rule->count) { valid = false; context.error = "missing"; }
            else if (rule->overflow_until > at) { valid = false; context.error = "capacity_exceeded"; }
            else {
                value = rule->window[0].value;
                if (!strcmp(config->kind, "average")) {
                    long double mean = 0;
                    for (size_t j = 0; j < rule->count; ++j) mean += ((long double)rule->window[j].value - mean) / (j + 1);
                    value = (double)mean;
                } else for (size_t j = 0; j < rule->count; ++j) value = !strcmp(config->kind, "minimum") ? fmin(value, rule->window[j].value) : fmax(value, rule->window[j].value);
                valid = isfinite(value);
            }
        }
        output->valid = valid; output->at = at; output->updated = true;
        output->value = config->boolean_result ? value != 0 : value;
        text(rule->output.quality, sizeof(rule->output.quality), valid ? "good" : context.error);
        if (!valid && config->unit_rules_count) rule->output.unit[0] = 0;
        text(output->unit, sizeof(output->unit), rule->output.unit);
        rule->output.has_value = valid;
        rule->output.sample_time_ms = at;
        if (valid) {
            rule->output.value.kind = config->boolean_result ? iot_edge_v1_ValueKind_VALUE_BOOL : iot_edge_v1_ValueKind_VALUE_DOUBLE;
            rule->output.value.which_value = config->boolean_result ? iot_edge_v1_ScalarValue_bool_value_tag : iot_edge_v1_ScalarValue_double_value_tag;
            if (config->boolean_result) rule->output.value.value.bool_value = value != 0;
            else rule->output.value.value.double_value = value;
        }
    }
    return any;
}
int64_t edge_derived_deadline(const edge_derived *state, int64_t now) {
    int64_t next = INT64_MAX;
    for (size_t i = 0; state && i < state->count; ++i) {
        const derived_rule *rule = &state->rules[i];
        for (pb_size_t j = 0; j < rule->config->inputs_count; ++j) for (size_t k = 0; k < state->latest_count; ++k) {
            const latest_sample *sample = &state->latest[k];
            int64_t at = sample->at + (int64_t)rule->config->max_age_seconds * 1000;
            if (!strcmp(sample->id, rule->config->inputs[j].point_id) && sample->at && at > now && at < next) next = at;
        }
        if (rule->count) { int64_t at = rule->window[0].at + (int64_t)rule->config->window_seconds * 1000; if (at > now && at < next) next = at; }
    }
    return next;
}
