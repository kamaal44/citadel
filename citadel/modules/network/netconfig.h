/*
 * This module handles shared rooms, inter-Citadel mail, and outbound
 * mailing list processing.
 *
 * Copyright (c) 2000-2012 by the citadel.org team
 *
 *  This program is open source software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * ** NOTE **   A word on the S_NETCONFIGS semaphore:
 * This is a fairly high-level type of critical section.  It ensures that no
 * two threads work on the netconfigs files at the same time.  Since we do
 * so many things inside these, here are the rules:
 *  1. begin_critical_section(S_NETCONFIGS) *before* begin_ any others.
 *  2. Do *not* perform any I/O with the client during these sections.
 *
 */

typedef struct _nodeconf {
	int DeleteMe;
	StrBuf *NodeName;
	StrBuf *Secret;
	StrBuf *Host;
	StrBuf *Port;
}NodeConf;

typedef struct __NetMap {
	StrBuf *NodeName;
	time_t lastcontact;
	StrBuf *NextHop;
}NetMap;

HashList* load_ignetcfg(void);

HashList* read_network_map(void);
StrBuf *SerializeNetworkMap(HashList *Map);
void network_learn_topology(char *node, char *path, HashList *the_netmap, int *netmap_changed);

int is_valid_node(const StrBuf **nexthop,
		  const StrBuf **secret,
		  StrBuf *node,
		  HashList *IgnetCfg,
		  HashList *the_netmap);