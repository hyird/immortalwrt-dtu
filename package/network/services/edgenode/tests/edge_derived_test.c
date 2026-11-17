#include "edge_derived.h"
#include "edge_protocol.h"
#include <pb_encode.h>
#include <pb_decode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static void check(bool ok, const char *reason) { if (!ok) { fprintf(stderr, "%s\n", reason); exit(1); } }
static bool variable(void *context, const char *name, double *value) { (void)context; (void)name; *value=12; return true; }
static iot_edge_v1_TelemetryValue sample(const char *id, double value, const char *unit) {
    iot_edge_v1_TelemetryValue out = iot_edge_v1_TelemetryValue_init_zero;
    snprintf(out.element_id,sizeof(out.element_id),"%s",id); snprintf(out.unit,sizeof(out.unit),"%s",unit);
    out.has_value=true; out.value.which_value=iot_edge_v1_ScalarValue_double_value_tag; out.value.value.double_value=value;
    return out;
}
static void test_external_io_dependencies(void) {
    iot_edge_v1_ConfigItem items[3] = {iot_edge_v1_ConfigItem_init_zero,
                                       iot_edge_v1_ConfigItem_init_zero,
                                       iot_edge_v1_ConfigItem_init_zero};
    items[0].which_item = iot_edge_v1_ConfigItem_external_io_tag;
    items[0].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_EXTERNAL_IO;
    items[0].item.external_io.device_id.size = 16;
    items[0].item.external_io.device_id.bytes[0] = 1;
    strcpy(items[0].item.external_io.point_id, "output");

    iot_edge_v1_DerivedPointInput inputs[2] = {iot_edge_v1_DerivedPointInput_init_zero,
                                              iot_edge_v1_DerivedPointInput_init_zero};
    strcpy(inputs[0].alias, "s");
    strcpy(inputs[0].point_id, "output");
    strcpy(inputs[1].alias, "x");
    strcpy(inputs[1].point_id, "numeric_output");
    for (size_t index = 1; index < 3; ++index) {
        items[index].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
        items[index].kind = iot_edge_v1_ConfigItemKind_CONFIG_ITEM_DERIVED_POINT;
        iot_edge_v1_DerivedPointConfig *rule = &items[index].item.derived_point;
        rule->device_id.size = 16;
        rule->device_id.bytes[0] = 1;
        strcpy(rule->name, index == 1 ? "numeric output" : "chained output");
        strcpy(rule->point_id, index == 1 ? "numeric_output" : "chained_output");
        strcpy(rule->kind, "expression");
        strcpy(rule->expression, index == 1 ? "if(s, 1, 0)" : "x * 2");
        rule->inputs = &inputs[index - 1];
        rule->inputs_count = 1;
        rule->max_age_seconds = 30;
    }
    edge_runtime_config config = {0};
    config.items = items;
    config.item_count = 3;
    check(edge_derived_validate_config(&config), "external IO derived chain rejected");

    items[0].item.external_io.device_id.bytes[0] = 2;
    check(!edge_derived_validate_config(&config), "external IO from another device accepted");
    items[0].item.external_io.device_id.bytes[0] = 1;
    strcpy(items[0].item.external_io.point_id, "numeric_output");
    check(!edge_derived_validate_config(&config), "duplicate external IO and derived ID accepted");
    strcpy(items[0].item.external_io.point_id, "output");
    iot_edge_v1_ConfigItem swap = items[1];
    items[1] = items[2];
    items[2] = swap;
    check(!edge_derived_validate_config(&config), "unordered derived dependency accepted");
    swap = items[1];
    items[1] = items[2];
    items[2] = swap;

    edge_derived *state = edge_derived_create(&config, items[1].item.derived_point.device_id.bytes);
    check(state != NULL, "external IO derived state creation failed");
    iot_edge_v1_TelemetryValue value = sample("output", 0, "");
    value.value.kind = iot_edge_v1_ValueKind_VALUE_BOOL;
    value.value.which_value = iot_edge_v1_ScalarValue_bool_value_tag;
    value.value.value.bool_value = true;
    iot_edge_v1_TelemetryValue result[2];
    check(edge_derived_update(state, &value, 1, 1000), "external IO true sample ignored");
    edge_derived_values(state, result);
    check(result[0].has_value && result[0].value.value.double_value == 1 &&
          result[1].has_value && result[1].value.value.double_value == 2,
          "external IO true chain failed");
    value.value.value.bool_value = false;
    check(edge_derived_update(state, &value, 1, 2000), "external IO false sample ignored");
    edge_derived_values(state, result);
    check(result[0].has_value && result[0].value.value.double_value == 0 &&
          result[1].has_value && result[1].value.value.double_value == 0,
          "external IO false chain failed");
    edge_derived_free(state);
}
static void test_typed_outputs(void) {
    iot_edge_v1_ConfigItem items[3] = {0};
    iot_edge_v1_DerivedPointInput inputs[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        items[i + 1].which_item = iot_edge_v1_ConfigItem_derived_point_tag;
        iot_edge_v1_DerivedPointConfig *rule = &items[i + 1].item.derived_point;
        rule->device_id.size = 16;
        strcpy(rule->point_id, i ? "chain" : "typed");
        strcpy(rule->name, rule->point_id);
        strcpy(rule->kind, "expression");
        strcpy(rule->expression, "x");
        rule->max_age_seconds = 60;
        rule->inputs_count = 1;
        rule->inputs = &inputs[i];
        strcpy(inputs[i].alias, "x");
        strcpy(inputs[i].point_id, i ? "typed" : "raw");
    }
    items[0].which_item = iot_edge_v1_ConfigItem_modbus_register_tag;
    items[0].item.modbus_register.device_id.size = 16;
    strcpy(items[0].item.modbus_register.element_id, "raw");
    edge_runtime_config config = {.items = items, .item_count = 3};
    iot_edge_v1_DerivedPointConfig *rule = &items[1].item.derived_point;
    const struct {
        const char *type, *rounding;
        double input, expected;
        bool valid;
    } cases[] = {
        {"", "", 1.5, 1.5, true},
        {"BOOL", "", -0.1, 1, true},
        {"INT16", "", 1.5, 2, true}, {"INT16", "round", -1.5, -2, true},
        {"INT16", "truncate", 1.9, 1, true}, {"INT16", "truncate", -1.9, -1, true},
        {"INT16", "round", 32767.5, 0, false}, {"INT16", "", -32768.5, 0, false},
        {"INT16", "truncate", 32767.9, 32767, true},
        {"UINT16", "", -0.4, 0, true}, {"UINT16", "", -0.5, 0, false},
        {"UINT16", "", 65535, 65535, true}, {"UINT16", "", 65535.5, 0, false},
        {"INT8", "", 127.4, 127, true}, {"INT8", "", 127.5, 0, false},
        {"INT8", "", -128, -128, true}, {"INT8", "", -128.5, 0, false},
        {"UINT8", "", 255, 255, true}, {"UINT8", "", 255.5, 0, false},
        {"UINT8", "truncate", -0.9, 0, true},
        {"INT32", "", -2147483648.0, -2147483648.0, true},
        {"INT32", "", 2147483648.0, 0, false},
        {"UINT32", "", 4294967295.0, 4294967295.0, true},
        {"UINT32", "", 4294967296.0, 0, false},
        {"INT64", "", -0x1p63, -0x1p63, true},
        {"INT64", "", 0x1p63, 0, false},
        {"INT64", "", 0x1.fffffffffffffp62, 0x1.fffffffffffffp62, true},
        {"INT64", "", -0x1.0000000000001p63, 0, false},
        {"UINT64", "", 0x1p64, 0, false},
        {"UINT64", "", 0x1.fffffffffffffp63, 0x1.fffffffffffffp63, true},
        {"UINT64", "", -1, 0, false},
        {"FLOAT32", "", 16777217, 16777216, true},
        {"FLOAT32", "", 0.1, (double)(float)0.1, true},
        {"FLOAT32", "", 1e39, 0, false},
        {"FLOAT32", "", 0x1.fffffe8p127, 0x1.fffffep127, true},
        {"FLOAT32", "", -0x1.fffffe8p127, -0x1.fffffep127, true},
        {"FLOAT32", "", 0x1.ffffffp127, 0, false},
        {"FLOAT32", "", -0x1.ffffffp127, 0, false},
        {"FLOAT64", "", 16777217, 16777217, true}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        strcpy(rule->data_type, cases[i].type);
        strcpy(rule->rounding, cases[i].rounding);
        rule->boolean_result = !strcmp(cases[i].type, "BOOL");
        check(edge_derived_validate_config(&config), "typed configuration rejected");
        edge_derived *state = edge_derived_create(&config, rule->device_id.bytes);
        check(state != NULL, "typed state creation");
        iot_edge_v1_TelemetryValue input = sample("raw", cases[i].input, ""), out[2];
        check(edge_derived_update(state, &input, 1, 1000), "typed update");
        edge_derived_values(state, out);
        check(out[0].has_value == cases[i].valid && out[1].has_value == cases[i].valid,
              "typed validity or chain validity");
        if (!cases[i].valid) {
            check(!strcmp(out[0].quality, "overflow"), "typed overflow quality");
        } else {
            check(out[1].value.value.double_value == cases[i].expected, "chain used unconverted value");
            if (!strncmp(cases[i].type, "INT", 3)) {
                check(out[0].value.kind == iot_edge_v1_ValueKind_VALUE_SIGNED &&
                      out[0].value.which_value == iot_edge_v1_ScalarValue_signed_value_tag &&
                      out[0].value.value.signed_value == (int64_t)cases[i].expected, "signed scalar");
            } else if (!strncmp(cases[i].type, "UINT", 4)) {
                check(out[0].value.kind == iot_edge_v1_ValueKind_VALUE_UNSIGNED &&
                      out[0].value.which_value == iot_edge_v1_ScalarValue_unsigned_value_tag &&
                      out[0].value.value.unsigned_value == (uint64_t)cases[i].expected, "unsigned scalar");
            } else if (rule->boolean_result) {
                check(out[0].value.kind == iot_edge_v1_ValueKind_VALUE_BOOL &&
                      out[0].value.value.bool_value, "boolean scalar");
            } else check(out[0].value.which_value == iot_edge_v1_ScalarValue_double_value_tag &&
                         out[0].value.value.double_value == cases[i].expected, "floating scalar");
        }
        edge_derived_free(state);
    }
    strcpy(rule->data_type, "INT16");
    strcpy(rule->rounding, "round");
    strcpy(rule->kind, "average");
    strcpy(rule->source_alias, "x");
    rule->window_seconds = 10;
    edge_derived *state = edge_derived_create(&config, rule->device_id.bytes);
    check(state != NULL, "typed window create");
    iot_edge_v1_TelemetryValue value = sample("raw", -1.5, ""), out[2];
    edge_derived_update(state, &value, 1, 1000);
    edge_derived_values(state, out);
    check(out[0].has_value && out[0].value.value.signed_value == -2 &&
          out[1].value.value.double_value == -2, "window conversion and chain");
    edge_derived_free(state);
    strcpy(rule->data_type, "FLOAT64");
    rule->rounding[0] = 0;
    strcpy(rule->kind, "expression");
    strcpy(rule->expression, "x * x");
    state = edge_derived_create(&config, rule->device_id.bytes);
    check(state != NULL, "overflow expression create");
    value = sample("raw", 1e300, "");
    edge_derived_update(state, &value, 1, 1000);
    edge_derived_values(state, out);
    check(!out[0].has_value && !strcmp(out[0].quality, "overflow"), "arithmetic overflow quality");
    edge_derived_free(state);
    strcpy(rule->expression, "x");
    rule->rounding[0] = rule->data_type[0] = 0;
    rule->boolean_result = true;
    state = edge_derived_create(&config, rule->device_id.bytes);
    check(state != NULL, "legacy boolean create");
    value = sample("raw", -0.5, "");
    edge_derived_update(state, &value, 1, 1000);
    edge_derived_values(state, out);
    check(out[0].has_value && out[0].value.which_value == iot_edge_v1_ScalarValue_bool_value_tag &&
          out[0].value.value.bool_value && out[1].value.value.double_value == 1,
          "legacy boolean output changed");
    edge_derived_free(state);
    rule->boolean_result = false;
    strcpy(rule->rounding, "round");
    strcpy(rule->kind, "expression");
    const char *invalid[] = {"DOUBLE", "float32", "UNKNOWN"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        strcpy(rule->data_type, invalid[i]);
        check(!edge_derived_validate(rule), "unknown output type accepted");
    }
    strcpy(rule->data_type, "FLOAT64");
    check(!edge_derived_validate(rule), "noninteger rounding accepted");
    strcpy(rule->data_type, "INT16");
    strcpy(rule->rounding, "floor");
    check(!edge_derived_validate(rule), "unknown rounding accepted");
    rule->rounding[0] = 0;
    rule->boolean_result = true;
    check(!edge_derived_validate(rule), "numeric boolean mismatch accepted");
    strcpy(rule->data_type, "BOOL");
    rule->boolean_result = false;
    check(!edge_derived_validate(rule), "boolean numeric mismatch accepted");
}

