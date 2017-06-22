/* mbed Microcontroller Library
 * Copyright (c) 2016 ARM Limited
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "emac_lwip_l2_bridge.h"
#include "emac_api.h"
#include "string.h"
#include "mbed_assert.h"
#include "sys.h"

#define EMAC_LWIP_L2B_MAC_ADDR_SIZE         (6)
#define EMAC_LWIP_L2B_THREAD_STACKSIZE      (256)
#define EMAC_LWIP_L2B_THREAD_PRIO           (osPriorityAboveNormal)

#define EMAC_LWIP_L2B_MAC_SRC(buf)  (&(((u8_t*)emac_stack_mem_ptr(buf))[EMAC_LWIP_L2B_MAC_ADDR_SIZE]))
#define EMAC_LWIP_L2B_MAC_DEST(buf) ((u8_t*)emac_stack_mem_ptr(buf))

#define EMAC_LWIP_L2B_IS_MULTICAST(mac_address)  ((mac_address[0] & 0x01) == 0x01)

#define EMAC_LWIP_L2B_IS_BROADCAST(mac_address)  ((mac_address[0] == 0xFF) &&\
                                                  (mac_address[1] == 0xFF) &&\
                                                  (mac_address[2] == 0xFF) &&\
                                                  (mac_address[3] == 0xFF) &&\
                                                  (mac_address[4] == 0xFF) &&\
                                                  (mac_address[5] == 0xFF))


typedef struct emac_lwip_l2b_entry_s {
    struct emac_lwip_l2b_entry_s    *next;
    struct emac_lwip_l2b_entry_s    *previous;

    struct netif                    *net;
    u8_t                            mac_address[EMAC_LWIP_L2B_MAC_ADDR_SIZE];
    int                             ticks;
} emac_lwip_l2b_entry_t;

typedef struct {
    bool    active;

    struct netif *net;
} emac_lwip_l2b_netif_t;

struct emac_lwip_l2b_bridge_s {
    int                      entries_count;
    emac_lwip_l2b_entry_t    *entries;
    emac_lwip_l2b_netif_t    netifs[EMAC_LWIP_L2B_MAX_NETIFS];
    sys_mutex_t              mutex;
    sys_thread_t             thread;
};

static bool                             _initialised = false;
static struct emac_lwip_l2b_bridge_s    *_bridge = 0;

static void remove_inactive_entries_thread()
{
    emac_lwip_l2b_entry_t   *next;
    emac_lwip_l2b_entry_t   *entry;

    while(true) {

        sys_msleep(EMAC_LWIP_L2B_TIMER_INTERVAL);

        sys_mutex_lock(&(_bridge->mutex));
        entry = _bridge->entries;

        while(entry != 0) {
            next = entry->next;

            entry->ticks++;

            if(entry->ticks > EMAC_LWIP_L2B_ENTRY_TIMEOUT) {
                if(entry->previous != 0) {
                    entry->previous->next = entry->next;
                }
                if(entry->next != 0) {
                    entry->next->previous = entry->previous;
                }
                if(entry == _bridge->entries) {
                    _bridge->entries = entry->next;
                }

                free(entry);
                _bridge->entries_count--;
            }

            entry = next;
        }
        sys_mutex_unlock(&(_bridge->mutex));
    }
}

static emac_lwip_l2b_entry_t* get_bridge_entry(uint8_t *mac_address)
{
    emac_lwip_l2b_entry_t   *found = 0;

    MBED_ASSERT(mac_address != 0);

    if(_bridge->entries_count > 0) {
        emac_lwip_l2b_entry_t *entry;

        for(entry = _bridge->entries; (entry != 0) && (found == 0); entry = entry->next) {
            if(memcmp(mac_address, entry->mac_address, EMAC_LWIP_L2B_MAC_ADDR_SIZE) == 0) {
                found = entry;
            }
        }
    }

    return found;
}

static emac_lwip_l2b_entry_t* alloc_bridge_entry(struct netif *net, uint8_t *mac_address)
{
    emac_lwip_l2b_entry_t *entry = 0;

    MBED_ASSERT(net != 0);
    MBED_ASSERT(mac_address != 0);

    //Room in list
    if(_bridge->entries_count < EMAC_LWIP_L2B_MAX_BRIDGE_ENTRIES) {
        entry = malloc(sizeof(emac_lwip_l2b_entry_t));
        MBED_ASSERT(entry != 0);

        // Place first
        entry->next = _bridge->entries;
        entry->previous = 0;
        if(_bridge->entries != 0) {
            _bridge->entries->previous = entry;
        }
        _bridge->entries = entry;
        _bridge->entries_count++;
    }
    // Re-use oldest one in list
    else {
        MBED_ASSERT(_bridge != 0);

        emac_lwip_l2b_entry_t *oldest_entry = _bridge->entries;

        entry = _bridge->entries->next;
        while(entry != 0) {
            if(entry->ticks >= oldest_entry->ticks) {
                oldest_entry = entry;
            }

            entry = entry->next;
        }

        entry = oldest_entry;
    }
    MBED_ASSERT(entry != 0);

    // Initialize
    entry->ticks = 0;
    entry->net = net;
    memcpy(entry->mac_address, mac_address, EMAC_LWIP_L2B_MAC_ADDR_SIZE);

    return entry;
}

static err_t output_from_netif_to_netifs(struct netif *net, emac_stack_mem_chain_t *buf)
{
    err_t              res = ERR_IF;
    bool               ok;
    emac_interface_t   *emac;

    MBED_ASSERT(net != 0);
    MBED_ASSERT(buf != 0);

    for(int i = 0; i < EMAC_LWIP_L2B_MAX_NETIFS; i++) {
        if(_bridge->netifs[i].active) {
            emac = (emac_interface_t*)(_bridge->netifs[i].net->state);
            MBED_ASSERT(emac != 0);

            if((_bridge->netifs[i].net != net) ||
               ((emac->flags & EMAC_FLAGS_BROADCAST_TO_SELF) != 0)) {

                ok = emac->ops->link_out(emac, buf);

                if(ok) {
                    res = ERR_OK; //At least one successful transmission
                }
            }
        }
    }

    return res;
}

static err_t output_from_local_to_netifs(emac_stack_mem_chain_t *buf)
{
    MBED_ASSERT(buf != 0);

    err_t               res = ERR_IF;
    bool                ok;
    emac_interface_t    *emac;
    u8_t                *mac_src = EMAC_LWIP_L2B_MAC_SRC(buf);

    for(int i = 0; i < EMAC_LWIP_L2B_MAX_NETIFS; i++) {
        if(_bridge->netifs[i].active) {
            emac = (emac_interface_t*)(_bridge->netifs[i].net->state);
            MBED_ASSERT(emac != 0);

            //TODO: Works if each interface copies data or is done with buf when link_out is returned
            memcpy(mac_src, _bridge->netifs[i].net->hwaddr, EMAC_LWIP_L2B_MAC_ADDR_SIZE);

            ok = emac->ops->link_out(emac, buf);

            if(ok) {
                res = ERR_OK; //At least one successful transmission
            }
        }
    }

    return res;
}

static bool is_addr_local(u8_t *mac_addr)
{
    bool found = false;

    MBED_ASSERT(mac_addr != 0);

    for(int i = 0; (i < EMAC_LWIP_L2B_MAX_NETIFS) && (!found); i++) {
        if(_bridge->netifs[i].active) {
            if(memcmp(mac_addr, _bridge->netifs[i].net->hwaddr, EMAC_LWIP_L2B_MAC_ADDR_SIZE) == 0) {
                found = true;
            }
        }
    }

    return found;
}

static void touch_bridge_entry(struct netif *net, u8_t *mac_address)
{
    MBED_ASSERT(net != 0);
    MBED_ASSERT(mac_address != 0);

    emac_lwip_l2b_entry_t *entry = get_bridge_entry(mac_address);

    if(entry != 0) {
        entry->net = net;
        entry->ticks = 0;
    }
    else {
        entry = alloc_bridge_entry(net, mac_address);
        MBED_ASSERT(entry != 0);
    }
}

err_t emac_lwip_l2b_register_interface(struct netif *net)
{
    err_t res = ERR_MEM;

    MBED_ASSERT(net != 0);

    if(!_initialised) {
        _bridge = malloc(sizeof(struct emac_lwip_l2b_bridge_s));
        MBED_ASSERT(_bridge != 0);
        memset(_bridge, 0, sizeof(struct emac_lwip_l2b_bridge_s));

        sys_mutex_new(&(_bridge->mutex));

        for(int i = 0; i < EMAC_LWIP_L2B_MAX_NETIFS; i++) {
            _bridge->netifs[i].active = false;
        }

        _bridge->thread = sys_thread_new("emac_lwip_l2b", remove_inactive_entries_thread, 0, EMAC_LWIP_L2B_THREAD_STACKSIZE, EMAC_LWIP_L2B_THREAD_PRIO);

        _initialised = true;
    }

    for(int i = 0; (i < EMAC_LWIP_L2B_MAX_NETIFS) && (res != ERR_OK); i++) {
        if(_bridge->netifs[i].active == false) {
            _bridge->netifs[i].active = true;
            _bridge->netifs[i].net = net;
            res = ERR_OK;
        }
    }

    return res;
}

err_t emac_lwip_l2b_output(struct netif *netif, emac_stack_mem_chain_t *buf)
{
    MBED_ASSERT(netif != 0);
    MBED_ASSERT(buf != 0);

    bool    ok = false;
    err_t   res = ERR_IF;

    u8_t    *mac_src = EMAC_LWIP_L2B_MAC_SRC(buf);
    u8_t    *mac_dest = EMAC_LWIP_L2B_MAC_DEST(buf);

    //All
    if(EMAC_LWIP_L2B_IS_MULTICAST(mac_dest)) {
        res = output_from_local_to_netifs(buf);
    }
    //Other
    else {
        sys_mutex_lock(&(_bridge->mutex));
        emac_lwip_l2b_entry_t *entry = get_bridge_entry(mac_dest);

        //Forward
        if(entry != 0) {
            emac_interface_t *emac = (emac_interface_t *)(entry->net->state);

            if(netif != entry->net) {
                memcpy(mac_src, entry->net->hwaddr, EMAC_LWIP_L2B_MAC_ADDR_SIZE);
            }
            sys_mutex_unlock(&(_bridge->mutex));

            ok = emac->ops->link_out(emac, buf);

            if(ok) {
                res = ERR_OK;
            }
        }
        //Flood
        else {
            sys_mutex_unlock(&(_bridge->mutex));
            res = output_from_local_to_netifs(buf);
        }
    }

    return res;
}

err_t emac_lwip_l2b_input(struct netif *net, emac_stack_mem_t *buf)
{
    MBED_ASSERT(net != 0);
    MBED_ASSERT(buf != 0);

    err_t   res;
    bool    free_msg = true;

    emac_stack_mem_ref(buf);

    u8_t    *mac_src = EMAC_LWIP_L2B_MAC_SRC(buf);
    u8_t    *mac_dest = EMAC_LWIP_L2B_MAC_DEST(buf);

    //All
    if(EMAC_LWIP_L2B_IS_MULTICAST(mac_dest)) {

        // Netifs
        res = output_from_netif_to_netifs(net, buf);

        //Local
        res = net->input((struct pbuf *)buf, net);
        if(res == ERR_OK) {
            free_msg = false;
        }
    }
    //Local only
    else if(is_addr_local(mac_dest)) {
        res = net->input((struct pbuf *)buf, net);
        if(res == ERR_OK) {
            free_msg = false;
        }
    }
    //Other
    else {
        sys_mutex_lock(&(_bridge->mutex));
        emac_lwip_l2b_entry_t *entry = get_bridge_entry(mac_dest);

        //Forward
        if(entry != 0) {
            emac_interface_t *emac = (emac_interface_t *)(entry->net->state);
            sys_mutex_unlock(&(_bridge->mutex));
            MBED_ASSERT(emac != 0);

            if((&(emac->netif) != net) ||
               ((emac->flags & EMAC_FLAGS_BROADCAST_TO_SELF) != 0)) {
                res = emac->ops->link_out(emac, buf);
            }
        }
        //Flood
        else {
            sys_mutex_unlock(&(_bridge->mutex));
            res = output_from_netif_to_netifs(net, buf);
        }

    }

    //Touch entry for source mac address
    sys_mutex_lock(&(_bridge->mutex));
    touch_bridge_entry(net, mac_src);
    sys_mutex_unlock(&(_bridge->mutex));

    emac_stack_mem_free(buf); // To match mem_ref above

    //Release buffer unless IP stack received message
    if(free_msg) {
        emac_stack_mem_free(buf);
    }

    return res;
}
