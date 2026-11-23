/* SPDX-License-Identifier: GPL-3.0-or-later */
/* One UDP owner. All response fragments are copied into its fixed IO scratch
 * before sendto returns. No request/parameter view survives this step. */
#include "lan_wire_v4.inc"
#include "service_mp.h"
#include "sha.h"
#include "firmware_variant_defs.h"
#include "bonjour_lan.inc"
extern bool feature_lan_context(const char *,const uint8_t[16],ph_auth_context *);
static lan_v4_server lan_business;
/* Station Bonjour is a post-association service. Keep its lifecycle separate
 * from the UDP owner so the provisioning SoftAP does not pay for mDNS. */
static const char *lan_business_model, *lan_business_version;
static bool lan_business_network_up, lan_business_bonjour_started;
static uint32_t lan_business_now(void *user){(void)user;return mico_rtos_get_time();}
static bool lan_business_authorize(void *user,const char *peer,const uint8_t token[16],ph_auth_context *ctx)
{(void)user;return feature_lan_context(peer,token,ctx)&&!strcmp(ctx->peer,peer);}
static bool lan_business_valid(void *user,const ph_auth_context *ctx)
{(void)user;return (ctx->source==PH_AUTH_UDP_PAIR||ctx->source==PH_AUTH_UDP_MQTT)&&ph_auth_context_valid(ctx,PH_AUTH_SCOPE_CONTROL);}
static bool lan_business_digest(void *user,const void *data,size_t length,uint8_t digest[32])
{(void)user;SHA256Context s;return !SHA256Reset(&s)&&!SHA256Input(&s,data,length)&&!SHA256Result(&s,digest);}
static ph_buf *lan_business_dispatch(void *user,const void *data,size_t length,const ph_auth_context *ctx,bool *admitted)
{
    (void)user;unsigned claim=service_memory_try_claim(length);
    if(!claim){*admitted=false;return NULL;}
    ph_buf *result=service_lan_try_command_mp(data,length,ctx,admitted);
    service_memory_release(claim);return result;
}
static bool lan_business_send(void *user,const char *ip,uint16_t port,const void *data,size_t length)
{
    (void)user;if(udp<0||!ip||!port||length>LAN_V4_FRAME_MAX)return false;
    struct sockaddr_in to;memset(&to,0,sizeof(to));to.sin_family=AF_INET;to.sin_port=htons(port);to.sin_addr.s_addr=inet_addr(ip);
    if(!to.sin_addr.s_addr||to.sin_addr.s_addr==INADDR_NONE)return false;
    return sendto(udp,data,length,0,(struct sockaddr *)&to,sizeof(to))==(int)length;
}
static lan_v4_ops lan_business_ops={LOCAL_ID,0,lan_business_now,lan_business_authorize,lan_business_valid,lan_business_digest,lan_business_dispatch,lan_business_send,NULL};
static bool lan_business_input(const ph_value *root,const char *ip,uint16_t port,void *scratch,size_t capacity)
{
    ph_value v;if(!phv_get(root,"v",&v))return false;int64_t n;if(!phv_int(&v,&n)||n!=4)return false;
    lan_v4_packet packet;
    if(lan_v4_decode_value(root,&packet))lan_v4_server_input(&lan_business,&lan_business_ops,&packet,ip,port,scratch,capacity);
    return true; /* Recognized v4 must never fall through to legacy grants. */
}
static void lan_business_start(uint32_t boot,const char *model,const char *version)
{
    lan_business_ops.boot=boot?boot:1;
    lan_business_model=model;lan_business_version=version;
    if(lan_business_network_up && udp>=0 && !lan_business_bonjour_started)
        lan_business_bonjour_started=bonjour_lan_start(LOCAL_ID,model,PHICOMM_FIRMWARE_VARIANT,version);
}
static void lan_business_network_ready(bool ready)
{
    lan_business_network_up=ready;
    if(!ready){
        if(lan_business_bonjour_started)bonjour_lan_stop();
        lan_business_bonjour_started=false;
        return;
    }
    if(udp>=0 && lan_business_model && !lan_business_bonjour_started)
        lan_business_bonjour_started=bonjour_lan_start(LOCAL_ID,lan_business_model,PHICOMM_FIRMWARE_VARIANT,lan_business_version);
}
static void lan_business_poll(void){lan_v4_server_expire(&lan_business,&lan_business_ops);}
static void lan_business_stop(void){lan_business_network_ready(false);lan_business_model=NULL;lan_business_version=NULL;lan_v4_server_release(&lan_business);}
