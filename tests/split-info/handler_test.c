/* SPDX-License-Identifier: MIT
 * Exercise the complete production dispatcher and label store with real
 * owner/nanopb codecs. Only Zephyr, ZMK APIs and the Studio allocation
 * boundary are controlled. All address bytes and labels below are synthetic
 * fixtures; they do not identify bonded devices.
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include <zmk/ble_management/ble_management.pb.h>

#define IS_ENABLED(x) (x)
#define LOG_MODULE_DECLARE(...)
#define LOG_MODULE_REGISTER(...)
#define LOG_DBG(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#include <zephyr/bluetooth/addr.h>
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define ZMK_STUDIO_RPC_HANDLER_UNSECURED 0
#define ZMK_RPC_CUSTOM_SUBSYSTEM_UI_URLS(...) .unused = 0
#define ZMK_RPC_CUSTOM_SUBSYSTEM(...)
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER(...)
#define ZMK_RPC_CUSTOM_SUBSYSTEM_RESPONSE_BUFFER_ALLOCATE(name, callback) allocate_response(callback)
#define SETTINGS_STATIC_HANDLER_DEFINE(...)

typedef int (*settings_read_cb)(void *, void *, size_t);
struct zmk_rpc_custom_subsystem_meta { int unused, security; };
typedef struct { struct { size_t size; uint8_t bytes[128]; } payload; } zmk_custom_CallRequest;
enum zmk_transport { ZMK_TRANSPORT_USB, ZMK_TRANSPORT_BLE };
static unsigned saves, deletes, selects, clears, clear_alls, transports;
enum zmk_studio_core_lock_state {
    ZMK_STUDIO_CORE_LOCK_STATE_LOCKED = 0,
    ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED = 1,
};
static enum zmk_studio_core_lock_state lock_state = ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
static enum zmk_studio_core_lock_state zmk_studio_core_get_lock_state(void) { return lock_state; }
static zmk_ble_management_Response response;
static char saved_key[64], saved_value[32], deleted_keys[8][64];
static size_t saved_size;
static bt_addr_le_t addresses[ZMK_BLE_PROFILE_COUNT];

/* Bluetooth links seen by bt_conn_foreach(). */
struct bt_conn { size_t id; };
enum bt_conn_type { BT_CONN_TYPE_LE = 1 };
enum { BT_CONN_ROLE_CENTRAL = 0, BT_CONN_ROLE_PERIPHERAL = 1 };
enum bt_conn_state { BT_CONN_STATE_DISCONNECTED, BT_CONN_STATE_CONNECTING,
                     BT_CONN_STATE_CONNECTED, BT_CONN_STATE_DISCONNECTING };
struct bt_conn_info { uint8_t type, role; enum bt_conn_state state; };
static struct { uint8_t role; enum bt_conn_state state; } links[4];
static size_t link_count;
static void bt_conn_foreach(enum bt_conn_type type, void (*func)(struct bt_conn *, void *), void *data) {
    assert(type == BT_CONN_TYPE_LE);
    for (size_t i = 0; i < link_count; i++) { struct bt_conn conn = {i}; func(&conn, data); }
}
static int bt_conn_get_info(const struct bt_conn *conn, struct bt_conn_info *info) {
    info->type = BT_CONN_TYPE_LE; info->role = links[conn->id].role; info->state = links[conn->id].state;
    return 0;
}

