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
int main(void) {
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