int main(void) {
    test_typed_outputs();
    test_external_io_dependencies();
    double value;
    check(edge_expression_evaluate("if(x > 10,\n x*2, 1/0)",variable,NULL,&value) && value==24,"short circuit");
    check(!edge_expression_evaluate("1/0",variable,NULL,&value),"division by zero");
    check(!edge_expression_evaluate("0xff",variable,NULL,&value),"hex unsupported");
    iot_edge_v1_ConfigItem items[3]={iot_edge_v1_ConfigItem_init_zero,iot_edge_v1_ConfigItem_init_zero,iot_edge_v1_ConfigItem_init_zero};
    for (unsigned i=0;i<2;++i) { items[i].which_item=iot_edge_v1_ConfigItem_modbus_register_tag; items[i].item.modbus_register.device_id.size=16; }
    strcpy(items[0].item.modbus_register.element_id,"pressure"); strcpy(items[1].item.modbus_register.element_id,"mode");
    items[2].which_item=iot_edge_v1_ConfigItem_derived_point_tag;
    iot_edge_v1_DerivedPointConfig *rule=&items[2].item.derived_point;
    rule->device_id.size=16; strcpy(rule->point_id,"derived"); strcpy(rule->name,"pressure"); strcpy(rule->unit,"kPa");
    strcpy(rule->kind,"expression"); strcpy(rule->expression,"if(mode == 1, p / 1000, p)"); rule->max_age_seconds=5;
    iot_edge_v1_DerivedPointInput inputs[2]={iot_edge_v1_DerivedPointInput_init_zero,iot_edge_v1_DerivedPointInput_init_zero};
    strcpy(inputs[0].alias,"p"); strcpy(inputs[0].point_id,"pressure"); strcpy(inputs[1].alias,"mode"); strcpy(inputs[1].point_id,"mode");
    rule->inputs=inputs; rule->inputs_count=2;
    iot_edge_v1_DerivedUnitRule unit=iot_edge_v1_DerivedUnitRule_init_zero; strcpy(unit.condition,"mode == 1"); strcpy(unit.unit,"MPa"); rule->unit_rules=&unit; rule->unit_rules_count=1;
    edge_runtime_config config={0}; config.items=items; config.item_count=3;
    check(edge_derived_validate_config(&config),"valid config rejected");
    uint8_t wire[EDGENODE_MAX_WS_MESSAGE];
    pb_ostream_t encoded = pb_ostream_from_buffer(wire, sizeof(wire));
    check(pb_encode(&encoded, iot_edge_v1_ConfigItem_fields, &items[2]), "derived config encoding");
    iot_edge_v1_ConfigItem decoded = iot_edge_v1_ConfigItem_init_zero;
    pb_istream_t input = pb_istream_from_buffer(wire, encoded.bytes_written);
    check(pb_decode(&input, iot_edge_v1_ConfigItem_fields, &decoded), "derived config decoding");
    check(edge_derived_validate(&decoded.item.derived_point) && decoded.item.derived_point.inputs_count == 2 && decoded.item.derived_point.unit_rules_count == 1, "dynamic config fields lost");
    pb_release(iot_edge_v1_ConfigItem_fields, &decoded);

    strcpy(rule->expression,"if(true, p, missing)"); check(!edge_derived_validate(rule),"hidden unbound variable accepted");
    strcpy(rule->expression,"if(mode == 1, p / 1000, p)");
    strcpy(inputs[0].point_id,"derived"); check(!edge_derived_validate_config(&config),"self cycle accepted"); strcpy(inputs[0].point_id,"pressure");
    edge_derived *state=edge_derived_create(&config,rule->device_id.bytes); check(state!=NULL,"create");
    iot_edge_v1_TelemetryValue values[2]={sample("pressure",2500,"kPa"),sample("mode",0,"")}, out;
    check(edge_derived_update(state,values,2,1000),"initial"); edge_derived_values(state,&out);
    check(out.has_value && out.value.value.double_value==2500 && !strcmp(out.unit,"kPa"),"initial snapshot");
    values[1]=sample("mode",1,""); edge_derived_update(state,values+1,1,2000); edge_derived_values(state,&out);
    check(out.value.value.double_value==2.5 && !strcmp(out.unit,"MPa"),"unit dependency");
    check(!edge_derived_update(state,values,2,1000),"replay changed state");
    check(edge_derived_deadline(state,2000)==6000,"stale deadline");
    edge_derived_update(state,NULL,0,6000); edge_derived_values(state,&out);
    check(!out.has_value && !*out.unit && !strcmp(out.quality,"stale"),"stale quality"); edge_derived_free(state);
    const char *kinds[]={"average","minimum","maximum"};
    for (unsigned i=0;i<3;++i) {
        strcpy(rule->kind,kinds[i]); strcpy(rule->source_alias,"p"); rule->inputs_count=1; rule->unit_rules_count=0; rule->window_seconds=10; rule->max_age_seconds=30;
        state=edge_derived_create(&config,rule->device_id.bytes); check(state!=NULL,"window create");
        values[0]=sample("pressure",10,"kPa"); edge_derived_update(state,values,1,1000); edge_derived_update(state,values,1,2000);
        values[0]=sample("pressure",40,"kPa"); edge_derived_update(state,values,1,3000); edge_derived_values(state,&out);
        check(out.has_value && out.value.value.double_value==(i==0?20:i==1?10:40),"window result");
        edge_derived_update(state,NULL,0,11000); edge_derived_values(state,&out);
        check(out.value.value.double_value==(i==0?25:i==1?10:40),"window expiration");
        values[0]=sample("pressure",2,"MPa"); edge_derived_update(state,values,1,12000); edge_derived_values(state,&out);
        check(out.value.value.double_value==2,"unit reset"); edge_derived_free(state);
    }
    puts("derived calculations passed"); return 0;
}