static zmk_ble_management_Response *allocate_response(pb_callback_t *callback) {
    memset(&response, 0, sizeof(response));
    memset(callback, 0, sizeof(*callback));
    return &response;
}
size_t hex2bin(const char *hex, size_t length, unsigned char *out, size_t capacity) {
    if(length!=2 || capacity<1)return 0;
    unsigned int value=0;
    for(size_t i=0;i<2;i++){
        unsigned char c=(unsigned char)hex[i];
        unsigned int digit;
        if(c>='0'&&c<='9')digit=c-'0';
        else if(c>='a'&&c<='f')digit=c-'a'+10;
        else if(c>='A'&&c<='F')digit=c-'A'+10;
        else return 0;
        value=value*16+digit;
    }
    *out=(unsigned char)value;return 1;
}
int bt_rand(void *out, size_t size) { (void)out;(void)size;return -ENOTSUP; }
static int settings_save_one(const char *name, const void *value, size_t size) {
    assert(strlen(name)<sizeof(saved_key));assert(size<=sizeof(saved_value));
    saves++;strcpy(saved_key,name);memcpy(saved_value,value,size);saved_size=size;return 0;
}
static int settings_delete(const char *name) {
    assert(strlen(name)<sizeof(deleted_keys[0]));
    if(deletes<ARRAY_SIZE(deleted_keys))strcpy(deleted_keys[deletes],name);
    deletes++;return 0;
}
static bool deleted(const char *name) {
    for(size_t i=0;i<deletes&&i<ARRAY_SIZE(deleted_keys);i++)if(strcmp(deleted_keys[i],name)==0)return true;
    return false;
}
static bool settings_name_steq(const char *name, const char *key, const char **next) {
    size_t n=strlen(key);*next=NULL;
    if(strncmp(name,key,n)!=0 || (name[n]!='/' && name[n]!='\0'))return false;
    if(name[n]=='/')*next=name+n+1;
    return true;
}
static int zmk_ble_active_profile_index(void) { return 0; }
static bool zmk_ble_profile_is_open(uint8_t i) { (void)i; return true; }
static bool zmk_ble_profile_is_connected(uint8_t i) { (void)i; return false; }
static bt_addr_le_t *zmk_ble_profile_address(uint8_t i) { assert(i<ZMK_BLE_PROFILE_COUNT);return &addresses[i]; }
static int zmk_ble_profile_index(const bt_addr_le_t *addr) {
    for(int i=0;i<ZMK_BLE_PROFILE_COUNT;i++)if(bt_addr_le_eq(&addresses[i],addr))return i;
    return -ENODEV;
}
static int zmk_ble_prof_select(uint8_t i) { (void)i; selects++; return 0; }
static void zmk_ble_clear_bonds(void) { clears++; }
static void zmk_ble_clear_all_bonds(void) { clear_alls++; }
static int zmk_endpoint_set_preferred_transport(enum zmk_transport transport) { (void)transport; transports++; return 0; }
static enum zmk_transport zmk_endpoint_get_preferred_transport(void) { return ZMK_TRANSPORT_USB; }

#include LABELS_SOURCE
#include HANDLER_SOURCE

static void reset_labels(void) {
    memset(profile_names,0,sizeof(profile_names));
    saves=deletes=selects=clears=clear_alls=transports=0;
}
static void unbond_all(void) {
    for(size_t i=0;i<ZMK_BLE_PROFILE_COUNT;i++)bt_addr_le_copy(&addresses[i],BT_ADDR_LE_NONE);
}

