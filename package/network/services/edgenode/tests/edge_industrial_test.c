#include "edge_industrial.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); exit(1); } } while(0)

static iot_edge_v1_DeviceConfig device(iot_edge_v1_Protocol protocol) {
    iot_edge_v1_DeviceConfig d=iot_edge_v1_DeviceConfig_init_zero;
    d.protocol=protocol;d.has_industrial=true;strcpy(d.device_code,"000000123456");
    d.industrial.mc_station=255;d.industrial.mc_module_io=1023;d.industrial.mc_monitoring_timer=16;
    d.industrial.dlt645_version=2007;d.industrial.dlt645_wakeup_bytes=4;
    return d;
}
static iot_edge_v1_IndustrialPointConfig point(void) {
    iot_edge_v1_IndustrialPointConfig p=iot_edge_v1_IndustrialPointConfig_init_zero;
    strcpy(p.element_id,"value");strcpy(p.area,"D");strcpy(p.data_type,"UINT16");
    strcpy(p.byte_order,"BIG_ENDIAN");p.scale=1;p.decimals=-1;p.address=100;p.writable=true;
    return p;
}
static void test_mc(void) {
    iot_edge_v1_DeviceConfig d=device(iot_edge_v1_Protocol_PROTOCOL_MC);
    iot_edge_v1_IndustrialPointConfig p=point();uint8_t q[128],out[8];size_t size=0;bool more=false;
    const uint8_t golden[]={0x50,0,0,0xff,0xff,3,0,12,0,16,0,1,4,0,0,100,0,0,0xa8,1,0};
    size_t n=edge_industrial_request(&d,&d.industrial,&p,123,0,NULL,0,q,sizeof(q));
    CHECK(n==sizeof(golden) && !memcmp(q,golden,n));
    uint8_t r[]={0xd0,0,0,0xff,0xff,3,0,4,0,0,0,0x34,0x12};
    CHECK(edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));
    CHECK(size==2 && out[0]==0x34 && out[1]==0x12 && !more);
    r[2]=1;CHECK(!edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));
    d.industrial.mc_four_e=true;
    n=edge_industrial_request(&d,&d.industrial,&p,123,0,NULL,0,q,sizeof(q));
    CHECK(n==25 && q[0]==0x54 && q[2]==123);
    uint8_t four[]={0xd4,0,123,0,0,0,0,0xff,0xff,3,0,4,0,0,0,0x34,0x12};
    CHECK(edge_industrial_response(&d,q,n,four,sizeof(four),out,sizeof(out),&size,&more));
    four[2]=124;CHECK(!edge_industrial_response(&d,q,n,four,sizeof(four),out,sizeof(out),&size,&more));
    strcpy(p.area,"M");strcpy(p.data_type,"BOOL");const uint8_t bit=1;
    n=edge_industrial_request(&d,&d.industrial,&p,1,0,&bit,1,q,sizeof(q));CHECK(n==26 && q[n-1]==0x10);
}
static void test_fins(void) {
    iot_edge_v1_DeviceConfig d=device(iot_edge_v1_Protocol_PROTOCOL_FINS);
    iot_edge_v1_IndustrialPointConfig p=point();uint8_t q[128],out[8];size_t size=0;bool more=false;
    CHECK(edge_fins_node_request(0,q,sizeof(q))==20 && q[7]==12);
    uint8_t nodes[24]={'F','I','N','S',0,0,0,16,0,0,0,1,0,0,0,0,0,0,0,10,0,0,0,20};
    CHECK(edge_fins_node_response(nodes,sizeof(nodes),&d.industrial));
    CHECK(d.industrial.fins_source_node==10 && d.industrial.fins_destination_node==20);
    size_t n=edge_industrial_request(&d,&d.industrial,&p,7,0,NULL,0,q,sizeof(q));
    const uint8_t golden[]={'F','I','N','S',0,0,0,26,0,0,0,2,0,0,0,0,0x80,0,2,0,20,0,0,10,0,7,1,1,0x82,0,100,0,0,1};
    CHECK(n==sizeof(golden) && !memcmp(q,golden,n));
    uint8_t r[]={'F','I','N','S',0,0,0,24,0,0,0,2,0,0,0,0,0xc0,0,2,0,10,0,0,20,0,7,1,1,0,0,0x12,0x34};
    CHECK(edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));CHECK(size==2 && out[0]==0x12);
    r[25]=8;CHECK(!edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));
}
static void checksum(uint8_t *p,size_t n) {uint8_t sum=0;for(size_t i=0;i<n-2;++i)sum=(uint8_t)(sum+p[i]);p[n-2]=sum;}
static void test_meter(void) {
    iot_edge_v1_DeviceConfig d=device(iot_edge_v1_Protocol_PROTOCOL_DLT645);
    iot_edge_v1_IndustrialPointConfig p=point();strcpy(p.data_type,"BCD");strcpy(p.identifier,"00010000");p.length=4;p.digits=2;p.writable=false;
    uint8_t q[128],out[8];size_t size=0;bool more=false;
    size_t n=edge_industrial_request(&d,&d.industrial,&p,0,0,NULL,0,q,sizeof(q));
    const uint8_t golden[]={0xfe,0xfe,0xfe,0xfe,0x68,0x56,0x34,0x12,0,0,0,0x68,0x11,4,0x33,0x33,0x34,0x33,0x4e,0x16};
    CHECK(n==sizeof(golden) && !memcmp(q,golden,n));
    uint8_t r[]={0x68,0x56,0x34,0x12,0,0,0,0x68,0x91,8,0x33,0x33,0x34,0x33,0xab,0x89,0x67,0x45,0,0x16};checksum(r,sizeof(r));
    CHECK(edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));
    char text[64];CHECK(edge_meter_decode(&p,out,size,text,sizeof(text)));CHECK(!strcmp(text,"123456.78"));
    CHECK(edge_meter_encode(&p,text,out,sizeof(out),&size));CHECK(out[0]==0x78 && out[3]==0x12);
    CHECK(!edge_meter_encode(&p,"1.234",out,sizeof(out),&size));CHECK(!edge_meter_encode(&p,"1000000",out,sizeof(out),&size));
    r[18]^=1;CHECK(!edge_industrial_response(&d,q,n,r,sizeof(r),out,sizeof(out),&size,&more));
    d.industrial.dlt645_version=1997;strcpy(p.identifier,"9010");
    n=edge_industrial_request(&d,&d.industrial,&p,0,0,NULL,0,q,sizeof(q));CHECK(n==18 && q[12]==1 && q[14]==0x43 && q[15]==0xc3);
    d.industrial.dlt645_write_password.size=4;p.writable=true;
    CHECK(edge_meter_encode(&p,"13.25",out,sizeof(out),&size));
    n=edge_industrial_request(&d,&d.industrial,&p,0,0,out,size,q,sizeof(q));CHECK(n==26 && q[12]==4);
    edge_industrial_redact(d.protocol,q,n);CHECK(q[16]==0 && q[19]==0);
}
int main(void) {test_mc();test_fins();test_meter();puts("industrial protocol vectors passed");return 0;}
