/*-
 * Copyright (c) 2008, 2009, 2010
 *      The Board of Trustees of The Leland Stanford Junior University
 *
 * We are making the OpenFlow specification and associated documentation
 * (Software) available for public use and benefit with the expectation that
 * others will use, modify and enhance the Software and contribute those
 * enhancements back to the community. However, since we would like to make the
 * Software available for broadest use, with as few restrictions as possible
 * permission is hereby granted, free of charge, to any person obtaining a copy
 * of this Software to deal in the Software under the copyrights without
 * restriction, including without limitation the rights to use, copy, modify,
 * merge, publish, distribute, sublicense, and/or sell copies of the Software,
 * and to permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * The name and trademarks of copyright holder(s) may NOT be used in
 * advertising or publicity pertaining to the Software or any derivatives
 * without specific, written prior permission.
 */

#include <stdlib.h>
#include <arpa/inet.h>

#include "list.h"
#include "udatapath/crc32.h"
#include "udatapath/switch-flow.h"
#include "udatapath/table.h"
#include "reg_defines_openflow_switch.h"
#include "nf2.h"
#include "nf2util.h"
#include "hw_flow.h"
#include "nf2_drv.h"
#include "nf2_lib.h"
#include "debug.h"

#define DEFAULT_IFACE	"nf2c0"

#define MAX_INT_32	0xFFFFFFFF
#define PORT_BASE 1

#define VID_BITMASK 0x0FFF
#define PCP_BITSHIFT 13
#define PCP_BITMASK 0xE000
#define TOS_BITMASK 0xFC

struct list wildcard_free_list;
struct nf2_flow *exact_free_list[OPENFLOW_NF2_EXACT_TABLE_SIZE];

static uint32_t make_nw_wildcard(int);
static struct nf2_flow *get_free_exact(nf2_of_entry_wrap *);
static struct nf2_flow *get_free_wildcard(void);
static int is_action_forward_all(struct sw_flow *);
static void populate_action_output(nf2_of_action_wrap *, nf2_of_entry_wrap *,
				   uint8_t *);