static void check_split_info(bool peripheral_up) {
    zmk_ble_management_Request req = zmk_ble_management_Request_init_zero;
    req.which_request_type = zmk_ble_management_Request_get_split_info_tag;
    zmk_custom_CallRequest raw = {0};
    pb_ostream_t request = pb_ostream_from_buffer(raw.payload.bytes, sizeof(raw.payload.bytes));
    assert(pb_encode(&request, zmk_ble_management_Request_fields, &req));
    raw.payload.size = request.bytes_written;
    /* A connected host link and a connecting half never count as the half. */
    links[0].role = BT_CONN_ROLE_PERIPHERAL; links[0].state = BT_CONN_STATE_CONNECTED;
    links[1].role = BT_CONN_ROLE_CENTRAL; links[1].state = BT_CONN_STATE_CONNECTING;
    links[2].role = BT_CONN_ROLE_CENTRAL;
    links[2].state = peripheral_up ? BT_CONN_STATE_CONNECTED : BT_CONN_STATE_DISCONNECTING;
    link_count = 3;
    pb_callback_t callback = {0};
    assert(ble_management_rpc_handle_request(&raw, &callback));

    uint8_t bytes[zmk_ble_management_Response_size];
    pb_ostream_t encoded = pb_ostream_from_buffer(bytes, sizeof(bytes));
    assert(pb_encode(&encoded, zmk_ble_management_Response_fields, &response));
    zmk_ble_management_Response decoded = zmk_ble_management_Response_init_zero;
    pb_istream_t input = pb_istream_from_buffer(bytes, encoded.bytes_written);
    assert(pb_decode(&input, zmk_ble_management_Response_fields, &decoded));
    assert(input.bytes_left == 0);
    assert(decoded.which_response_type == zmk_ble_management_Response_get_split_info_tag);
    assert(decoded.response_type.get_split_info.has_info);
    const zmk_ble_management_SplitInfo *info = &decoded.response_type.get_split_info.info;
    assert(info->is_split == (CONFIG_ZMK_SPLIT_BLE != 0));
    assert(info->is_central == (CONFIG_ZMK_SPLIT_BLE != 0));
    assert(info->peripheral_connected == (CONFIG_ZMK_SPLIT_BLE && peripheral_up));
    assert(!info->central_bonded); /* a peripheral's view; Studio runs on the central */
    printf("split-info %s peripheral=%d wire=", TEST_ROLE, peripheral_up);
    for (size_t i = 0; i < encoded.bytes_written; i++) { printf("%02x", bytes[i]); }
    puts(" PASS");
}

