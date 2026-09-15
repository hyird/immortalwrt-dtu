#include "edge_industrial.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static void put_le16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)n; p[1]=(uint8_t)(n>>8); }
static void put_be16(uint8_t *p, uint16_t n) { p[0]=(uint8_t)(n>>8); p[1]=(uint8_t)n; }
static void put_be32(uint8_t *p, uint32_t n) {
    p[0]=(uint8_t)(n>>24); p[1]=(uint8_t)(n>>16); p[2]=(uint8_t)(n>>8); p[3]=(uint8_t)n;
}
static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c-'0';
    if (c >= 'a' && c <= 'f') return c-'a'+10;
    if (c >= 'A' && c <= 'F') return c-'A'+10;
    return -1;
}
static bool hex_bytes(const char *text, uint8_t *bytes, size_t size) {
    if (strlen(text) != size*2) return false;
    for (size_t i=0; i<size; ++i) {
        int a=hex_digit(text[2*i]), b=hex_digit(text[2*i+1]);
        if (a<0 || b<0) return false;
        bytes[i]=(uint8_t)(a*16+b);
    }
    return true;
}
static uint8_t mc_area(const char *name, bool *bit) {
    static const struct { const char *name; uint8_t code; bool bit; } areas[]={
        {"D",0xa8,false},{"W",0xb4,false},{"R",0xaf,false},{"ZR",0xb0,false},
        {"M",0x90,true},{"X",0x9c,true},{"Y",0x9d,true},{"B",0xa0,true},
        {"L",0x92,true},{"F",0x93,true},{"V",0x94,true},{"S",0x98,true},
        {"TN",0xc2,false},{"CN",0xc5,false},{"TS",0xc1,true},{"CS",0xc4,true}};
    for (size_t i=0;i<sizeof(areas)/sizeof(areas[0]);++i)
        if (!strcmp(name,areas[i].name)) { *bit=areas[i].bit; return areas[i].code; }
    return 0;
}
static uint8_t fins_area(const char *name, bool bit) {
    if (!strcmp(name,"D")) return bit?0x02:0x82;
    if (!strcmp(name,"CIO")) return bit?0x30:0xb0;
    if (!strcmp(name,"W")) return bit?0x31:0xb1;
    if (!strcmp(name,"H")) return bit?0x32:0xb2;
    if (!strcmp(name,"A")) return bit?0x33:0xb3;
    return 0;
}
bool edge_industrial_protocol(iot_edge_v1_Protocol protocol) {
    return protocol==iot_edge_v1_Protocol_PROTOCOL_MC || protocol==iot_edge_v1_Protocol_PROTOCOL_FINS ||
           protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645;
}
size_t edge_industrial_width(const iot_edge_v1_IndustrialPointConfig *p) {
    if (!strcmp(p->data_type,"BOOL")) return 1;
    if (!strcmp(p->data_type,"INT16") || !strcmp(p->data_type,"UINT16")) return 2;
    if (!strcmp(p->data_type,"INT32") || !strcmp(p->data_type,"UINT32") || !strcmp(p->data_type,"FLOAT32")) return 4;
    if (!strcmp(p->data_type,"INT64") || !strcmp(p->data_type,"UINT64") || !strcmp(p->data_type,"DOUBLE")) return 8;
    if (!strcmp(p->data_type,"BCD") || !strcmp(p->data_type,"BCD_SIGNED") || !strcmp(p->data_type,"HEX")) return p->length;
    return 0;
}
bool edge_industrial_valid_device(const iot_edge_v1_DeviceConfig *d) {
    if (!d->has_industrial) return false;
    const iot_edge_v1_IndustrialConnectionConfig *c=&d->industrial;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC)
        return c->mc_network<=255 && c->mc_station<=255 && c->mc_module_io<=65535 &&
               c->mc_multidrop<=255 && c->mc_monitoring_timer>0 && c->mc_monitoring_timer<=65535;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS)
        return c->fins_source_network<=127 && c->fins_destination_network<=127 &&
               c->fins_source_node<=254 && c->fins_destination_node<=254 &&
               c->fins_source_unit<=255 && c->fins_destination_unit<=255;
    if (d->protocol!=iot_edge_v1_Protocol_PROTOCOL_DLT645 ||
        (c->dlt645_version!=1997 && c->dlt645_version!=2007) || c->dlt645_wakeup_bytes>4 ||
        (c->dlt645_write_password.size!=0 && c->dlt645_write_password.size!=4) ||
        (c->dlt645_operator_code.size!=0 && c->dlt645_operator_code.size!=4) || strlen(d->device_code)!=12) return false;
    for (size_t i=0;i<12;++i) if (d->device_code[i]<'0' || d->device_code[i]>'9') return false;
    return true;
}
bool edge_industrial_valid_point(const iot_edge_v1_DeviceConfig *d, const iot_edge_v1_IndustrialPointConfig *p) {
    size_t width=edge_industrial_width(p);
    if (!p->element_id[0] || !width || !isfinite(p->scale) || p->decimals < -1 || p->decimals>8) return false;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_DLT645) {
        uint8_t id[4];
        bool hex=!strcmp(p->data_type,"HEX");
        if ((!hex && strcmp(p->data_type,"BCD") && strcmp(p->data_type,"BCD_SIGNED")) ||
            p->length>(hex?200U:8U) || p->digits>8 ||
            !hex_bytes(p->identifier,id,d->industrial.dlt645_version==2007?4:2)) return false;
        return !p->writable || (d->industrial.dlt645_write_password.size==4 &&
            (d->industrial.dlt645_version==1997 || d->industrial.dlt645_operator_code.size==4) &&
            p->length<=(d->industrial.dlt645_version==2007?38U:44U));
    }
    if (strcmp(p->byte_order,"BIG_ENDIAN") && strcmp(p->byte_order,"LITTLE_ENDIAN") &&
        strcmp(p->byte_order,"BIG_ENDIAN_BYTE_SWAP") && strcmp(p->byte_order,"LITTLE_ENDIAN_BYTE_SWAP")) return false;
    bool bit=!strcmp(p->data_type,"BOOL"), area_bit=false;
    if (width>8 || !strcmp(p->data_type,"BCD") || !strcmp(p->data_type,"BCD_SIGNED") || !strcmp(p->data_type,"HEX")) return false;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC) {
        if (!mc_area(p->area,&area_bit) || (bit&&!area_bit)) return false;
        size_t count=bit?1:width/2;
        size_t range=(!bit&&area_bit)?count*16:count;
        return p->address<=0xffffff && range<=0x1000000U-p->address;
    }
    return d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS && fins_area(p->area,bit) &&
           p->address<=65535 && p->bit<=15 && (bit || p->bit==0) && (bit || width/2<=65536U-p->address);
}
size_t edge_industrial_header_size(const iot_edge_v1_DeviceConfig *d) {
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC) return d->industrial.mc_four_e?13:9;
    return d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS?8:10;
}
size_t edge_industrial_frame_size(const iot_edge_v1_DeviceConfig *d,const uint8_t *p,size_t n) {
    size_t h=edge_industrial_header_size(d);
    if (n<h) return 0;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC)
        return p[0]==(d->industrial.mc_four_e?0xd4:0xd0) && p[1]==0 && le16(p+h-2)>=2 ? h+le16(p+h-2):0;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS)
        return !memcmp(p,"FINS",4) && be32(p+4)>=8 && be32(p+4)<=4088 ? 8+be32(p+4):0;
    return p[0]==0x68 && p[7]==0x68 && p[9]<=200 ? 12U+p[9]:0;
}
size_t edge_fins_node_request(uint8_t source,uint8_t *p,size_t capacity) {
    if (capacity<20 || source>254) return 0;
    memset(p,0,20); memcpy(p,"FINS",4); put_be32(p+4,12); put_be32(p+16,source); return 20;
}
bool edge_fins_node_response(const uint8_t *p,size_t n,iot_edge_v1_IndustrialConnectionConfig *c) {
    if (n!=24 || memcmp(p,"FINS",4) || be32(p+4)!=16 || be32(p+8)!=1 || be32(p+12)!=0) return false;
    uint32_t source=be32(p+16),destination=be32(p+20);
    if (!source || source>254 || !destination || destination>254 || (c->fins_source_node && c->fins_source_node!=source)) return false;
    c->fins_source_node=source;
    if (!c->fins_destination_node) c->fins_destination_node=destination;
    return true;
}
size_t edge_industrial_request(const iot_edge_v1_DeviceConfig *d,const iot_edge_v1_IndustrialConnectionConfig *c,
    const iot_edge_v1_IndustrialPointConfig *p,uint16_t serial,uint8_t sequence,
    const uint8_t *value,size_t value_size,uint8_t *out,size_t capacity) {
    if (!edge_industrial_valid_device(d) || !edge_industrial_valid_point(d,p)) return 0;
    size_t width=edge_industrial_width(p);
    bool write=value!=NULL, bit=!strcmp(p->data_type,"BOOL"), ignored=false;
    if (write && (!p->writable || width!=value_size || (bit && value[0]>1))) return 0;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC) {
        size_t h=c->mc_four_e?13:9, payload=write?value_size:0, n=h+12+payload;
        if (capacity<n) return 0;
        memset(out,0,n); out[0]=c->mc_four_e?0x54:0x50;
        if (c->mc_four_e) put_le16(out+2,serial);
        size_t route=h-7;
        out[route]=(uint8_t)c->mc_network; out[route+1]=(uint8_t)c->mc_station;
        put_le16(out+route+2,(uint16_t)c->mc_module_io); out[route+4]=(uint8_t)c->mc_multidrop;
        put_le16(out+h-2,(uint16_t)(12+payload)); put_le16(out+h,(uint16_t)c->mc_monitoring_timer);
        put_le16(out+h+2,write?0x1401:0x0401); put_le16(out+h+4,bit?1:0);
        out[h+6]=(uint8_t)p->address;out[h+7]=(uint8_t)(p->address>>8);out[h+8]=(uint8_t)(p->address>>16);
        out[h+9]=mc_area(p->area,&ignored); put_le16(out+h+10,(uint16_t)(bit?1:width/2));
        if (write) { memcpy(out+h+12,value,value_size); if (bit) out[h+12]=(uint8_t)(value[0]<<4); }
        return n;
    }
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS) {
        size_t n=34+(write?value_size:0);
        if (capacity<n || !c->fins_source_node || !c->fins_destination_node) return 0;
        memset(out,0,n);memcpy(out,"FINS",4);put_be32(out+4,(uint32_t)(n-8));put_be32(out+8,2);
        out[16]=0x80;out[18]=2;out[19]=(uint8_t)c->fins_destination_network;out[20]=(uint8_t)c->fins_destination_node;
        out[21]=(uint8_t)c->fins_destination_unit;out[22]=(uint8_t)c->fins_source_network;out[23]=(uint8_t)c->fins_source_node;
        out[24]=(uint8_t)c->fins_source_unit;out[25]=(uint8_t)serial;out[26]=1;out[27]=write?2:1;
        out[28]=fins_area(p->area,bit);put_be16(out+29,(uint16_t)p->address);out[31]=(uint8_t)p->bit;
        put_be16(out+32,(uint16_t)(bit?1:width/2));if (write) memcpy(out+34,value,value_size);return n;
    }
    size_t wake=c->dlt645_wakeup_bytes, id_size=c->dlt645_version==2007?4:2;
    size_t auth=write?(c->dlt645_version==2007?8:4):0;
    size_t data_size=id_size+auth+(write?value_size:sequence?1:0), n=wake+12+data_size;
    if (capacity<n || data_size>200) return 0;
    memset(out,0xfe,wake);uint8_t *f=out+wake;f[0]=0x68;
    for (size_t i=0;i<6;++i) f[i+1]=(uint8_t)((d->device_code[10-i*2]-'0')*16+d->device_code[11-i*2]-'0');
    f[7]=0x68;f[8]=(uint8_t)(write?(c->dlt645_version==2007?0x14:0x04):(c->dlt645_version==2007?0x11:0x01)+(sequence?1:0));
    f[9]=(uint8_t)data_size;uint8_t id[4];if (!hex_bytes(p->identifier,id,id_size)) return 0;
    for (size_t i=0;i<id_size;++i) f[10+i]=id[id_size-1-i];
    if (write) {
        memcpy(f+10+id_size,c->dlt645_write_password.bytes,4);
        if (auth==8) memcpy(f+14+id_size,c->dlt645_operator_code.bytes,4);
        memcpy(f+10+id_size+auth,value,value_size);
    } else if (sequence) f[10+id_size]=sequence;
    for (size_t i=0;i<data_size;++i) f[10+i]=(uint8_t)(f[10+i]+0x33);
    uint8_t sum=0;for (size_t i=0;i<10+data_size;++i) sum=(uint8_t)(sum+f[i]);
    f[10+data_size]=sum;f[11+data_size]=0x16;return n;
}
bool edge_industrial_response(const iot_edge_v1_DeviceConfig *d,const uint8_t *q,size_t qn,
    const uint8_t *f,size_t n,uint8_t *out,size_t capacity,size_t *size,bool *more) {
    *size=0;*more=false;
    if (edge_industrial_frame_size(d,f,n)!=n) return false;
    size_t offset=0, count=0;
    if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_MC) {
        size_t h=edge_industrial_header_size(d),route=h-7;
        if (qn<h+12 || n<h+2 || memcmp(q+route,f+route,5) || le16(f+h)!=0 ||
            (d->industrial.mc_four_e && memcmp(q+2,f+2,4))) return false;
        bool write=le16(q+h+2)==0x1401,bit=le16(q+h+4)==1;
        count=write?0:bit?1:(size_t)le16(q+h+10)*2;offset=h+2;
        if (n-offset!=count || count>capacity) return false;
        if (bit && !write) { if ((f[offset]!=0 && f[offset]!=0x10)) return false;out[0]=(uint8_t)(f[offset]>>4);*size=1;return true; }
    } else if (d->protocol==iot_edge_v1_Protocol_PROTOCOL_FINS) {
        if (qn<34 || n<30 || be32(f+8)!=2 || be32(f+12)!=0 || !(f[16]&0x40) ||
            memcmp(f+19,q+22,3) || memcmp(f+22,q+19,3) || memcmp(f+25,q+25,3) || f[28] || f[29]) return false;
        count=q[27]==2?0:(size_t)(((uint16_t)q[32]<<8)|q[33])*(q[28]<0x80?1U:2U);offset=30;
        if (n-offset!=count) return false;
        if (count && q[28]<0x80 && (count!=1 || f[offset]>1)) return false;
    } else {
        size_t wake=0;while (wake<qn && q[wake]==0xfe) ++wake;
        q+=wake;qn-=wake;
        if (qn<12 || n<12 || memcmp(q+1,f+1,6) || f[n-1]!=0x16 ||
            !(f[8]&0x80) || (f[8]&0x40) || (f[8]&0x1f)!=q[8]) return false;
        uint8_t sum=0;for (size_t i=0;i<n-2;++i) sum=(uint8_t)(sum+f[i]);
        if (sum!=f[n-2]) return false;
        bool write=q[8]==0x14 || q[8]==0x04;
        size_t id=d->industrial.dlt645_version==2007?4:2;
        if (write) return f[9]==0 && !(f[8]&0x20);
        bool continuation=q[8]==0x12 || q[8]==0x02;
        if (f[9]<id+(continuation?1U:0U) || memcmp(q+10,f+10,id)) return false;
        if (continuation && f[n-3]!=q[10+id]) return false;
        *more=(f[8]&0x20)!=0;count=f[9]-id-(continuation?1U:0U);offset=10+id;
        if (!count || count>capacity) return false;
        for (size_t i=0;i<count;++i) out[i]=(uint8_t)(f[offset+i]-0x33);
        *size=count;return true;
    }
    if (n-offset!=count || count>capacity) return false;
    if (count) memcpy(out,f+offset,count);
    *size=count;return true;
}
void edge_industrial_redact(iot_edge_v1_Protocol protocol,uint8_t *f,size_t n) {
    if (protocol!=iot_edge_v1_Protocol_PROTOCOL_DLT645) return;
    size_t i=0;while (i<n && f[i]==0xfe) ++i;
    if (n-i<10 || f[i]!=0x68 || f[i+7]!=0x68) return;
    size_t id=f[i+8]==0x14?4:f[i+8]==0x04?2:0;
    if (!id) return;
    size_t begin=i+10+id,end=begin+(id==4?8:4);
    for (size_t p=begin;p<n && p<end;++p) f[p]=0;
}
bool edge_meter_decode(const iot_edge_v1_IndustrialPointConfig *p,const uint8_t *raw,size_t n,char *text,size_t capacity) {
    if (!n || n!=p->length) return false;
    if (!strcmp(p->data_type,"HEX")) {
        if (capacity<n*2+1) return false;
        static const char hex[]="0123456789ABCDEF";
        for (size_t i=0;i<n;++i) {text[2*i]=hex[raw[i]>>4];text[2*i+1]=hex[raw[i]&15];}text[2*n]=0;return true;
    }
    if (n>8 || p->digits>8) return false;
    bool signed_value=!strcmp(p->data_type,"BCD_SIGNED"), negative=signed_value && (raw[n-1]&0x80);
    char digits[25]={0};size_t length=0;
    for (size_t i=n;i>0;--i) {
        uint8_t b=(uint8_t)(raw[i-1] & (signed_value && i==n?0x7f:0xff));
        if ((b>>4)>9 || (b&15)>9) return false;
        digits[length++]=(char)('0'+(b>>4));digits[length++]=(char)('0'+(b&15));
    }
    while (length<=p->digits) {memmove(digits+1,digits,length++);digits[0]='0';}
    size_t start=0;while (start+1<length-p->digits && digits[start]=='0') ++start;
    if (capacity<length-start+3) return false;
    size_t output=0;if (negative) text[output++]='-';
    for (size_t i=start;i<length;++i) {if (p->digits && i==length-p->digits) text[output++]='.';text[output++]=digits[i];}
    text[output]=0;return true;
}
bool edge_meter_encode(const iot_edge_v1_IndustrialPointConfig *p,const char *text,uint8_t *raw,size_t capacity,size_t *size) {
    size_t n=p->length;if (!n || capacity<n) return false;
    if (!strcmp(p->data_type,"HEX")) {if (!hex_bytes(text,raw,n)) return false;*size=n;return true;}
    if (n>8 || p->digits>8) return false;
    bool negative=*text=='-',sign=!strcmp(p->data_type,"BCD_SIGNED");if (negative) ++text;
    if (!*text || (negative&&!sign)) return false;
    char digits[32];size_t length=0,fraction=0;bool decimal=false;
    for (;*text;++text) {
        if (*text=='.' && !decimal && length && text[1]) {decimal=true;continue;}
        if (*text<'0' || *text>'9' || length==sizeof(digits)) return false;
        digits[length++]=*text;if (decimal) ++fraction;
    }
    if (fraction>p->digits) return false;
    while (fraction++<p->digits) {if (length==sizeof(digits)) return false;digits[length++]='0';}
    size_t start=0;while (start<length && digits[start]=='0') ++start;
    if (length-start>n*2) return false;
    memset(raw,0,n);
    for (size_t i=0;i<length-start;++i) raw[i/2]|=(uint8_t)((digits[length-1-i]-'0')<<(i%2?4:0));
    if (sign && (raw[n-1]&0x80)) return false;
    if (negative) raw[n-1]|=0x80;
    *size=n;return true;
}