static void populate_action_set_dl_src(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_dl_dst(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_nw_src(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_nw_dst(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_tp_src(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_tp_dst(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_nw_tos(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_vlan_vid(nf2_of_action_wrap *, uint8_t *);
static void populate_action_set_vlan_pcp(nf2_of_action_wrap *, uint8_t *);
static void populate_action_strip_vlan(nf2_of_action_wrap *);

int iface_chk_done = 0;
int net_iface = 0;

struct nf2device *
nf2_get_net_device(void)
{
	struct nf2device *dev;
        dev = calloc(1, sizeof(struct nf2device));
        dev->device_name = DEFAULT_IFACE;

	if (iface_chk_done) {
		dev->net_iface = net_iface;
	} else {
		if (check_iface(dev)) {
			iface_chk_done = 0;
			return NULL;
		} else {
			iface_chk_done = 1;
			net_iface = dev->net_iface;
		}
	}

        if (openDescriptor(dev)) {
                return NULL;
        }
        return dev;
}

void
nf2_free_net_device(struct nf2device *dev)
{
	if (dev == NULL){
		return;
	}

        closeDescriptor(dev);
	free(dev);
}

/* Checks to see if the actions requested by the flow are capable of being
 * done in the NF2 hardware. Returns 1 if yes, 0 for no.
 */
int
nf2_are_actions_supported(struct sw_flow *flow)
{
	struct sw_flow_actions *sfa;
	size_t actions_len;
	uint8_t *p;

	sfa = flow->sf_acts;
	actions_len = sfa->actions_len;
	p = (uint8_t *)sfa->actions;

	while (actions_len > 0) {
		struct ofp_action_header *ah = (struct ofp_action_header *)p;
		struct ofp_action_output *oa = (struct ofp_action_output *)p;
		size_t len = ntohs(ah->len);

		DBG_VERBOSE("Action Support Chk: Len of this action: %i\n",
			    len);
		DBG_VERBOSE("Action Support Chk: Len of actions    : %i\n",
			    actions_len);

		// All the modify actions are supported.
		// Each of them can be specified once otherwise overwritten.
		// Output action happens last.
		if (!(ntohs(ah->type) == OFPAT_OUTPUT
		      || ntohs(ah->type) == OFPAT_SET_DL_SRC
		      || ntohs(ah->type) == OFPAT_SET_DL_DST

		      || ntohs(ah->type) == OFPAT_SET_NW_SRC
		      || ntohs(ah->type) == OFPAT_SET_NW_DST

		      || ntohs(ah->type) == OFPAT_SET_NW_TOS

		      || ntohs(ah->type) == OFPAT_SET_TP_SRC
		      || ntohs(ah->type) == OFPAT_SET_TP_DST

		      || ntohs(ah->type) == OFPAT_SET_VLAN_VID
		      || ntohs(ah->type) == OFPAT_SET_VLAN_PCP
		      || ntohs(ah->type) == OFPAT_STRIP_VLAN)) {
			DBG_VERBOSE
				("Flow action type %#0x not supported in hardware\n",
				 ntohs(ah->type));
			return 0;
		}
		// Only support ports 1-4(incl. IN_PORT), ALL, FLOOD.
		// Let CONTROLLER/LOCAL fall through
		if ((ntohs(ah->type) == OFPAT_OUTPUT)
		    && (!((ntohs(oa->port) >= PORT_BASE)
			  && (ntohs(oa->port) <= MAX_IFACE))
			&& !(ntohs(oa->port) == OFPP_ALL)
			&& !(ntohs(oa->port) == OFPP_FLOOD)
			&& !(ntohs(oa->port) == OFPP_IN_PORT))) {
			DBG_VERBOSE
				("Flow action output port %#0x is not supported in hardware\n",
				 ntohs(oa->port));
			return 0;
		}
		p += len;
		actions_len -= len;
	}

	return 1;
}

/* Write all 0's out to an exact entry position. */
void
nf2_clear_of_exact(uint32_t pos)
{
	nf2_of_entry_wrap entry;
	nf2_of_action_wrap action;
	struct nf2device *dev = NULL;

	memset(&entry, 0, sizeof(nf2_of_entry_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	dev = nf2_get_net_device();
	if (dev == NULL) {
		DBG_ERROR("Could not open NetFPGA device\n");
		return;
	}
	nf2_write_of_exact(dev, pos, &entry, &action);
	nf2_free_net_device(dev);
}

/*
 * Write all 0's out to a wildcard entry position
 */
void
nf2_clear_of_wildcard(uint32_t pos)
{
	nf2_of_entry_wrap entry;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;
	struct nf2device *dev = NULL;

	memset(&entry, 0, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0, sizeof(nf2_of_mask_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	dev = nf2_get_net_device();
	if (dev == NULL) {
		DBG_ERROR("Could not open NetFPGA device\n");
		return;
	}
	nf2_write_of_wildcard(dev, pos, &entry, &mask, &action);
	nf2_free_net_device(dev);
}

int
nf2_init_exact_freelist(void)
{
	struct nf2_flow *sfw = NULL;
	int i;

	for (i = 0; i < (OPENFLOW_NF2_EXACT_TABLE_SIZE); ++i) {
		sfw = calloc(1, sizeof(struct nf2_flow));
		if (sfw == NULL) {
			return 1;
		}
		sfw->pos = i;
		sfw->type = NF2_TABLE_EXACT;
		nf2_add_free_exact(sfw);
		sfw = NULL;
	}

	return 0;
}

int
nf2_init_wildcard_freelist(void)
{
	struct nf2_flow *sfw = NULL;
	int i;
	list_init(&wildcard_free_list);

	for (i = 0; i < (OPENFLOW_WILDCARD_TABLE_SIZE - RESERVED_FOR_CPU2NETFPGA); ++i) {
		sfw = calloc(1, sizeof(struct nf2_flow));
		if (sfw == NULL) {
			return 1;
		}
		sfw->pos = i;
		sfw->type = NF2_TABLE_WILDCARD;
		nf2_add_free_wildcard(sfw);
		sfw = NULL;
	}

	return 0;
}

/* Called when the table is being deleted. */
void
nf2_destroy_exact_freelist(void)
{
	struct nf2_flow *sfw = NULL;
	int i;

	for (i = 0; i < (OPENFLOW_NF2_EXACT_TABLE_SIZE); ++i) {
		sfw = exact_free_list[i];
		if (sfw) {
			free(sfw);
		}
		sfw = NULL;
	}
}

/* Called when the table is being deleted. */
void
nf2_destroy_wildcard_freelist(void)
{
	struct nf2_flow *sfw = NULL;

	while (!list_is_empty(&wildcard_free_list)) {
		sfw = CONTAINER_OF(list_front(&wildcard_free_list), struct nf2_flow, node);
		list_remove(&sfw->node);
		free(sfw);
	}
}

/* Setup the wildcard table by adding static flows that will handle
 * misses by sending them up to the cpu ports, and handle packets coming
 * back down from the cpu by sending them out the corresponding port.
 */
int
nf2_write_static_wildcard(void)
{
	nf2_of_entry_wrap entry;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;
	int i;
	struct nf2device *dev;

	dev = nf2_get_net_device();
	if (dev == NULL)
		return 1;

	memset(&entry, 0x00, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0xFF, sizeof(nf2_of_mask_wrap));
	// Only non-wildcard section is the source port
	mask.entry.src_port = 0;
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	// write the catch all entries to send to the cpu
	for (i = 0; i < 4; ++i) {
		entry.entry.src_port = i * 2;
		action.action.forward_bitmask = 0x1 << ((i * 2) + 1);
		nf2_write_of_wildcard(dev, (OPENFLOW_WILDCARD_TABLE_SIZE - 4)
				      + i, &entry, &mask, &action);
	}

	// write the entries to send out packets coming from the cpu
	for (i = 0; i < 4; ++i) {
		entry.entry.src_port = (i * 2) + 1;
		action.action.forward_bitmask = 0x1 << (i * 2);
		nf2_write_of_wildcard(dev, (OPENFLOW_WILDCARD_TABLE_SIZE - 8)
				      + i, &entry, &mask, &action);
	}

	nf2_free_net_device(dev);
	return 0;
}

/* Populate a nf2_of_entry_wrap with entries from a struct sw_flow. */
void
nf2_populate_of_entry(nf2_of_entry_wrap *key, struct sw_flow *flow)
{
	int vlan_vid;
	int vlan_pcp;
	int i;

	key->entry.transp_dst = ntohs(flow->key.flow.tp_dst);
	key->entry.transp_src = ntohs(flow->key.flow.tp_src);
	key->entry.ip_proto = flow->key.flow.nw_proto;
	key->entry.ip_dst = ntohl(flow->key.flow.nw_dst);
	key->entry.ip_src = ntohl(flow->key.flow.nw_src);
	key->entry.eth_type = ntohs(flow->key.flow.dl_type);
	// Blame Jad for applying endian'ness to character arrays
	for (i = 0; i < 6; ++i) {
		key->entry.eth_dst[i] = flow->key.flow.dl_dst[5 - i];
	}
	for (i = 0; i < 6; ++i) {
		key->entry.eth_src[i] = flow->key.flow.dl_src[5 - i];
	}
	key->entry.src_port = (ntohs(flow->key.flow.in_port) - PORT_BASE) * 2;
	key->entry.ip_tos = TOS_BITMASK & flow->key.flow.nw_tos;
	if (ntohs(flow->key.flow.dl_vlan) == 0xffff) {
		key->entry.vlan_id = 0xffff;
	} else {
		vlan_vid = VID_BITMASK & ntohs(flow->key.flow.dl_vlan);
		vlan_pcp = PCP_BITMASK
			& ((uint16_t)(flow->key.flow.dl_vlan_pcp) << PCP_BITSHIFT);
		key->entry.vlan_id = vlan_pcp | vlan_vid;
	}
}

static uint32_t
make_nw_wildcard(int n_wild_bits)
{
	n_wild_bits &= (1u << OFPFW_NW_SRC_BITS) - 1;
	return n_wild_bits < 32 ? ((1u << n_wild_bits) - 1) : 0xFFFFFFFF;
}

/* Populate a nf2_of_mask_wrap with entries from a struct sw_flow's wildcards. */
void
nf2_populate_of_mask(nf2_of_mask_wrap *mask, struct sw_flow *flow)
{
	int vlan_vid = 0;
	int vlan_pcp = 0;
	int i;

	if (OFPFW_IN_PORT & flow->key.wildcards) {
		mask->entry.src_port = 0xFF;
	}
	if (OFPFW_DL_VLAN & flow->key.wildcards) {
		vlan_vid = VID_BITMASK;
	}
	if (OFPFW_DL_VLAN_PCP & flow->key.wildcards) {
		vlan_pcp = 0xF000;
	}
	mask->entry.vlan_id = vlan_pcp | vlan_vid;
	if (OFPFW_DL_SRC & flow->key.wildcards) {
		for (i = 0; i < 6; ++i) {
			mask->entry.eth_src[i] = 0xFF;
		}
	}
	if (OFPFW_DL_DST & flow->key.wildcards) {
		for (i = 0; i < 6; ++i) {
			mask->entry.eth_dst[i] = 0xFF;
		}
	}
	if (OFPFW_DL_TYPE & flow->key.wildcards)
		mask->entry.eth_type = 0xFFFF;
	if ((OFPFW_NW_SRC_ALL & flow->key.wildcards)
	    || (OFPFW_NW_SRC_MASK & flow->key.wildcards))
		mask->entry.ip_src = make_nw_wildcard
			(flow->key.wildcards >> OFPFW_NW_SRC_SHIFT);
	if ((OFPFW_NW_DST_ALL & flow->key.wildcards)
	    || (OFPFW_NW_DST_MASK & flow->key.wildcards))
		mask->entry.ip_dst = make_nw_wildcard
			(flow->key.wildcards >> OFPFW_NW_DST_SHIFT);
	if (OFPFW_NW_PROTO & flow->key.wildcards)
		mask->entry.ip_proto = 0xFF;
	if (OFPFW_TP_SRC & flow->key.wildcards)
		mask->entry.transp_src = 0xFFFF;
	if (OFPFW_TP_DST & flow->key.wildcards)
		mask->entry.transp_dst = 0xFFFF;
	if (OFPFW_NW_TOS & flow->key.wildcards)
		mask->entry.ip_tos = TOS_BITMASK;

	mask->entry.pad = 0x00;
}

static void
populate_action_output(nf2_of_action_wrap *action, nf2_of_entry_wrap *entry,
		       uint8_t *flowact)
{
	uint16_t port = 0;
	struct ofp_action_output *actout = (struct ofp_action_output *)flowact;
	int i;

	port = ntohs(actout->port);
	DBG_VERBOSE("Action Type: %i Output Port: %i\n",
		    ntohs(actout->type), port);

	if ((port >= PORT_BASE) && (port <= MAX_IFACE)) {
		// Bitmask for output port(s), evens are phys odds cpu
		action->action.forward_bitmask
			|= (1 << ((port - PORT_BASE) * 2));
		DBG_VERBOSE("Output Port: %i Forward Bitmask: %x\n",
			    port, action->action.forward_bitmask);
	} else if (port == OFPP_IN_PORT) {
		// Send out to input port
		action->action.forward_bitmask
			|= (1 << (entry->entry.src_port));
		DBG_VERBOSE("Output Port = Input Port  Forward Bitmask: %x\n",
			    action->action.forward_bitmask);
	} else if (port == OFPP_ALL || port == OFPP_FLOOD) {
		// Send out all ports except the source
		for (i = 0; i < 4; ++i) {
			if ((i * 2) != entry->entry.src_port) {
				// Bitmask for output port(s), evens are
				// phys odds cpu
				action->action.forward_bitmask
					|= (1 << (i * 2));
				DBG_VERBOSE
					("Output Port: %i Forward Bitmask: %x\n",
					 port, action->action.forward_bitmask);
			}
		}
	}
}

static void
populate_action_set_dl_src(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_dl_addr *actdl = (struct ofp_action_dl_addr *)flowact;
	int i;

	for (i = 0; i < 6; ++i) {
		action->action.eth_src[5 - i] = actdl->dl_addr[i];
	}
	action->action.nf2_action_flag |= (1 << OFPAT_SET_DL_SRC);
}

static void
populate_action_set_dl_dst(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_dl_addr *actdl = (struct ofp_action_dl_addr *)flowact;
	int i;

	for (i = 0; i < 6; ++i) {
		action->action.eth_dst[5 - i] = actdl->dl_addr[i];
	}
	action->action.nf2_action_flag |= (1 << OFPAT_SET_DL_DST);
}

static void
populate_action_set_nw_src(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_nw_addr  *actnw = (struct ofp_action_nw_addr *)flowact;
	action->action.ip_src = ntohl(actnw->nw_addr);
	action->action.nf2_action_flag |= (1 << OFPAT_SET_NW_SRC);
}

static void
populate_action_set_nw_dst(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_nw_addr  *actnw = (struct ofp_action_nw_addr *)flowact;
	action->action.ip_dst = ntohl(actnw->nw_addr);
	action->action.nf2_action_flag |= (1 << OFPAT_SET_NW_DST);
}

static void
populate_action_set_nw_tos(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_nw_tos *actnwtos = (struct ofp_action_nw_tos *)flowact;
	action->action.ip_tos = actnwtos->nw_tos;
	action->action.nf2_action_flag |= (1 << OFPAT_SET_NW_TOS);
}

static void
populate_action_set_tp_src(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_tp_port  *acttp = (struct ofp_action_tp_port *)flowact;
	action->action.transp_src = ntohs(acttp->tp_port);
	action->action.nf2_action_flag |= (1 << OFPAT_SET_TP_SRC);
}

static void
populate_action_set_tp_dst(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_tp_port  *acttp = (struct ofp_action_tp_port *)flowact;
	action->action.transp_dst = ntohs(acttp->tp_port);
	action->action.nf2_action_flag |= (1 << OFPAT_SET_TP_DST);
}

static void
populate_action_set_vlan_vid(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_vlan_vid *actvlan = (struct ofp_action_vlan_vid *)flowact;
	action->action.vlan_id = ntohs(actvlan->vlan_vid) & VID_BITMASK;
	action->action.nf2_action_flag |= (1 << OFPAT_SET_VLAN_VID);
}

static void
populate_action_set_vlan_pcp(nf2_of_action_wrap *action, uint8_t *flowact)
{
	struct ofp_action_vlan_pcp *actvlan = (struct ofp_action_vlan_pcp *)flowact;
	action->action.vlan_pcp = actvlan->vlan_pcp;
	action->action.nf2_action_flag |= (1 << OFPAT_SET_VLAN_PCP);
}

static void
populate_action_strip_vlan(nf2_of_action_wrap *action)
{
	action->action.nf2_action_flag |= (1 << OFPAT_STRIP_VLAN);
}

/* Populate an nf2_of_action_wrap. */
void
nf2_populate_of_action(nf2_of_action_wrap *action, nf2_of_entry_wrap *entry,
		       struct sw_flow *flow)
{
	struct sw_flow_actions *sfa;
	size_t actions_len;
	uint8_t *p;

	sfa = flow->sf_acts;
	actions_len = sfa->actions_len;
	p = (uint8_t *)sfa->actions;

	// zero it out for now
	memset(action, 0, sizeof(nf2_of_action_wrap));
	action->action.nf2_action_flag = 0;

	while (actions_len > 0) {
		struct ofp_action_header *acth = (struct ofp_action_header *)p;
		size_t len = ntohs(acth->len);

		DBG_VERBOSE("Action Populate: Len of this action: %i\n", len);
		DBG_VERBOSE("Action Populate: Len of actions    : %i\n",
			    actions_len);

		if (acth->type == htons(OFPAT_OUTPUT)) {
			populate_action_output(action, entry, p);
		} else if (acth->type == htons(OFPAT_SET_DL_SRC)) {
			populate_action_set_dl_src(action, p);
		} else if (acth->type == htons(OFPAT_SET_DL_DST)) {
			populate_action_set_dl_dst(action, p);
		} else if (acth->type == htons(OFPAT_SET_NW_SRC)) {
			populate_action_set_nw_src(action, p);
		} else if (acth->type == htons(OFPAT_SET_NW_DST)) {
			populate_action_set_nw_dst(action, p);
		} else if (acth->type == htons(OFPAT_SET_NW_TOS)) {
			populate_action_set_nw_tos(action, p);
		} else if (acth->type == htons(OFPAT_SET_TP_SRC)) {
			populate_action_set_tp_src(action, p);
		} else if (acth->type == htons(OFPAT_SET_TP_DST)) {
			populate_action_set_tp_dst(action, p);
		} else if (acth->type == htons(OFPAT_SET_VLAN_VID)) {
			populate_action_set_vlan_vid(action, p);
		} else if (acth->type == htons(OFPAT_SET_VLAN_PCP)) {
			populate_action_set_vlan_pcp(action, p);
		} else if (acth->type == htons(OFPAT_STRIP_VLAN)) {
			populate_action_strip_vlan(action);
		}

		p += len;
		actions_len -= len;
	}
}

/* Add a free hardware entry back to the exact pool. */
void
nf2_add_free_exact(struct nf2_flow *sfw)
{
	// clear the node entry
	list_init(&sfw->node);

	// Critical section, adding to the actual list
	exact_free_list[sfw->pos] = sfw;
}

/* Add a free hardware entry back to the wildcard pool. */
void
nf2_add_free_wildcard(struct nf2_flow *sfw)
{
	// clear the hw values
	sfw->hw_packet_count = 0;
	sfw->hw_byte_count = 0;

	// Critical section, adding to the actual list
	list_insert(&wildcard_free_list, &sfw->node);
}

/* Hashes the entry to find where it should exist in the exact table
 * returns NULL on failure
 */
static struct nf2_flow *
get_free_exact(nf2_of_entry_wrap *entry)
{
	unsigned int poly1 = 0x04C11DB7;
	unsigned int poly2 = 0x1EDC6F41;
	struct nf2_flow *sfw = NULL;
	unsigned int hash = 0x0;
	unsigned int index = 0x0;
	struct crc32 crc;

	crc32_init(&crc, poly1);
	hash = crc32_calculate(&crc, entry, sizeof(nf2_of_entry_wrap));

	// the bottom 15 bits of hash == the index into the table
	index = 0x7FFF & hash;

	// if this index is free, grab it
	sfw = exact_free_list[index];
	exact_free_list[index] = NULL;

	if (sfw != NULL) {
		return sfw;
	}
	// try the second index
	crc32_init(&crc, poly2);
	hash = crc32_calculate(&crc, entry, sizeof(nf2_of_entry_wrap));
	// the bottom 15 bits of hash == the index into the table
	index = 0x7FFF & hash;

	// if this index is free, grab it
	sfw = exact_free_list[index];
	exact_free_list[index] = NULL;

	// return whether its good or not
	return sfw;
}

/* Get the first free position in the wildcard hardware table
 * to write into.
 */
static struct nf2_flow *
get_free_wildcard(void)
{
	struct nf2_flow *sfw = NULL;

	// Critical section, pulling the first available from the list
	if (list_is_empty(&wildcard_free_list)) {
		// empty :(
		sfw = NULL;
	} else {
		sfw = CONTAINER_OF(list_front(&wildcard_free_list), struct nf2_flow, node);
		list_remove(&sfw->node);
		list_init(&sfw->node);
	}

	return sfw;
}

/* Retrieves the type of table this flow should go into. */
int
nf2_get_table_type(struct sw_flow *flow)
{
	if (flow->key.wildcards != 0) {
		DBG_VERBOSE("--- TABLE TYPE: WILDCARD ---\n");
		return NF2_TABLE_WILDCARD;
	} else {
		DBG_VERBOSE("--- TABLE TYPE: EXACT ---\n");
		return NF2_TABLE_EXACT;
	}
}

/* Returns 1 if this flow contains an action outputting to all ports except
 * input port, 0 otherwise. We support OFPP_ALL and OFPP_FLOOD actions, however
 * since we do not perform the spanning tree protocol (STP) then OFPP_FLOOD is
 * equivalent to OFPP_ALL.
 */
static int
is_action_forward_all(struct sw_flow *flow)
{
	struct sw_flow_actions *sfa;
	size_t actions_len;
	uint8_t *p;

	sfa = flow->sf_acts;
	actions_len = sfa->actions_len;
	p = (uint8_t *)sfa->actions;

	while (actions_len > 0) {
		struct ofp_action_header *ah = (struct ofp_action_header *)p;
		struct ofp_action_output *oa = (struct ofp_action_output *)p;
		size_t len = ntohs(ah->len);

		DBG_VERBOSE("Fwd Action Chk: Action type: %x\n",
			    ntohs(ah->type));
		DBG_VERBOSE("Fwd Action Chk: Output port: %x\n",
			    ntohs(oa->port));
		DBG_VERBOSE("Fwd Action Chk: Len of this action: %i\n", len);
		DBG_VERBOSE("Fwd Action Chk: Len of actions    : %i\n",
			    actions_len);
		// Currently only support the output port(s) action
		if (ntohs(ah->type) == OFPAT_OUTPUT
		    && (ntohs(oa->port) == OFPP_ALL
			|| ntohs(oa->port) == OFPP_FLOOD)) {
			return 1;
		}
		p += len;
		actions_len -= len;
	}

	return 0;
}

/* Attempts to build and write the flow to hardware.
 * Returns 0 on success, 1 on failure.
 */
int
nf2_build_and_write_flow(struct sw_flow *flow)
{
	struct nf2_flow *sfw = NULL;
	struct nf2_flow *sfw_next = NULL;
	struct nf2device *dev;
	int num_entries = 0;
	int i, table_type;
	nf2_of_entry_wrap key;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;

	memset(&key, 0, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0, sizeof(nf2_of_mask_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	dev = nf2_get_net_device();
	if (dev == NULL) {
		return 1;
	}

	table_type = nf2_get_table_type(flow);
	switch (table_type) {
	default:
		break;

	case NF2_TABLE_EXACT:
		DBG_VERBOSE("---Exact Entry---\n");
		nf2_populate_of_entry(&key, flow);
		nf2_populate_of_action(&action, &key, flow);
		sfw = get_free_exact(&key);
		if (sfw == NULL) {
			DBG_VERBOSE
				("Collision getting free exact match entry\n");
			// collision
			nf2_free_net_device(dev);
			return 1;
		}
		// set the active bit on this entry
		key.entry.pad = 0x80;
		nf2_write_of_exact(dev, sfw->pos, &key, &action);
		flow->private = (void *)sfw;
		break;

	case NF2_TABLE_WILDCARD:
		DBG_VERBOSE("---Wildcard Entry---\n");
		// if action is all out and source port is wildcarded
		if ((is_action_forward_all(flow)) &&
		    (flow->key.wildcards & OFPFW_IN_PORT)) {
			DBG_VERBOSE("Grab four wildcard tables\n");
			if (!(sfw = get_free_wildcard())) {
				DBG_VERBOSE("No free wildcard entries found.");
				// no free entries
				nf2_free_net_device(dev);
				return 1;
			}
			// try to get 3 more positions
			for (i = 0; i < 3; ++i) {
				if (!(sfw_next = get_free_wildcard())) {
					break;
				}
				list_insert(&sfw->node, &sfw_next->node);
				++num_entries;
			}

			if (num_entries < 3) {
				// failed to get enough entries, return them and exit
				nf2_delete_private((void *)sfw);
				nf2_free_net_device(dev);
				return 1;
			}

			nf2_populate_of_entry(&key, flow);
			nf2_populate_of_mask(&mask, flow);

			// set first entry's src port to 0, remove wildcard mask on src
			key.entry.src_port = 0;
			mask.entry.src_port = 0;
			nf2_populate_of_action(&action, &key, flow);
			nf2_write_of_wildcard(dev, sfw->pos, &key, &mask,
					      &action);

			i = 1;
			sfw_next = CONTAINER_OF(list_front(&sfw->node),
					      struct nf2_flow, node);
			// walk through and write the remaining 3 entries
			while (sfw_next != sfw) {
				key.entry.src_port = i * 2;
				nf2_populate_of_action(&action, &key, flow);
				nf2_write_of_wildcard(dev, sfw_next->pos, &key,
						      &mask, &action);
				sfw_next = CONTAINER_OF(list_front(&sfw_next->node),
						      struct nf2_flow, node);
				++i;
			}
			flow->private = (void *)sfw;
		} else {
			/* Get a free position here, and write to it */
			if ((sfw = get_free_wildcard())) {
				nf2_populate_of_entry(&key, flow);
				nf2_populate_of_mask(&mask, flow);
				nf2_populate_of_action(&action, &key, flow);
				if (nf2_write_of_wildcard
				    (dev, sfw->pos, &key, &mask, &action)) {
					// failure writing to hardware
					nf2_add_free_wildcard(sfw);
					DBG_VERBOSE
						("Failure writing to hardware\n");
					nf2_free_net_device(dev);
					return 1;
				} else {
					// success writing to hardware, store the position
					flow->private = (void *)sfw;
				}
			} else {
				// hardware is full, return 0
				DBG_VERBOSE("No free wildcard entries found.");
				nf2_free_net_device(dev);
				return 1;
			}
		}
		break;
	}

	nf2_free_net_device(dev);
	return 0;
}

void
nf2_delete_private(void *private)
{
	struct nf2_flow *sfw = (struct nf2_flow *)private;
	struct nf2_flow *sfw_next;
	struct list *next;

	switch (sfw->type) {
	default:
		break;

	case NF2_TABLE_EXACT:
		nf2_clear_of_exact(sfw->pos);
		nf2_add_free_exact(sfw);
		break;

	case NF2_TABLE_WILDCARD:
		while (!list_is_empty(&sfw->node)) {
			next = sfw->node.next;
			sfw_next = CONTAINER_OF(list_front(&sfw->node), struct nf2_flow, node);
			list_remove(&sfw_next->node);
			list_init(&sfw_next->node);
			// Immediately zero out the entry in hardware
			nf2_clear_of_wildcard(sfw_next->pos);
			// add it back to the pool
			nf2_add_free_wildcard(sfw_next);
		}
		// zero the core entry
		nf2_clear_of_wildcard(sfw->pos);
		// add back the core entry
		nf2_add_free_wildcard(sfw);
		break;
	}
}

int
nf2_modify_acts(struct sw_flow *flow)
{
	struct nf2_flow *sfw = (struct nf2_flow *)flow->private;
	struct nf2device *dev;
	nf2_of_entry_wrap key;
	nf2_of_mask_wrap mask;
	nf2_of_action_wrap action;

	memset(&key, 0, sizeof(nf2_of_entry_wrap));
	memset(&mask, 0, sizeof(nf2_of_mask_wrap));
	memset(&action, 0, sizeof(nf2_of_action_wrap));

	dev = nf2_get_net_device();
	if (dev == NULL)
		return 0;

	switch (sfw->type) {
	default:
		break;

	case NF2_TABLE_EXACT:
		nf2_populate_of_entry(&key, flow);
		nf2_populate_of_action(&action, &key, flow);
		key.entry.pad = 0x80;
		nf2_modify_write_of_exact(dev, sfw->pos, &action);
		break;

	case NF2_TABLE_WILDCARD:
		if (flow->key.wildcards & OFPFW_IN_PORT) {
			nf2_free_net_device(dev);
			return 0;
		}
		nf2_populate_of_entry(&key, flow);
		nf2_populate_of_mask(&mask, flow);
		nf2_populate_of_action(&action, &key, flow);
		nf2_modify_write_of_wildcard(dev, sfw->pos,
					     &key, &mask, &action);
		break;
	}

	nf2_free_net_device(dev);
	return 1;
}

uint64_t
nf2_get_packet_count(struct nf2device *dev, struct nf2_flow *sfw)
{
	uint32_t count = 0;
	uint32_t hw_count = 0;
	uint64_t total = 0;
	struct nf2_flow *sfw_next = NULL;

	switch (sfw->type) {
	default:
		break;

	case NF2_TABLE_EXACT:
		// Get delta value
		total = nf2_get_exact_packet_count(dev, sfw->pos);
		break;

	case NF2_TABLE_WILDCARD:
		sfw_next = sfw;
		do {
			// Get sum value
			hw_count = nf2_get_wildcard_packet_count(dev,
								 sfw_next->pos);
			if (hw_count >= sfw_next->hw_packet_count) {
				count = hw_count - sfw_next->hw_packet_count;
				sfw_next->hw_packet_count = hw_count;
			} else {
				// wrapping occurred
				count = (MAX_INT_32 - sfw_next->hw_packet_count)
					+ hw_count;
				sfw_next->hw_packet_count = hw_count;
			}
			total += count;

			if(!list_is_empty(&sfw_next->node)){
				sfw_next = CONTAINER_OF(list_front(&sfw_next->node),
						      struct nf2_flow, node);
			}
		} while (sfw_next != sfw);
		break;
	}

	return total;
}

uint64_t
nf2_get_byte_count(struct nf2device *dev, struct nf2_flow *sfw)
{
	uint32_t count = 0;
	uint32_t hw_count = 0;
	uint64_t total = 0;
	struct nf2_flow *sfw_next = NULL;

	switch (sfw->type) {
	default:
		break;

	case NF2_TABLE_EXACT:
		// Get delta value
		total = nf2_get_exact_byte_count(dev, sfw->pos);
		break;

	case NF2_TABLE_WILDCARD:
		sfw_next = sfw;
		do {
			// Get sum value
			hw_count = nf2_get_wildcard_byte_count(dev,
							       sfw_next->pos);
			if (hw_count >= sfw_next->hw_byte_count) {
				count = hw_count - sfw_next->hw_byte_count;
				sfw_next->hw_byte_count = hw_count;
			} else {
				// wrapping occurred
				count = (MAX_INT_32 - sfw_next->hw_byte_count)
					+ hw_count;
				sfw_next->hw_byte_count = hw_count;
			}

			total += count;

			if(!list_is_empty(&sfw_next->node)) {
				sfw_next = CONTAINER_OF(list_front(&sfw_next->node),
						      struct nf2_flow, node);
			}
		} while (sfw_next != sfw);
		break;
	}

	return total;
}