struct reader {const char *bytes;size_t size;int result;};
static int read_setting(void *arg, void *dst, size_t size) {
    struct reader *r=arg;size_t n=r->size<size?r->size:size;memcpy(dst,r->bytes,n);
    return r->result;
}
static int load(const char *key, const char *value, size_t len, int result) {
    struct reader r={value,len,result};return profile_names_settings_set(key,len,read_setting,&r);
}
static void check_storage_key(void) {
    puts("storage-key: same MAC, four independent LE address types, save/reload");fflush(stdout);
    reset_labels();
    for(unsigned int type=0;type<4;type++){
        bt_addr_le_t a={.type=type,.a={{6,5,4,3,2,1}}};
        char value[32],key[64];snprintf(value,sizeof(value),"type-%u",type);
        assert(save_profile_name(&a,value)==0);
        snprintf(key,sizeof(key),"ble_mgmt/name/%02x/01:02:03:04:05:06",type);
        assert(strcmp(saved_key,key)==0);
    }
    reset_labels();
    for(unsigned int type=0;type<4;type++){
        bt_addr_le_t a={.type=type,.a={{6,5,4,3,2,1}}};
        char value[32],key[64];snprintf(value,sizeof(value),"type-%u",type);
        snprintf(key,sizeof(key),"name/%02x/01:02:03:04:05:06",type);
        assert(load(key,value,strlen(value)+1,(int)strlen(value)+1)==0);
        assert(strcmp(get_profile_name(&a),value)==0);
    }
    /* A modern public record wins over a legacy public key in either order. */
    for(int order=0;order<2;order++){
        reset_labels();
        const char *old="name/01:02:03:04:05:06",*modern="name/00/01:02:03:04:05:06";
        assert(load(order?modern:old,order?"":"legacy",order?1:7,order?1:7)==0);
        assert(load(order?old:modern,order?"legacy":"",order?7:1,order?7:1)==0);
        bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
        assert(strcmp(get_profile_name(&a),"")==0);
    }
    /* Unpairing deletes both stored keys and frees the slot for reuse. */
    bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
    addresses[0]=a;
    zmk_ble_management_UnpairProfileRequest unpair=zmk_ble_management_UnpairProfileRequest_init_zero;
    zmk_ble_management_Response result=zmk_ble_management_Response_init_zero;
    deletes=0;
    assert(handle_unpair_profile(&unpair,&result)==0);
    assert(result.response_type.unpair_profile.success);
    assert(deleted("ble_mgmt/name/00/01:02:03:04:05:06")&&deleted("ble_mgmt/name/01:02:03:04:05:06"));
    assert(strcmp(get_profile_name(&a),"")==0);
    assert(load("name/01:02:03:04:05:06","legacy",7,7)==0);
    assert(strcmp(get_profile_name(&a),"legacy")==0);
    unbond_all();
    puts("storage-key PASS");
}
static void check_storage_read(void) {
    puts("storage-read: negative/short/invalid reads preserve cache and empty slots");fflush(stdout);
    bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
    const char *legacy="name/01:02:03:04:05:06";
    reset_labels();assert(save_profile_name(&a,"stable")==0);
    int rc=load(legacy,"poison",7,-EIO);
    assert(strcmp(get_profile_name(&a),"stable")==0);assert(rc==-EIO);
    rc=load(legacy,"short",6,2);
    assert(strcmp(get_profile_name(&a),"stable")==0);assert(rc==-EIO);
    char full[32];memset(full,'X',sizeof(full));
    assert(load(legacy,full,sizeof(full),sizeof(full))==-EINVAL);
    assert(strcmp(get_profile_name(&a),"stable")==0);
    char long_value[33];memset(long_value,'Y',sizeof(long_value));
    assert(load(legacy,long_value,sizeof(long_value),sizeof(long_value))==-EINVAL);
    assert(load(legacy,"",0,0)==-EINVAL);
    assert(strcmp(get_profile_name(&a),"stable")==0);
    /* Successful legacy records remain readable; failed reads allocate nothing. */
    reset_labels();assert(load(legacy,"poison",7,-EIO)==-EIO);
    for(size_t i=0;i<ARRAY_SIZE(profile_names);i++)assert(!profile_names[i].used);
    assert(load(legacy,"legacy",7,7)==0);assert(strcmp(get_profile_name(&a),"legacy")==0);
    char max_name[32];memset(max_name,'M',31);max_name[31]='\0';
    assert(load(legacy,max_name,32,32)==0);assert(strcmp(get_profile_name(&a),max_name)==0);
    assert(load("name/01:02:03:04:05:06/extra","bad",4,4)==0);
    assert(load("name/04/01:02:03:04:05:06","bad",4,4)==0);
    assert(strcmp(get_profile_name(&a),max_name)==0);
    puts("storage-read PASS");
}
static void check_label_lifecycle(void) {
    puts("labels: empty name deletes, stale labels are reclaimed, legacy keys retire");fflush(stdout);
    bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
    /* An empty name deletes the label instead of storing "". */
    reset_labels();assert(save_profile_name(&a,"desk")==0&&saves==1);
    assert(save_profile_name(&a,"")==0);
    assert(saves==1&&deleted("ble_mgmt/name/00/01:02:03:04:05:06"));
    assert(strcmp(get_profile_name(&a),"")==0&&!profile_names[0].used);
    /* Saving a typed key retires the legacy key it replaces, once. */
    reset_labels();assert(load("name/01:02:03:04:05:06","legacy",7,7)==0);
    assert(save_profile_name(&a,"typed")==0&&deletes==1&&deleted("ble_mgmt/name/01:02:03:04:05:06"));
    assert(save_profile_name(&a,"again")==0&&deletes==1);
    /* Every slot holds a label of an unbonded host: a new label reclaims one
     * and deletes its record; with every label still bonded there is no room. */
    reset_labels();unbond_all();
    for(int i=0;i<ZMK_BLE_PROFILE_COUNT;i++){
        bt_addr_le_t stale={.type=BT_ADDR_LE_RANDOM,.a={{(uint8_t)i,9,9,9,9,9}}};
        assert(save_profile_name(&stale,"old")==0);
    }
    deletes=0;
    addresses[0]=a;
    assert(save_profile_name(&a,"new")==0&&strcmp(get_profile_name(&a),"new")==0);
    assert(deletes==1&&deleted("ble_mgmt/name/01/09:09:09:09:09:00"));
    for(int i=0;i<ZMK_BLE_PROFILE_COUNT;i++)addresses[i]=profile_names[i].addr;
    bt_addr_le_t other={.type=BT_ADDR_LE_PUBLIC,.a={{7,7,7,7,7,7}}};
    assert(save_profile_name(&other,"full")==-ENOMEM);
    unbond_all();
    puts("labels PASS");
}
static zmk_ble_management_Response call(zmk_ble_management_Request req) {
    zmk_custom_CallRequest raw={0};pb_ostream_t w=pb_ostream_from_buffer(raw.payload.bytes,sizeof(raw.payload.bytes));
    assert(pb_encode(&w,zmk_ble_management_Request_fields,&req));raw.payload.size=w.bytes_written;
    pb_callback_t cb={0};assert(ble_management_rpc_handle_request(&raw,&cb));
    uint8_t bytes[zmk_ble_management_Response_size];w=pb_ostream_from_buffer(bytes,sizeof(bytes));
    assert(pb_encode(&w,zmk_ble_management_Response_fields,&response));
    zmk_ble_management_Response decoded=zmk_ble_management_Response_init_zero;
    pb_istream_t r=pb_istream_from_buffer(bytes,w.bytes_written);
    assert(pb_decode(&r,zmk_ble_management_Response_fields,&decoded));
    return decoded;
}
static void check_profiles(void) {
    puts("profiles: bounded real protobuf response");fflush(stdout);
    zmk_ble_management_Request req=zmk_ble_management_Request_init_zero;
    req.which_request_type=zmk_ble_management_Request_get_profiles_tag;
    zmk_ble_management_Response decoded=call(req);
    if(ZMK_BLE_PROFILE_COUNT>5){
        assert(decoded.which_response_type==zmk_ble_management_Response_error_tag);
        assert(strstr(decoded.response_type.error.message,"-7")!=NULL); /* -E2BIG */
    }else{
        assert(decoded.which_response_type==zmk_ble_management_Response_get_profiles_tag);
        assert(decoded.response_type.get_profiles.profiles_count==ZMK_BLE_PROFILE_COUNT);
        for(size_t i=0;i<ZMK_BLE_PROFILE_COUNT;i++)assert(decoded.response_type.get_profiles.profiles[i].index==i);
    }
    printf("profiles %s count=%d PASS\n",TEST_ROLE,ZMK_BLE_PROFILE_COUNT);
}
static void check_forget_split(void) {
    puts("forget-split: host bonds are cleared and their labels deleted");fflush(stdout);
    reset_labels();unbond_all();
    bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
    addresses[1]=a;assert(save_profile_name(&a,"desk")==0);
    zmk_ble_management_Request req=zmk_ble_management_Request_init_zero;
    req.which_request_type=zmk_ble_management_Request_forget_split_bond_tag;
    zmk_ble_management_Response resp=call(req);
    assert(resp.which_response_type==zmk_ble_management_Response_forget_split_bond_tag);
    assert(resp.response_type.forget_split_bond.success==(CONFIG_ZMK_SPLIT_BLE!=0));
    assert(clear_alls==(CONFIG_ZMK_SPLIT_BLE?1u:0u));
    assert(strcmp(get_profile_name(&a),CONFIG_ZMK_SPLIT_BLE?"":"desk")==0);
    unbond_all();
    printf("forget-split %s PASS\n",TEST_ROLE);
}
static void check_unlock(void) {
    puts("unlock: changes need an unlocked Studio; addresses only while unlocked");fflush(stdout);
    reset_labels();
    bt_addr_le_t a={.type=BT_ADDR_LE_PUBLIC,.a={{6,5,4,3,2,1}}};
    addresses[0]=a;
    assert(save_profile_name(&a,"desk")==0);
    const pb_size_t changes[]={zmk_ble_management_Request_set_profile_name_tag,
                               zmk_ble_management_Request_switch_profile_tag,
                               zmk_ble_management_Request_unpair_profile_tag,
                               zmk_ble_management_Request_forget_split_bond_tag,
                               zmk_ble_management_Request_set_output_priority_tag};
    lock_state=ZMK_STUDIO_CORE_LOCK_STATE_LOCKED;
    saves=deletes=selects=clears=clear_alls=transports=0;
    for(size_t i=0;i<ARRAY_SIZE(changes);i++){
        zmk_ble_management_Request req=zmk_ble_management_Request_init_zero;
        req.which_request_type=changes[i];
        if(changes[i]==zmk_ble_management_Request_set_profile_name_tag){
            strcpy(req.request_type.set_profile_name.name,"renamed");
        }
        zmk_ble_management_Response resp=call(req);
        assert(resp.which_response_type==zmk_ble_management_Response_error_tag);
        assert(strcmp(resp.response_type.error.message,"Unlock required")==0);
    }
    assert(saves==0&&deletes==0&&selects==0&&clears==0&&clear_alls==0&&transports==0);
    assert(strcmp(get_profile_name(&a),"desk")==0);
    /* Reads still work while locked, without the bonded host's address. */
    zmk_ble_management_Request list=zmk_ble_management_Request_init_zero;
    list.which_request_type=zmk_ble_management_Request_get_profiles_tag;
    zmk_ble_management_Response resp=call(list);
    assert(resp.which_response_type==zmk_ble_management_Response_get_profiles_tag);
    assert(resp.response_type.get_profiles.profiles[0].address[0]=='\0');
    assert(strcmp(resp.response_type.get_profiles.profiles[0].name,"desk")==0);
    zmk_ble_management_Request split=zmk_ble_management_Request_init_zero;
    split.which_request_type=zmk_ble_management_Request_get_split_info_tag;
    assert(call(split).which_response_type==zmk_ble_management_Response_get_split_info_tag);
    zmk_ble_management_Request output=zmk_ble_management_Request_init_zero;
    output.which_request_type=zmk_ble_management_Request_get_output_priority_tag;
    assert(call(output).which_response_type==zmk_ble_management_Response_get_output_priority_tag);
    lock_state=ZMK_STUDIO_CORE_LOCK_STATE_UNLOCKED;
    resp=call(list);
    assert(strcmp(resp.response_type.get_profiles.profiles[0].address,"01:02:03:04:05:06")==0);
    zmk_ble_management_Request rename=zmk_ble_management_Request_init_zero;
    rename.which_request_type=zmk_ble_management_Request_set_profile_name_tag;
    strcpy(rename.request_type.set_profile_name.name,"renamed");
    resp=call(rename);
    assert(resp.which_response_type==zmk_ble_management_Response_set_profile_name_tag);
    assert(saves==1&&strcmp(get_profile_name(&a),"renamed")==0);
    unbond_all();
    puts("unlock PASS");
}
static void check_all_split_info(void) {
    check_split_info(false);
    check_split_info(true);
    /* Malformed and unsupported input must not reuse a prior info response. */
    zmk_custom_CallRequest malformed = {.payload = {.size = 1, .bytes = {0x80}}};
    pb_callback_t callback = {0};
    assert(ble_management_rpc_handle_request(&malformed, &callback));
    assert(response.which_response_type == zmk_ble_management_Response_error_tag);
    zmk_custom_CallRequest empty = {0};
    assert(ble_management_rpc_handle_request(&empty, &callback));
    assert(response.which_response_type == zmk_ble_management_Response_error_tag);
    check_split_info(false);
}
int main(int argc,char **argv) {
    const char *test=argc>1?argv[1]:"all";
    unbond_all();
    if(strcmp(test,"all")==0||strcmp(test,"split-info")==0)check_all_split_info();
    if(strcmp(test,"all")==0||strcmp(test,"storage-key")==0)check_storage_key();
    if(strcmp(test,"all")==0||strcmp(test,"storage-read")==0)check_storage_read();
    if(strcmp(test,"all")==0||strcmp(test,"labels")==0)check_label_lifecycle();
    if(strcmp(test,"all")==0||strcmp(test,"profiles")==0)check_profiles();
    if(strcmp(test,"all")==0||strcmp(test,"forget-split")==0)check_forget_split();
    if(strcmp(test,"all")==0||strcmp(test,"unlock")==0)check_unlock();
    return 0;
}
